import Foundation

/// Audio settings shared between the UI and the audio threads (plain values: a race here is harmless).
final class Settings {
    static let shared = Settings()
    var gainDb = 0
    var nsStr = 70
    var ns = true
    var agc = true
    var boost = false
    var micOn = true
    var sysOn = false
    var rev = false
    var echo = true
    var diar = false
    var sens = 50
    var sysLastSpeechMs: Int64 = 0
}

struct Fx { var hx: Float = 0, hy: Float = 0, noise: Float = 40, gate: Float = 1, agc: Float = 1, prevG: Float = 1 }

/// High-pass (rumble) + adaptive noise gate + manual gain / boost / automatic gain control.
/// `mic` is false for system audio: it is clean digital sound, so gain / boost / noise gate are not applied to it.
func processChunk(_ fx: inout Fx, _ s: inout [Int16], mic: Bool) {
    let cfg = Settings.shared
    let B = 160  // 10 ms blocks
    let ns = mic && cfg.ns
    let agcOn = cfg.agc
    let st = Float(cfg.nsStr) / 100
    let baseLin: Float = mic ? powf(10, Float(cfg.gainDb + (cfg.boost ? 12 : 0)) / 20) : 1
    var v = [Float](repeating: 0, count: B)
    let n = s.count
    var o = 0
    while o < n {
        let m = min(B, n - o)
        var acc: Double = 0
        for i in 0..<m {
            var x = Float(s[o + i])
            if ns { let y: Float = 0.97 * (fx.hy + x - fx.hx); fx.hx = x; fx.hy = y; x = y }
            v[i] = x
            acc += Double(x) * Double(x)
        }
        let r = Float((acc / Double(m)).squareRoot())
        // noise floor: follows dips quickly, rises slowly (and is capped) so speech can't become "noise"
        fx.noise = r < fx.noise ? fx.noise * 0.8 + r * 0.2 : fx.noise * 1.0003
        fx.noise = min(800, max(5, fx.noise))
        var gt: Float = 1
        if ns {
            let lo = fx.noise * 1.6, hi = fx.noise * 2.8, depth = st * 0.97
            let k: Float = r <= lo ? 0 : (r >= hi ? 1 : (r - lo) / (hi - lo))
            gt = (1 - depth) + depth * k
        }
        fx.gate += (gt - fx.gate) * (gt > fx.gate ? 0.6 : 0.08)
        if agcOn {
            if r > fx.noise * 2.8 {   // adapt only while somebody is actually speaking
                let want = min(31, max(0.3, 2500 / max(r * baseLin, 1)))
                fx.agc += (want - fx.agc) * 0.01
            }
        } else { fx.agc += (1 - fx.agc) * 0.05 }
        let g = baseLin * fx.agc * fx.gate
        for i in 0..<m {
            let gi = fx.prevG + (g - fx.prevG) * Float(i + 1) / Float(m)   // ramp: no clicks between blocks
            let y = v[i] * gi
            s[o + i] = Int16(max(-32000, min(32000, y)))
        }
        fx.prevG = g
        o += B
    }
}

/// One audio channel (0 = microphone "You", 1 = system audio "Them"): processing, energy speech detector
/// and framing towards the worker. Feed it 100 ms chunks of 16 kHz mono Int16.
final class Channel {
    let id: Int
    var fx = Fx()
    private var pre: [[Int16]] = []
    private var hist: [Double] = []
    private var histPos = 0
    private var speaking = false, suppressed = false
    private var silence = 0, total = 0, loud = 0, guardN = 0, relLoud = 0

    var onAudio: ((Int, [Int16]) -> Void)?
    var onEnd: ((Int) -> Void)?
    var onLevel: ((Int, Double, Bool) -> Void)?

    init(_ i: Int) { id = i }

    func feed(_ input: [Int16]) {
        let PREROLL = 3          // chunks kept before speech start
        let HANG = 7             // 0.7 s of silence ends an utterance
        let MAXC = 200           // 20 s hard cap
        let HIST = 150
        var s = input
        processChunk(&fx, &s, mic: id == 0)
        var acc = 0.0
        for x in s { acc += Double(x) * Double(x) }
        let rms = s.isEmpty ? 0 : (acc / Double(s.count)).squareRoot()
        onLevel?(id, min(100, rms / 12), speaking)

        // Noise floor = low percentile of the last 15 s of levels, so quiet speech can never drag the threshold up.
        if hist.count < HIST { hist.append(rms) } else { hist[histPos % HIST] = rms; histPos += 1 }
        let sorted = hist.sorted()
        let floorRms = sorted[sorted.count * 15 / 100]
        let thr = min(700, max(80, floorRms * 3))
        let isSpeech = rms > (speaking ? thr * 0.6 : thr)   // hysteresis
        let cfg = Settings.shared
        let now = nowMs()
        if id == 1 && isSpeech { cfg.sysLastSpeechMs = now }

        pre.append(s)
        if pre.count > PREROLL { pre.removeFirst() }
        // Echo filter: with loudspeakers the microphone hears the other side too. While the system audio carries
        // speech, a microphone utterance is held back; once the other side is silent and you keep talking it is released.
        let echoNow = id == 0 && cfg.echo && cfg.sysOn && now - cfg.sysLastSpeechMs < 650
        if !speaking {
            loud = isSpeech ? loud + 1 : 0
            if loud >= 2 {   // two loud chunks in a row = speech start
                speaking = true; silence = 0; total = 0; guardN = 0
                suppressed = echoNow
                if !suppressed { for c in pre { onAudio?(id, c); total += 1 } }
                pre.removeAll()
            }
        } else {
            if echoNow && !suppressed { onEnd?(id); suppressed = true }
            relLoud = (suppressed && !echoNow && isSpeech) ? relLoud + 1 : 0
            if suppressed && relLoud >= 2 {
                suppressed = false; silence = 0; total = 0; relLoud = 0
                for c in pre { onAudio?(id, c); total += 1 }
                pre.removeAll()
            } else if !suppressed {
                onAudio?(id, s)
                total += 1
            }
            silence = isSpeech ? 0 : silence + 1
            guardN += 1
            if silence >= HANG || guardN >= MAXC + 40 {
                if !suppressed { onEnd?(id) }
                speaking = false; suppressed = false; loud = 0
            }
        }
    }

    func flush() {
        if speaking && !suppressed { onEnd?(id) }
        speaking = false; suppressed = false
    }
}

import Foundation
import AVFoundation
import CoreAudio
import AudioToolbox
import ScreenCaptureKit
import CoreMedia

struct InputDevice { let id: AudioDeviceID; let name: String }

/// Microphones known to Core Audio (devices with input channels).
func listInputDevices() -> [InputDevice] {
    let sys = AudioObjectID(kAudioObjectSystemObject)
    var addr = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyDevices,
                                          mScope: kAudioObjectPropertyScopeGlobal,
                                          mElement: kAudioObjectPropertyElementMain)
    var size: UInt32 = 0
    guard AudioObjectGetPropertyDataSize(sys, &addr, 0, nil, &size) == noErr else { return [] }
    var ids = [AudioDeviceID](repeating: 0, count: Int(size) / MemoryLayout<AudioDeviceID>.size)
    guard AudioObjectGetPropertyData(sys, &addr, 0, nil, &size, &ids) == noErr else { return [] }
    var result: [InputDevice] = []
    for d in ids {
        var cfg = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreamConfiguration,
                                             mScope: kAudioDevicePropertyScopeInput,
                                             mElement: kAudioObjectPropertyElementMain)
        var sz: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(d, &cfg, 0, nil, &sz) == noErr, sz > 0 else { continue }
        let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(sz), alignment: 16)
        defer { raw.deallocate() }
        guard AudioObjectGetPropertyData(d, &cfg, 0, nil, &sz, raw) == noErr else { continue }
        let list = UnsafeMutableAudioBufferListPointer(raw.assumingMemoryBound(to: AudioBufferList.self))
        var chans = 0
        for b in list { chans += Int(b.mNumberChannels) }
        if chans == 0 { continue }
        var nameAddr = AudioObjectPropertyAddress(mSelector: kAudioObjectPropertyName,
                                                  mScope: kAudioObjectPropertyScopeGlobal,
                                                  mElement: kAudioObjectPropertyElementMain)
        var nameRef: Unmanaged<CFString>?
        var nsz = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        let st = AudioObjectGetPropertyData(d, &nameAddr, 0, nil, &nsz, &nameRef)
        let name = (st == noErr ? nameRef?.takeRetainedValue() as String? : nil) ?? "Input device \(d)"
        result.append(InputDevice(id: d, name: name))
    }
    return result
}

/// Chunks arbitrary-length Float samples into 100 ms Int16 blocks.
final class Chunker {
    private var buf: [Int16] = []
    private let size = 1600
    private let emit: ([Int16]) -> Void
    init(emit: @escaping ([Int16]) -> Void) { self.emit = emit }
    func push(_ p: UnsafeBufferPointer<Float>) {
        for x in p { buf.append(Int16(max(-1, min(1, x)) * 32767)) }
        while buf.count >= size {
            emit(Array(buf[0..<size]))
            buf.removeFirst(size)
        }
    }
}

/// Channel 0: the microphone through AVAudioEngine, converted to 16 kHz mono.
final class MicCapture {
    private let engine = AVAudioEngine()
    private var chunker: Chunker?
    private(set) var running = false

    func start(device: AudioDeviceID?, onChunk: @escaping ([Int16]) -> Void) throws {
        let input = engine.inputNode
        if let dev = device, dev != 0, let unit = input.audioUnit {
            var id = dev
            AudioUnitSetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0,
                                 &id, UInt32(MemoryLayout<AudioDeviceID>.size))
        }
        let inFmt = input.outputFormat(forBus: 0)
        guard inFmt.sampleRate > 0, let outFmt = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false),
              let conv = AVAudioConverter(from: inFmt, to: outFmt) else {
            throw BootstrapError.message("The microphone format is not supported.")
        }
        let chunker = Chunker(emit: onChunk)
        self.chunker = chunker
        input.installTap(onBus: 0, bufferSize: 4096, format: inFmt) { buffer, _ in
            let ratio = 16000.0 / inFmt.sampleRate
            let cap = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 64
            guard let out = AVAudioPCMBuffer(pcmFormat: outFmt, frameCapacity: cap) else { return }
            var supplied = false
            var err: NSError?
            conv.convert(to: out, error: &err) { _, status in
                if supplied { status.pointee = .noDataNow; return nil }
                supplied = true
                status.pointee = .haveData
                return buffer
            }
            guard err == nil, let data = out.floatChannelData?[0] else { return }
            chunker.push(UnsafeBufferPointer(start: data, count: Int(out.frameLength)))
        }
        engine.prepare()
        try engine.start()
        running = true
    }

    func stop() {
        guard running else { return }
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        running = false
    }
}

/// Channel 1: everything the Mac plays (the other side of a call), via ScreenCaptureKit audio capture (macOS 13+).
/// Needs the "Screen Recording" permission; no virtual audio driver is required.
final class SystemCapture: NSObject, SCStreamOutput, SCStreamDelegate {
    private var stream: SCStream?
    private var chunker: Chunker?
    private let queue = DispatchQueue(label: "voicerecognition.systemaudio")
    var onStop: ((String) -> Void)?

    static func hasPermission() -> Bool { CGPreflightScreenCaptureAccess() }
    static func requestPermission() { _ = CGRequestScreenCaptureAccess() }

    func start(onChunk: @escaping ([Int16]) -> Void) async throws {
        let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: false)
        guard let display = content.displays.first else { throw BootstrapError.message("No display found for audio capture.") }
        let filter = SCContentFilter(display: display, excludingApplications: [], exceptingWindows: [])
        let cfg = SCStreamConfiguration()
        cfg.capturesAudio = true
        cfg.sampleRate = 16000
        cfg.channelCount = 1
        cfg.excludesCurrentProcessAudio = true
        cfg.width = 2                      // the picture is not used: keep it as small and rare as possible
        cfg.height = 2
        cfg.minimumFrameInterval = CMTime(value: 1, timescale: 1)
        chunker = Chunker(emit: onChunk)
        let s = SCStream(filter: filter, configuration: cfg, delegate: self)
        try s.addStreamOutput(self, type: .audio, sampleHandlerQueue: queue)
        try await s.startCapture()
        stream = s
    }

    func stop() {
        guard let s = stream else { return }
        stream = nil
        Task { try? await s.stopCapture() }
    }

    func stream(_ stream: SCStream, didOutputSampleBuffer sampleBuffer: CMSampleBuffer, of type: SCStreamOutputType) {
        guard type == .audio, sampleBuffer.isValid else { return }
        try? sampleBuffer.withAudioBufferList { list, _ in
            guard let b = list.first, let raw = b.mData else { return }
            let n = Int(b.mDataByteSize) / MemoryLayout<Float>.size
            let p = raw.assumingMemoryBound(to: Float.self)
            chunker?.push(UnsafeBufferPointer(start: p, count: n))
        }
    }

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        onStop?(error.localizedDescription)
    }
}

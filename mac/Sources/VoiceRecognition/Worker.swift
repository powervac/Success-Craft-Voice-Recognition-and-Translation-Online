import Foundation

/// A line from the worker: KIND \t field \t field ...  (see worker.py for the protocol).
struct WorkerMsg {
    let kind: Character
    let f: [String]
    var text: String { f.last ?? "" }
    var ch: Int { f.count > 0 ? (Int(f[0]) ?? 0) & 1 : 0 }
}

/// Runs worker.py from the private virtual environment and speaks its pipe protocol:
/// stdin frames = 1 byte type + uint32 LE length + payload.
final class WorkerProcess {
    private var proc: Process?
    private var inH: FileHandle?
    private let writeQ = DispatchQueue(label: "voicerecognition.worker.write")
    private var acc = Data()
    var onMessage: ((WorkerMsg) -> Void)?

    func start(speakerModel: String, translationModel: String) throws {
        let p = Process()
        p.executableURL = Paths.venvPython
        p.arguments = ["-u", Paths.app.appendingPathComponent("worker.py").path]
        var env = ProcessInfo.processInfo.environment
        env["DICTATION_ROOT"] = Paths.data.path
        env["PYTHONUNBUFFERED"] = "1"
        env["PYTHONIOENCODING"] = "utf-8"
        env["TOKENIZERS_PARALLELISM"] = "false"
        if !speakerModel.isEmpty { env["SPEAKER_MODEL"] = speakerModel }
        if !translationModel.isEmpty { env["TRANSLATION_MODEL"] = translationModel }
        p.environment = env
        p.currentDirectoryURL = Paths.data
        let inPipe = Pipe(), outPipe = Pipe()
        p.standardInput = inPipe
        p.standardOutput = outPipe
        FileManager.default.createFile(atPath: Paths.log.path, contents: nil)
        p.standardError = try FileHandle(forWritingTo: Paths.log)
        outPipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            if d.isEmpty {
                h.readabilityHandler = nil
                DispatchQueue.main.async { self?.onMessage?(WorkerMsg(kind: "X", f: [])) }
                return
            }
            self?.consume(d)
        }
        try p.run()
        proc = p
        inH = inPipe.fileHandleForWriting
    }

    private func consume(_ d: Data) {
        acc.append(d)
        while let idx = acc.firstIndex(of: 10) {
            let lineData = acc.subdata(in: acc.startIndex..<idx)
            acc.removeSubrange(acc.startIndex...idx)
            let line = String(decoding: lineData, as: UTF8.self)
            guard let kind = line.first else { continue }
            let rest = line.count > 2 ? String(line.dropFirst(2)) : ""
            let fields = rest.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
            let m = WorkerMsg(kind: kind, f: fields)
            DispatchQueue.main.async { [weak self] in self?.onMessage?(m) }
        }
    }

    // ---- frames ----
    private func frame(_ type: UInt8, _ payload: Data) {
        var d = Data([type])
        var n = UInt32(payload.count).littleEndian
        withUnsafeBytes(of: &n) { d.append(contentsOf: $0) }
        d.append(payload)
        writeQ.async { [weak self] in try? self?.inH?.write(contentsOf: d) }
    }
    func sendAudio(ch: Int, samples: [Int16]) {
        var d = Data([UInt8(ch)])
        samples.withUnsafeBytes { d.append(contentsOf: $0) }
        frame(0x41, d)                                   // 'A'
    }
    func sendEnd(ch: Int) { frame(0x45, Data([UInt8(ch)])) }                  // 'E'
    func sendLanguage(_ s: String) { frame(0x4C, Data(s.utf8)) }              // 'L'  src|tgt|reverse|diarize|sensitivity
    func resetSpeakers() { frame(0x52, Data()) }                              // 'R'
    func setSpeakerModel(_ id: String) { frame(0x4D, Data(id.utf8)) }         // 'M'
    func setTranslationModel(_ id: String) { frame(0x47, Data(id.utf8)) }     // 'G'

    func stop() {
        frame(0x51, Data())                                                   // 'Q'
        let p = proc
        DispatchQueue.global().asyncAfter(deadline: .now() + 1.5) { if p?.isRunning == true { p?.terminate() } }
    }
    func killNow() { if proc?.isRunning == true { proc?.terminate() } }
}

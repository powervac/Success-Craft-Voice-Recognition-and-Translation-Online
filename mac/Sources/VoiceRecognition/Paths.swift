import Foundation

/// Everything the app writes lives in ~/Library/Application Support/VoiceRecognition (no admin rights needed).
enum Paths {
    static let support: URL = {
        let u = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("VoiceRecognition")
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }()
    static var python: URL { support.appendingPathComponent("python") }          // standalone CPython (if none is installed)
    static var venv: URL { support.appendingPathComponent("venv") }
    static var venvPython: URL { venv.appendingPathComponent("bin/python") }
    static var app: URL { support.appendingPathComponent("app") }                // worker.py, dl_progress.py, requirements.txt
    static var data: URL { support.appendingPathComponent("data") }              // models and temporary files
    static var history: URL { support.appendingPathComponent("history") }
    static var log: URL { support.appendingPathComponent("worker.log") }

    /// Files shipped inside the .app bundle (Contents/Resources).
    static var resources: URL {
        if let r = Bundle.main.resourceURL, FileManager.default.fileExists(atPath: r.appendingPathComponent("worker.py").path) { return r }
        // `swift run` during development: the files sit next to the package
        return URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().deletingLastPathComponent()
    }

    static func ensureDirs() {
        for d in [app, data, history] { try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true) }
    }
}

func nowMs() -> Int64 { Int64(DispatchTime.now().uptimeNanoseconds / 1_000_000) }

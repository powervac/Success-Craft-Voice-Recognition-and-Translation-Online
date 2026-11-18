import Foundation
import CryptoKit

enum BootstrapError: Error, LocalizedError {
    case message(String)
    var errorDescription: String? { if case .message(let s) = self { return s }; return nil }
}

/// First-run setup, repeated safely on every start (finished steps are skipped):
///   1. copy worker.py & co. out of the app bundle,
///   2. find a usable Python (3.10 - 3.13 with venv/ssl/sqlite) or download a private, relocatable CPython
///      (python-build-standalone) - no Homebrew, no Xcode tools, no admin rights, nothing is registered system-wide,
///   3. create the virtual environment.
/// Python packages and models are then checked / installed by worker.py itself (with progress messages).
enum Bootstrap {
    static let pbsTag = "20250317"
    static let pbsVersion = "3.12.9"

    typealias Status = (String, Double?) -> Void   // text, progress 0...1 (nil = indeterminate)

    static func run(status: @escaping Status) async throws {
        Paths.ensureDirs()
        status("Preparing files...", nil)
        try copyResources()

        if pythonWorks(Paths.venvPython) { return }       // environment already exists

        status("Looking for Python...", nil)
        var py = findPython()
        if py == nil {
            py = try await downloadPython(status: status)
        }
        guard let python = py else { throw BootstrapError.message("Could not set up Python.") }

        status("Creating the Python environment...", nil)
        try? FileManager.default.removeItem(at: Paths.venv)
        let rc = run(python, ["-m", "venv", Paths.venv.path])
        if rc != 0 || !pythonWorks(Paths.venvPython) {
            throw BootstrapError.message("Could not create the Python environment (code \(rc)).")
        }
    }

    // ---- 1. resources ----
    static func copyResources() throws {
        let fm = FileManager.default
        for name in ["worker.py", "dl_progress.py", "requirements.txt"] {
            let src = Paths.resources.appendingPathComponent(name)
            let dst = Paths.app.appendingPathComponent(name)
            guard fm.fileExists(atPath: src.path) else { throw BootstrapError.message("Missing file in the app: \(name)") }
            try? fm.removeItem(at: dst)
            try fm.copyItem(at: src, to: dst)
        }
    }

    // ---- 2. Python ----
    /// A usable Python: 3.10 - 3.13 with the modules the setup relies on.
    static func pythonWorks(_ exe: URL) -> Bool {
        guard FileManager.default.isExecutableFile(atPath: exe.path) else { return false }
        let code = "import sys, ssl, venv, ensurepip, sqlite3; sys.exit(0 if (3, 10) <= sys.version_info[:2] <= (3, 13) else 1)"
        return run(exe, ["-c", code]) == 0
    }

    static func findPython() -> URL? {
        var c: [String] = [Paths.python.appendingPathComponent("bin/python3").path]      // our private copy from an earlier run
        for v in ["3.13", "3.12", "3.11", "3.10"] {
            c.append("/opt/homebrew/bin/python\(v)")                                     // Homebrew, Apple Silicon
            c.append("/usr/local/bin/python\(v)")                                        // Homebrew, Intel
            c.append("/Library/Frameworks/Python.framework/Versions/\(v)/bin/python3")   // python.org installer
        }
        for p in c where pythonWorks(URL(fileURLWithPath: p)) { return URL(fileURLWithPath: p) }
        return nil
    }

    static func downloadPython(status: @escaping Status) async throws -> URL {
        #if arch(arm64)
        let arch = "aarch64"
        #else
        let arch = "x86_64"
        #endif
        let name = "cpython-\(pbsVersion)+\(pbsTag)-\(arch)-apple-darwin-install_only.tar.gz"
        let base = "https://github.com/astral-sh/python-build-standalone/releases/download/\(pbsTag)/"
        let encoded = name.replacingOccurrences(of: "+", with: "%2B")
        guard let url = URL(string: base + encoded), let shaURL = URL(string: base + encoded + ".sha256") else {
            throw BootstrapError.message("Bad download address.")
        }
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("voicerecognition-python.tar.gz")
        defer { try? FileManager.default.removeItem(at: tmp) }

        status("Downloading Python \(pbsVersion) (about 20 MB)...", 0)
        try await download(url, to: tmp) { status("Downloading Python \(pbsVersion): \(Int($0 * 100))%", $0) }

        // verify the checksum published next to the archive
        let (shaData, _) = try await URLSession.shared.data(from: shaURL)
        let expected = String(decoding: shaData, as: UTF8.self).split(whereSeparator: { $0 == " " || $0 == "\n" }).first.map(String.init) ?? ""
        let digest = SHA256.hash(data: try Data(contentsOf: tmp)).map { String(format: "%02x", $0) }.joined()
        if !expected.isEmpty && expected.lowercased() != digest {
            throw BootstrapError.message("The downloaded Python archive is corrupted (checksum mismatch). Try again.")
        }

        status("Unpacking Python...", nil)
        try? FileManager.default.removeItem(at: Paths.python)
        let rc = run(URL(fileURLWithPath: "/usr/bin/tar"), ["-xzf", tmp.path, "-C", Paths.support.path])   // creates .../python
        let exe = Paths.python.appendingPathComponent("bin/python3")
        if rc != 0 || !pythonWorks(exe) { throw BootstrapError.message("The unpacked Python does not start (code \(rc)).") }
        return exe
    }

    // ---- helpers ----
    @discardableResult
    static func run(_ exe: URL, _ args: [String]) -> Int32 {
        let p = Process()
        p.executableURL = exe
        p.arguments = args
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return -1 }
        p.waitUntilExit()
        return p.terminationStatus
    }

    static func download(_ url: URL, to dest: URL, progress: @escaping (Double) -> Void) async throws {
        let (bytes, resp) = try await URLSession.shared.bytes(from: url)
        if let h = resp as? HTTPURLResponse, h.statusCode != 200 {
            throw BootstrapError.message("Download failed (HTTP \(h.statusCode)). Check your internet connection.")
        }
        let total = resp.expectedContentLength
        FileManager.default.createFile(atPath: dest.path, contents: nil)
        let fh = try FileHandle(forWritingTo: dest)
        defer { try? fh.close() }
        var buf = Data()
        buf.reserveCapacity(1 << 16)
        var got: Int64 = 0
        for try await b in bytes {
            buf.append(b)
            got += 1
            if buf.count >= 1 << 16 {
                try fh.write(contentsOf: buf)
                buf.removeAll(keepingCapacity: true)
                if total > 0 { progress(Double(got) / Double(total)) }
            }
        }
        try fh.write(contentsOf: buf)
    }
}

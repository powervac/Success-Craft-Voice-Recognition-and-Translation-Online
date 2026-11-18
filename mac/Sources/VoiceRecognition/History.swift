import Foundation

/// One recognized utterance (same record layout as the Windows version: history/<stamp>.txt, tab separated).
struct Utt {
    var ch: Int
    var spk: Int
    var labeled: Bool
    var oLang: String
    var tLang: String
    var orig: String
    var tr: String
}

struct HistSession {
    var url: URL
    var stamp: String      // yyyy-MM-dd_HH-mm-ss
    var lines: Int
    var first: String
}

enum History {
    static func clean(_ s: String) -> String {
        s.replacingOccurrences(of: "\t", with: " ").replacingOccurrences(of: "\r", with: " ").replacingOccurrences(of: "\n", with: " ")
    }

    static func stampNow() -> String {
        let f = DateFormatter()
        f.dateFormat = "yyyy-MM-dd_HH-mm-ss"
        f.locale = Locale(identifier: "en_US_POSIX")
        return f.string(from: Date())
    }

    /// Creates the file of a new record; it shows up in the sidebar at once.
    static func create() -> URL? {
        Paths.ensureDirs()
        let stamp = stampNow()
        let url = Paths.history.appendingPathComponent(stamp + ".txt")
        let header = "#V1\t" + stamp + "\n"
        guard FileManager.default.createFile(atPath: url.path, contents: Data(header.utf8)) else { return nil }
        return url
    }

    static func append(_ u: Utt, to url: URL) {
        let line = "U\t\(u.ch)\t\(u.spk)\t\(u.labeled ? "1" : "0")\t\(u.oLang)\t\(u.tLang)\t\(clean(u.orig))\t\(clean(u.tr))\n"
        guard let h = try? FileHandle(forWritingTo: url) else { return }
        defer { try? h.close() }
        _ = try? h.seekToEnd()
        try? h.write(contentsOf: Data(line.utf8))
        try? h.synchronize()                 // the history survives a crash
    }

    static func parse(_ line: String) -> Utt? {
        let f = line.split(separator: "\t", omittingEmptySubsequences: false).map(String.init)
        guard f.count >= 8, f[0] == "U" else { return nil }
        return Utt(ch: (Int(f[1]) ?? 0) & 1, spk: Int(f[2]) ?? 1, labeled: f[3] == "1", oLang: f[4], tLang: f[5], orig: f[6], tr: f[7])
    }

    static func load(_ url: URL) -> [Utt] {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return [] }
        return text.split(separator: "\n").compactMap { parse(String($0)) }
    }

    static func scan(_ url: URL) -> HistSession {
        let text = (try? String(contentsOf: url, encoding: .utf8)) ?? ""
        var stamp = url.deletingPathExtension().lastPathComponent
        var lines = 0
        var first = ""
        for raw in text.split(separator: "\n") {
            let l = String(raw)
            if l.hasPrefix("#V1") {
                let f = l.split(separator: "\t").map(String.init)
                if f.count > 1 { stamp = f[1] }
            } else if let u = parse(l) {
                if lines == 0 { first = String(u.orig.prefix(70)) }
                lines += 1
            }
        }
        return HistSession(url: url, stamp: stamp, lines: lines, first: first)
    }

    /// All records, newest first.
    static func all() -> [HistSession] {
        let files = (try? FileManager.default.contentsOfDirectory(at: Paths.history, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "txt" }.map(scan).sorted { $0.stamp > $1.stamp }
    }

    static func title(_ h: HistSession, live: Bool) -> String {
        let d = String(h.stamp.prefix(10))
        var t = h.stamp.count >= 19 ? String(h.stamp.dropFirst(11).prefix(8)) : ""
        t = t.replacingOccurrences(of: "-", with: ":")
        return (live ? "\u{25CF} " : "") + "\(d)   \(t)   (\(h.lines))"
    }
}

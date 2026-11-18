import AppKit

struct Entry { var ch: Int; var spk: Int; var labeled: Bool; var text: String }

/// A scrolling text pane with committed lines and one lighter "live" (not yet final) line per channel.
final class Pane {
    let scroll = NSScrollView()
    let text = NSTextView()
    private var doneLen = 0
    private var live: [Int: Entry] = [:]
    private(set) var done: [Entry] = []

    init() {
        scroll.hasVerticalScroller = true
        scroll.borderType = .bezelBorder
        text.isEditable = false
        text.isRichText = true
        text.font = NSFont.systemFont(ofSize: 15)
        text.textContainerInset = NSSize(width: 8, height: 8)
        text.isVerticallyResizable = true
        text.isHorizontallyResizable = false
        text.autoresizingMask = [.width]
        text.textContainer?.widthTracksTextView = true
        scroll.documentView = text
        scroll.translatesAutoresizingMaskIntoConstraints = false
    }

    static func label(_ e: Entry) -> String {
        let base = e.ch == 0 ? "You" : "Them"
        return e.spk >= 2 ? "\(base) \(e.spk)" : base
    }

    static func color(_ ch: Int, _ spk: Int) -> NSColor {
        let p0: [(CGFloat, CGFloat, CGFloat)] = [(0, 84, 166), (123, 63, 180), (0, 130, 140), (70, 70, 170), (160, 50, 120), (70, 110, 140)]
        let p1: [(CGFloat, CGFloat, CGFloat)] = [(20, 130, 50), (205, 100, 10), (190, 40, 50), (120, 120, 0), (130, 80, 40), (190, 40, 160)]
        let i = (max(spk, 1) - 1) % 6
        let c = ch == 0 ? p0[i] : p1[i]
        return NSColor(srgbRed: c.0 / 255, green: c.1 / 255, blue: c.2 / 255, alpha: 1)
    }

    private func attributed(_ e: Entry, faded: Bool) -> NSAttributedString {
        let out = NSMutableAttributedString()
        let col = Pane.color(e.ch, e.spk)
        let font = NSFont.systemFont(ofSize: 15)
        let alpha: CGFloat = faded ? 0.6 : 1
        if e.labeled {
            out.append(NSAttributedString(string: Pane.label(e) + ": ", attributes: [
                .font: NSFont.boldSystemFont(ofSize: 15), .foregroundColor: col.withAlphaComponent(alpha)]))
            out.append(NSAttributedString(string: e.text + "\n", attributes: [
                .font: font, .foregroundColor: NSColor.labelColor.withAlphaComponent(alpha)]))
        } else {
            out.append(NSAttributedString(string: e.text + "\n", attributes: [
                .font: font, .foregroundColor: col.withAlphaComponent(alpha)]))
        }
        return out
    }

    private func atBottom() -> Bool {
        let h = scroll.documentView?.frame.height ?? 0
        return scroll.contentView.bounds.maxY >= h - 40
    }

    private func render(commit: Entry?) {
        guard let st = text.textStorage else { return }
        let stick = atBottom()
        st.beginEditing()
        if st.length > doneLen { st.deleteCharacters(in: NSRange(location: doneLen, length: st.length - doneLen)) }
        if let e = commit { st.append(attributed(e, faded: false)) }
        doneLen = st.length
        for c in 0...1 { if let l = live[c] { st.append(attributed(l, faded: true)) } }
        st.endEditing()
        if stick { text.scrollToEndOfDocument(nil) }
    }

    func setLive(_ ch: Int, _ e: Entry) { if e.text.isEmpty { live[ch] = nil } else { live[ch] = e }; render(commit: nil) }
    func clearLive(_ ch: Int) { live[ch] = nil; render(commit: nil) }
    func commit(_ e: Entry) { done.append(e); live[e.ch] = nil; render(commit: e) }

    func clear() {
        text.string = ""
        doneLen = 0
        live = [:]
        done = []
    }

    /// Plain text for saving to a file.
    func plain() -> String {
        done.map { $0.labeled ? "\(Pane.label($0)): \($0.text)" : $0.text }.joined(separator: "\n") + "\n"
    }
}

/// The round red Record / Stop button.
final class RecordButton: NSButton {
    var recording = false { didSet { needsDisplay = true } }
    override var intrinsicContentSize: NSSize { NSSize(width: 130, height: 40) }
    override var isEnabled: Bool { didSet { needsDisplay = true } }

    override func draw(_ dirtyRect: NSRect) {
        let r = bounds
        let d = min(r.height - 6, 30)
        let circle = NSRect(x: 6, y: (r.height - d) / 2, width: d, height: d)
        NSColor(srgbRed: 0.86, green: 0.10, blue: 0.10, alpha: isEnabled ? 1 : 0.35).setFill()
        NSBezierPath(ovalIn: circle).fill()
        if recording {
            NSColor.white.setFill()
            let s = d * 0.34
            NSBezierPath(rect: NSRect(x: circle.midX - s / 2, y: circle.midY - s / 2, width: s, height: s)).fill()
        }
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 15),
            .foregroundColor: isEnabled ? NSColor.labelColor : NSColor.disabledControlTextColor]
        let title = (recording ? "Stop" : "Record") as NSString
        let sz = title.size(withAttributes: attrs)
        title.draw(at: NSPoint(x: circle.maxX + 8, y: (r.height - sz.height) / 2), withAttributes: attrs)
    }
}

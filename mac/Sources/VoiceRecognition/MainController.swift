import AppKit
import AVFoundation

struct ModelItem { var id: String; var label: String; var mb: Int; var present: Bool }

let languages: [(code: String, name: String)] = [
    ("ru", "Russian"), ("en", "English"), ("de", "German"), ("fr", "French"),
    ("es", "Spanish"), ("uk", "Ukrainian"), ("pl", "Polish"), ("it", "Italian")]

final class MainController: NSObject, NSTableViewDataSource, NSTableViewDelegate, NSWindowDelegate {
    // ---- state ----
    let worker = WorkerProcess()
    let mic = MicCapture()
    let sysCap = SystemCapture()
    let channels = [Channel(0), Channel(1)]
    var inDevices: [InputDevice] = []
    var ready = false, busy = true, switching = false, capturing = false, active = false
    var trAvailable = true, spkAvailable = true
    var sessionURL: URL?
    var sessions: [HistSession] = []
    var utts: [Utt] = []
    var waiting: [[Int]] = [[], []]
    var trModels: [ModelItem] = [], spkModels: [ModelItem] = []
    var trWanted = "", spkWanted = "", trActive = "", spkActive = ""
    var refreshing = false
    let defaults = UserDefaults.standard

    // ---- views ----
    let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1180, height: 780),
                          styleMask: [.titled, .closable, .miniaturizable, .resizable], backing: .buffered, defer: false)
    let table = NSTableView()
    let newBtn = NSButton(title: "+ New session", target: nil, action: nil)
    let srcPane = Pane(), trPane = Pane()
    let recordBtn = RecordButton()
    let statusLbl = NSTextField(labelWithString: "Starting...")
    let progress = NSProgressIndicator()
    let spinner = NSProgressIndicator()
    let levelMic = NSLevelIndicator(), levelSys = NSLevelIndicator()

    let micPop = NSPopUpButton(), micOn = NSButton(checkboxWithTitle: "Use microphone", target: nil, action: nil)
    let sysOn = NSButton(checkboxWithTitle: "Their voice (system audio)", target: nil, action: nil)
    let revChk = NSButton(checkboxWithTitle: "Translate them the other way", target: nil, action: nil)
    let echoChk = NSButton(checkboxWithTitle: "Echo filter", target: nil, action: nil)
    let langPop = NSPopUpButton(), tgtPop = NSPopUpButton(), swapBtn = NSButton(title: "\u{21C4}", target: nil, action: nil)
    let trmPop = NSPopUpButton(), clearBtn = NSButton(title: "Clear", target: nil, action: nil)
    let gainSld = NSSlider(value: 0, minValue: -10, maxValue: 30, target: nil, action: nil), gainLbl = NSTextField(labelWithString: "+0 dB")
    let agcChk = NSButton(checkboxWithTitle: "Auto gain", target: nil, action: nil)
    let boostChk = NSButton(checkboxWithTitle: "Boost +12 dB", target: nil, action: nil)
    let nsChk = NSButton(checkboxWithTitle: "Noise suppression", target: nil, action: nil)
    let nsSld = NSSlider(value: 70, minValue: 0, maxValue: 100, target: nil, action: nil), nsLbl = NSTextField(labelWithString: "70%")
    let oneRadio = NSButton(radioButtonWithTitle: "One speaker", target: nil, action: nil)
    let manyRadio = NSButton(radioButtonWithTitle: "Several speakers", target: nil, action: nil)
    let spkPop = NSPopUpButton()
    let sensSld = NSSlider(value: 50, minValue: 0, maxValue: 100, target: nil, action: nil), sensLbl = NSTextField(labelWithString: "50%")
    let topChk = NSButton(checkboxWithTitle: "Always on top", target: nil, action: nil)
    let saveBtn = NSButton(title: "Save documents...", target: nil, action: nil)
    var gated: [NSControl] = []

    // =============================================================================================
    //  Start-up
    // =============================================================================================
    func start() {
        buildUI()
        loadDefaults()
        wireChannels()
        refreshSidebar(select: nil)
        updateUi()
        window.makeKeyAndOrderFront(nil)
        worker.onMessage = { [weak self] m in self?.handle(m) }
        setBusy(true)
        Task {
            do {
                try await Bootstrap.run { text, frac in
                    DispatchQueue.main.async { [weak self] in
                        self?.statusLbl.stringValue = text
                        if let f = frac { self?.showProgress(Int(f * 100)) } else { self?.progress.isHidden = true }
                    }
                }
                DispatchQueue.main.async { [weak self] in self?.launchWorker() }
            } catch {
                DispatchQueue.main.async { [weak self] in
                    self?.setBusy(false)
                    self?.progress.isHidden = true
                    self?.statusLbl.stringValue = "Setup failed: \(error.localizedDescription)"
                }
            }
        }
    }

    func launchWorker() {
        do {
            statusLbl.stringValue = "Starting the recognition engine..."
            try worker.start(speakerModel: spkWanted, translationModel: trWanted)
        } catch {
            setBusy(false)
            statusLbl.stringValue = "Could not start the recognition engine: \(error.localizedDescription)"
        }
    }

    func shutdown() {
        if capturing { stopCapture() }
        worker.stop()
    }

    func wireChannels() {
        for c in channels {
            c.onAudio = { [weak self] ch, s in self?.worker.sendAudio(ch: ch, samples: s) }
            c.onEnd = { [weak self] ch in self?.worker.sendEnd(ch: ch) }
            c.onLevel = { [weak self] ch, lvl, _ in
                DispatchQueue.main.async { (ch == 0 ? self?.levelMic : self?.levelSys)?.doubleValue = lvl }
            }
        }
    }

    // =============================================================================================
    //  UI construction
    // =============================================================================================
    func lbl(_ s: String) -> NSTextField { NSTextField(labelWithString: s) }

    func row(_ views: [NSView]) -> NSStackView {
        let s = NSStackView(views: views)
        s.orientation = .horizontal
        s.alignment = .centerY
        s.spacing = 10
        return s
    }

    func paneBox(_ title: String, _ pane: Pane) -> NSStackView {
        let t = NSTextField(labelWithString: title)
        t.font = NSFont.boldSystemFont(ofSize: 12)
        let s = NSStackView(views: [t, pane.scroll])
        s.orientation = .vertical
        s.alignment = .leading
        s.spacing = 4
        pane.scroll.setContentHuggingPriority(.defaultLow, for: .vertical)
        pane.scroll.widthAnchor.constraint(equalTo: s.widthAnchor).isActive = true
        return s
    }

    func buildUI() {
        window.title = "Voice Recognition and Translation"
        window.minSize = NSSize(width: 1100, height: 640)
        if let vis = NSScreen.main?.visibleFrame {      // never taller / wider than the screen
            window.setContentSize(NSSize(width: min(1180, vis.width - 40), height: min(780, vis.height - 60)))
        }
        window.delegate = self
        window.center()

        // targets / actions
        for (b, a) in [(micOn, #selector(optionsChanged)), (sysOn, #selector(optionsChanged)), (revChk, #selector(optionsChanged)),
                       (echoChk, #selector(optionsChanged)), (agcChk, #selector(optionsChanged)), (boostChk, #selector(optionsChanged)),
                       (nsChk, #selector(optionsChanged)), (oneRadio, #selector(optionsChanged)), (manyRadio, #selector(optionsChanged))] as [(NSButton, Selector)] {
            b.target = self; b.action = a
        }
        for s in [gainSld, nsSld, sensSld] { s.target = self; s.action = #selector(optionsChanged); s.isContinuous = true }
        topChk.target = self; topChk.action = #selector(topChanged)
        langPop.target = self; langPop.action = #selector(languageChanged)
        tgtPop.target = self; tgtPop.action = #selector(languageChanged)
        swapBtn.target = self; swapBtn.action = #selector(swapLanguages)
        trmPop.target = self; trmPop.action = #selector(trModelChanged)
        spkPop.target = self; spkPop.action = #selector(spkModelChanged)
        clearBtn.target = self; clearBtn.action = #selector(clearPanes)
        saveBtn.target = self; saveBtn.action = #selector(saveDocuments)
        newBtn.target = self; newBtn.action = #selector(newSession)
        newBtn.bezelStyle = .rounded
        recordBtn.target = self; recordBtn.action = #selector(toggleRecord)
        recordBtn.isBordered = false
        recordBtn.title = ""

        langPop.addItem(withTitle: "Auto-detect")
        tgtPop.addItem(withTitle: "No translation")
        for l in languages { langPop.addItem(withTitle: l.name); tgtPop.addItem(withTitle: l.name) }
        oneRadio.state = .on

        inDevices = listInputDevices()
        micPop.addItem(withTitle: "System default")
        for d in inDevices { micPop.addItem(withTitle: d.name) }

        for p in [langPop, tgtPop, trmPop, spkPop, micPop] { p.controlSize = .regular }
        trmPop.addItem(withTitle: "Loading...")
        spkPop.addItem(withTitle: "Loading...")
        gainSld.widthAnchor.constraint(equalToConstant: 140).isActive = true
        nsSld.widthAnchor.constraint(equalToConstant: 120).isActive = true
        sensSld.widthAnchor.constraint(equalToConstant: 120).isActive = true

        // fixed widths: nothing may push the window wider than the screen
        micPop.widthAnchor.constraint(equalToConstant: 200).isActive = true
        langPop.widthAnchor.constraint(equalToConstant: 130).isActive = true
        tgtPop.widthAnchor.constraint(equalToConstant: 150).isActive = true
        trmPop.widthAnchor.constraint(equalToConstant: 260).isActive = true
        spkPop.widthAnchor.constraint(equalToConstant: 230).isActive = true
        gainLbl.widthAnchor.constraint(equalToConstant: 56).isActive = true
        nsLbl.widthAnchor.constraint(equalToConstant: 40).isActive = true
        sensLbl.widthAnchor.constraint(equalToConstant: 40).isActive = true

        let r1 = row([lbl("Microphone:"), micPop, micOn, sysOn, echoChk])
        let r2 = row([lbl("Language:"), langPop, swapBtn, lbl("Translate to:"), tgtPop, revChk])
        let r3 = row([lbl("Translation model:"), trmPop, clearBtn, saveBtn, topChk])
        let r4 = row([lbl("Gain:"), gainSld, gainLbl, agcChk, boostChk, nsChk, nsSld, nsLbl])
        let r5 = row([oneRadio, manyRadio, lbl("Speaker model:"), spkPop, lbl("Sensitivity:"), sensSld, sensLbl])
        for r in [r1, r2, r3, r4, r5] { r.setHuggingPriority(.required, for: .vertical) }

        gated = [micPop, micOn, sysOn, revChk, echoChk, langPop, tgtPop, swapBtn, trmPop, clearBtn, gainSld, agcChk, boostChk, nsChk,
                 nsSld, oneRadio, manyRadio, spkPop, sensSld, saveBtn]

        let panes = NSStackView(views: [paneBox("Recognized speech", srcPane), paneBox("Translation", trPane)])
        panes.orientation = .vertical
        panes.distribution = .fillEqually
        panes.spacing = 8
        panes.alignment = .leading

        progress.style = .bar
        progress.isIndeterminate = false
        progress.minValue = 0
        progress.maxValue = 100
        progress.isHidden = true
        progress.widthAnchor.constraint(equalToConstant: 160).isActive = true
        spinner.style = .spinning
        spinner.controlSize = .small
        spinner.isDisplayedWhenStopped = false
        for l in [levelMic, levelSys] {
            l.levelIndicatorStyle = .continuousCapacity
            l.minValue = 0; l.maxValue = 100
            l.widthAnchor.constraint(equalToConstant: 70).isActive = true
        }
        statusLbl.lineBreakMode = .byTruncatingTail
        statusLbl.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        let bottom = row([recordBtn, lbl("You"), levelMic, lbl("Them"), levelSys, spinner, progress, statusLbl])

        for box in panes.arrangedSubviews { box.widthAnchor.constraint(equalTo: panes.widthAnchor).isActive = true }
        let main = NSStackView(views: [r1, r2, r3, r4, r5, panes, bottom])
        main.orientation = .vertical
        main.alignment = .leading
        main.spacing = 10
        main.edgeInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 12)
        panes.widthAnchor.constraint(equalTo: main.widthAnchor, constant: -24).isActive = true
        bottom.widthAnchor.constraint(equalTo: main.widthAnchor, constant: -24).isActive = true

        // sidebar
        let col = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("s"))
        col.title = "Sessions"
        table.addTableColumn(col)
        table.headerView = nil
        table.dataSource = self
        table.delegate = self
        let menu = NSMenu()
        menu.addItem(NSMenuItem(title: "Delete session", action: #selector(deleteSelected), keyEquivalent: ""))
        menu.items.forEach { $0.target = self }
        table.menu = menu
        let tScroll = NSScrollView()
        tScroll.documentView = table
        tScroll.hasVerticalScroller = true
        tScroll.borderType = .bezelBorder
        let sideTitle = NSTextField(labelWithString: "Sessions")
        sideTitle.font = NSFont.boldSystemFont(ofSize: 12)
        let side = NSStackView(views: [sideTitle, newBtn, tScroll])
        side.orientation = .vertical
        side.alignment = .leading
        side.spacing = 8
        side.edgeInsets = NSEdgeInsets(top: 12, left: 12, bottom: 12, right: 6)
        newBtn.widthAnchor.constraint(equalTo: side.widthAnchor, constant: -18).isActive = true
        tScroll.widthAnchor.constraint(equalTo: side.widthAnchor, constant: -18).isActive = true
        side.widthAnchor.constraint(equalToConstant: 250).isActive = true

        let root = NSStackView(views: [side, main])
        root.orientation = .horizontal
        root.alignment = .top
        root.spacing = 0
        root.distribution = .fill
        side.heightAnchor.constraint(equalTo: root.heightAnchor).isActive = true
        main.heightAnchor.constraint(equalTo: root.heightAnchor).isActive = true
        window.contentView = root
    }

    // =============================================================================================
    //  Settings
    // =============================================================================================
    func loadDefaults() {
        func b(_ k: String, _ d: Bool) -> Bool { defaults.object(forKey: k) == nil ? d : defaults.bool(forKey: k) }
        func i(_ k: String, _ d: Int) -> Int { defaults.object(forKey: k) == nil ? d : defaults.integer(forKey: k) }
        micOn.state = b("micon", true) ? .on : .off
        sysOn.state = b("syson", false) ? .on : .off
        revChk.state = b("rev", false) ? .on : .off
        echoChk.state = b("echo", true) ? .on : .off
        agcChk.state = b("agc", true) ? .on : .off
        boostChk.state = b("boost", false) ? .on : .off
        nsChk.state = b("ns", true) ? .on : .off
        gainSld.integerValue = i("gain", 0)
        nsSld.integerValue = i("nsstr", 70)
        sensSld.integerValue = i("sens", 50)
        manyRadio.state = b("many", false) ? .on : .off
        oneRadio.state = manyRadio.state == .on ? .off : .on
        topChk.state = b("top", false) ? .on : .off
        langPop.selectItem(at: min(i("lang", 0), langPop.numberOfItems - 1))
        tgtPop.selectItem(at: min(i("tgt", 0), tgtPop.numberOfItems - 1))
        trWanted = defaults.string(forKey: "trmodel") ?? ""
        spkWanted = defaults.string(forKey: "spkmodel") ?? ""
        topChanged()
        readOptions(save: false)
    }

    func saveDefaults() {
        defaults.set(micOn.state == .on, forKey: "micon")
        defaults.set(sysOn.state == .on, forKey: "syson")
        defaults.set(revChk.state == .on, forKey: "rev")
        defaults.set(echoChk.state == .on, forKey: "echo")
        defaults.set(agcChk.state == .on, forKey: "agc")
        defaults.set(boostChk.state == .on, forKey: "boost")
        defaults.set(nsChk.state == .on, forKey: "ns")
        defaults.set(gainSld.integerValue, forKey: "gain")
        defaults.set(nsSld.integerValue, forKey: "nsstr")
        defaults.set(sensSld.integerValue, forKey: "sens")
        defaults.set(manyRadio.state == .on, forKey: "many")
        defaults.set(topChk.state == .on, forKey: "top")
        defaults.set(langPop.indexOfSelectedItem, forKey: "lang")
        defaults.set(tgtPop.indexOfSelectedItem, forKey: "tgt")
        defaults.set(trWanted, forKey: "trmodel")
        defaults.set(spkWanted, forKey: "spkmodel")
    }

    /// controls -> audio-thread settings, value labels
    func readOptions(save: Bool) {
        let s = Settings.shared
        s.gainDb = gainSld.integerValue
        s.nsStr = nsSld.integerValue
        s.ns = nsChk.state == .on
        s.agc = agcChk.state == .on
        s.boost = boostChk.state == .on
        s.micOn = micOn.state == .on
        s.sysOn = sysOn.state == .on
        s.rev = revChk.state == .on
        s.echo = echoChk.state == .on
        s.diar = manyRadio.state == .on && spkAvailable
        s.sens = sensSld.integerValue
        gainLbl.stringValue = String(format: "%+d dB", s.gainDb)
        nsLbl.stringValue = "\(s.nsStr)%"
        sensLbl.stringValue = "\(s.sens)%"
        if save { saveDefaults() }
    }

    func code(_ pop: NSPopUpButton, special: String) -> String {
        let i = pop.indexOfSelectedItem
        return i <= 0 ? special : languages[i - 1].code
    }

    func sendLanguage() {
        let s = Settings.shared
        worker.sendLanguage("\(code(langPop, special: "auto"))|\(code(tgtPop, special: "none"))|\(s.rev ? 1 : 0)|\(s.diar ? 1 : 0)|\(s.sens)")
    }

    // =============================================================================================
    //  Actions
    // =============================================================================================
    @objc func optionsChanged() {
        readOptions(save: true)
        updateUi()
        if ready { sendLanguage() }
    }
    @objc func languageChanged() { saveDefaults(); if ready { sendLanguage() } }
    @objc func swapLanguages() {
        let a = langPop.indexOfSelectedItem, b = tgtPop.indexOfSelectedItem
        if a > 0 { tgtPop.selectItem(at: a) } else { tgtPop.selectItem(at: 0) }
        if b > 0 { langPop.selectItem(at: b) } else { langPop.selectItem(at: 0) }
        languageChanged()
    }
    @objc func topChanged() { window.level = topChk.state == .on ? .floating : .normal; saveDefaults() }
    @objc func clearPanes() { srcPane.clear(); trPane.clear(); utts = []; waiting = [[], []] }

    @objc func trModelChanged() {
        let i = trmPop.indexOfSelectedItem
        guard ready, i >= 0, i < trModels.count, trModels[i].id != trActive else { return }
        trWanted = trModels[i].id
        saveDefaults()
        switching = true
        updateUi()
        worker.setTranslationModel(trWanted)
    }
    @objc func spkModelChanged() {
        let i = spkPop.indexOfSelectedItem
        guard ready, i >= 0, i < spkModels.count, spkModels[i].id != spkActive else { return }
        spkWanted = spkModels[i].id
        saveDefaults()
        switching = true
        updateUi()
        worker.setSpeakerModel(spkWanted)
    }

    @objc func saveDocuments() {
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = false
        p.canCreateDirectories = true
        p.prompt = "Save here"
        p.message = "Choose a folder: the recognized text and its translation are saved as two files."
        guard p.runModal() == .OK, let dir = p.url else { return }
        let stamp = History.stampNow()
        do {
            try srcPane.plain().write(to: dir.appendingPathComponent("\(stamp)_original.txt"), atomically: true, encoding: .utf8)
            if !trPane.done.isEmpty {
                try trPane.plain().write(to: dir.appendingPathComponent("\(stamp)_translation.txt"), atomically: true, encoding: .utf8)
            }
            statusLbl.stringValue = "Saved to \(dir.path)"
        } catch { statusLbl.stringValue = "Could not save: \(error.localizedDescription)" }
    }

    // ---- sessions ----
    @objc func newSession() {
        if capturing { return }
        dropEmpty(sessionURL)
        guard let url = History.create() else { statusLbl.stringValue = "Could not create a session file."; return }
        sessionURL = url
        active = true
        srcPane.clear(); trPane.clear(); utts = []; waiting = [[], []]
        if ready { worker.resetSpeakers() }
        refreshSidebar(select: url)
        statusLbl.stringValue = ready ? "New session. Press Record." : statusLbl.stringValue
        updateUi()
    }

    func dropEmpty(_ url: URL?) {
        guard let u = url, History.scan(u).lines == 0 else { return }
        try? FileManager.default.removeItem(at: u)
    }

    func refreshSidebar(select: URL?) {
        sessions = History.all()
        refreshing = true
        table.reloadData()
        if let u = select, let i = sessions.firstIndex(where: { $0.url == u }) {
            table.selectRowIndexes(IndexSet(integer: i), byExtendingSelection: false)
        }
        refreshing = false
    }

    func numberOfRows(in tableView: NSTableView) -> Int { sessions.count }
    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        let id = NSUserInterfaceItemIdentifier("cell")
        let cell = (tableView.makeView(withIdentifier: id, owner: self) as? NSTextField) ?? {
            let t = NSTextField(labelWithString: "")
            t.identifier = id
            t.lineBreakMode = .byTruncatingTail
            return t
        }()
        let h = sessions[row]
        cell.stringValue = History.title(h, live: capturing && h.url == sessionURL)
        return cell
    }
    func tableViewSelectionDidChange(_ notification: Notification) {
        if refreshing || capturing { return }
        let i = table.selectedRow
        guard i >= 0, i < sessions.count else { return }
        openSession(sessions[i])
    }

    func openSession(_ h: HistSession) {
        dropEmpty(sessionURL)
        sessionURL = nil
        active = false
        srcPane.clear(); trPane.clear(); utts = []; waiting = [[], []]
        var n = 0
        for u in History.load(h.url) {
            utts.append(u)
            srcPane.commit(Entry(ch: u.ch, spk: u.spk, labeled: u.labeled, text: u.orig))
            if !u.tr.isEmpty { trPane.commit(Entry(ch: u.ch, spk: u.spk, labeled: u.labeled, text: u.tr)) }
            n += 1
        }
        statusLbl.stringValue = "Loaded a session from the history (\(n) lines)."
        updateUi()
    }

    @objc func deleteSelected() {
        let i = table.clickedRow >= 0 ? table.clickedRow : table.selectedRow
        guard !capturing, i >= 0, i < sessions.count else { return }
        let h = sessions[i]
        let a = NSAlert()
        a.messageText = "Delete this session?"
        a.informativeText = "\(History.title(h, live: false)) will be deleted permanently."
        a.addButton(withTitle: "Delete")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        try? FileManager.default.removeItem(at: h.url)
        if h.url == sessionURL { sessionURL = nil; active = false; srcPane.clear(); trPane.clear(); utts = [] }
        refreshSidebar(select: nil)
        updateUi()
    }

    // ---- recording ----
    @objc func toggleRecord() { capturing ? stopCapture() : startCapture() }

    func startCapture() {
        guard ready, active, !capturing else { return }
        readOptions(save: false)
        let s = Settings.shared
        if !s.micOn && !s.sysOn { statusLbl.stringValue = "Turn on the microphone or their voice first."; return }
        if s.micOn {
            switch AVCaptureDevice.authorizationStatus(for: .audio) {
            case .authorized: break
            case .notDetermined:
                AVCaptureDevice.requestAccess(for: .audio) { ok in
                    DispatchQueue.main.async { if ok { self.startCapture() } else { self.permissionHelp("Microphone", "Privacy_Microphone") } }
                }
                return
            default: permissionHelp("Microphone", "Privacy_Microphone"); return
            }
        }
        if s.sysOn && !SystemCapture.hasPermission() {
            SystemCapture.requestPermission()
            permissionHelp("Screen & System Audio Recording", "Privacy_ScreenCapture")
            return
        }
        for c in channels { c.flush() }
        var err = ""
        if s.micOn {
            let idx = micPop.indexOfSelectedItem
            let dev: AudioDeviceID? = idx > 0 && idx - 1 < inDevices.count ? inDevices[idx - 1].id : nil
            do { try mic.start(device: dev) { [weak self] chunk in self?.channels[0].feed(chunk) } }
            catch { err = "Microphone: \(error.localizedDescription)" }
        }
        capturing = true
        updateUi()
        refreshSidebar(select: sessionURL)
        statusLbl.stringValue = err.isEmpty ? "Listening..." : err
        if s.sysOn {
            Task {
                do { try await self.sysCap.start { [weak self] chunk in self?.channels[1].feed(chunk) } }
                catch {
                    DispatchQueue.main.async { self.statusLbl.stringValue = "System audio: \(error.localizedDescription)" }
                }
            }
            sysCap.onStop = { [weak self] msg in
                DispatchQueue.main.async { self?.statusLbl.stringValue = "System audio stopped: \(msg)" }
            }
        }
    }

    func stopCapture() {
        mic.stop()
        sysCap.stop()
        for c in channels { c.flush() }
        capturing = false
        levelMic.doubleValue = 0; levelSys.doubleValue = 0
        statusLbl.stringValue = "Stopped."
        refreshSidebar(select: sessionURL)
        updateUi()
    }

    func permissionHelp(_ what: String, _ pane: String) {
        statusLbl.stringValue = "Allow \"\(what)\" for Voice Recognition in System Settings, then press Record again."
        if let u = URL(string: "x-apple.systempreferences:com.apple.preference.security?\(pane)") { NSWorkspace.shared.open(u) }
    }

    // =============================================================================================
    //  UI state
    // =============================================================================================
    func setBusy(_ b: Bool) {
        busy = b
        if b { spinner.startAnimation(nil) } else { spinner.stopAnimation(nil) }
        updateUi()
    }

    func showProgress(_ pct: Int) {
        progress.isHidden = false
        progress.doubleValue = Double(min(100, max(0, pct)))
    }

    func updateUi() {
        let sessionOn = active || capturing
        for c in gated { c.isEnabled = sessionOn }
        if sessionOn {
            tgtPop.isEnabled = trAvailable
            swapBtn.isEnabled = trAvailable
            trmPop.isEnabled = trAvailable && ready && !switching
            manyRadio.isEnabled = spkAvailable
            micPop.isEnabled = micOn.state == .on && !capturing
            revChk.isEnabled = sysOn.state == .on
            echoChk.isEnabled = sysOn.state == .on
            nsSld.isEnabled = nsChk.state == .on
            sensSld.isEnabled = manyRadio.state == .on && spkAvailable
            spkPop.isEnabled = manyRadio.state == .on && spkAvailable && ready && !switching
        }
        recordBtn.isEnabled = ready && sessionOn && !switching
        recordBtn.recording = capturing
        newBtn.isEnabled = !capturing
        newBtn.bezelColor = (!active && !capturing) ? NSColor.systemOrange : nil
        table.isEnabled = !capturing
        if busy || switching { spinner.startAnimation(nil) } else { spinner.stopAnimation(nil) }
    }

    func refreshModelPop(_ pop: NSPopUpButton, _ items: [ModelItem], active: String) {
        pop.removeAllItems()
        for m in items { pop.addItem(withTitle: m.present ? m.label : "\(m.label) - download \(m.mb) MB") }
        if let i = items.firstIndex(where: { $0.id == (active.isEmpty ? (pop === trmPop ? trWanted : spkWanted) : active) }) { pop.selectItem(at: i) }
    }

    // =============================================================================================
    //  Worker messages
    // =============================================================================================
    func labeled() -> Bool { Settings.shared.sysOn || Settings.shared.diar }

    func handle(_ m: WorkerMsg) {
        switch m.kind {
        case "S":
            if m.text.hasPrefix("ready") {
                ready = true
                setBusy(false)
                progress.isHidden = true
                sendLanguage()
                let dev = String(m.text.dropFirst(6))
                let tr = trAvailable ? ", translation enabled." : ", translation unavailable."
                statusLbl.stringValue = "Ready " + dev + tr + " Press \"+ New session\", then Record."
                updateUi()
            } else { statusLbl.stringValue = m.text }
        case "N":
            trAvailable = false
            tgtPop.selectItem(at: 0)
            saveDefaults()
            updateUi()
        case "K", "C":
            guard m.f.count >= 5 else { break }
            let item = ModelItem(id: m.f[1], label: m.f[2], mb: Int(m.f[3]) ?? 0, present: m.f[4] == "1")
            if m.kind == "K" {
                if m.f[0] == "0" { trModels = [] }
                trModels.append(item)
                refreshModelPop(trmPop, trModels, active: trActive)
            } else {
                if m.f[0] == "0" { spkModels = [] }
                spkModels.append(item)
                refreshModelPop(spkPop, spkModels, active: spkActive)
            }
        case "Z":
            trActive = m.text
            switching = false
            for i in trModels.indices where trModels[i].id == m.text { trModels[i].present = true }
            if trWanted.isEmpty { trWanted = m.text }
            refreshModelPop(trmPop, trModels, active: trActive)
            if !m.text.isEmpty { progress.isHidden = true }
            updateUi()
        case "M":
            spkActive = m.text
            switching = false
            for i in spkModels.indices where spkModels[i].id == m.text { spkModels[i].present = true }
            if spkWanted.isEmpty { spkWanted = m.text }
            refreshModelPop(spkPop, spkModels, active: spkActive)
            if !m.text.isEmpty { progress.isHidden = true }
            updateUi()
        case "E":
            setBusy(false)
            statusLbl.stringValue = m.text
        case "D":
            let pct = Int(m.f.first ?? "") ?? -1
            if pct < 0 { progress.isHidden = true } else {
                showProgress(pct)
                if m.f.count > 1 { statusLbl.stringValue = m.f[1] }
            }
        case "V":
            spkAvailable = false
            manyRadio.state = .off
            oneRadio.state = .on
            readOptions(save: true)
            updateUi()
        case "P":
            let ch = m.ch
            srcPane.setLive(ch, Entry(ch: ch, spk: Int(m.f.count > 1 ? m.f[1] : "1") ?? 1, labeled: labeled(), text: m.text))
        case "F":
            let ch = m.ch
            if m.text.isEmpty { waiting[ch].append(-1); srcPane.clearLive(ch); break }
            let spk = Int(m.f.count > 1 ? m.f[1] : "1") ?? 1
            let lang = m.f.count > 2 ? m.f[2] : ""
            let e = Entry(ch: ch, spk: spk, labeled: labeled(), text: m.text)
            utts.append(Utt(ch: ch, spk: spk, labeled: e.labeled, oLang: lang, tLang: "", orig: m.text, tr: ""))
            waiting[ch].append(utts.count - 1)
            srcPane.commit(e)
        case "T":
            let ch = m.ch
            trPane.setLive(ch, Entry(ch: ch, spk: Int(m.f.count > 1 ? m.f[1] : "1") ?? 1, labeled: labeled(), text: m.text))
        case "U":
            let ch = m.ch
            var idx = -1
            if !waiting[ch].isEmpty { idx = waiting[ch].removeFirst() }
            if idx >= 0 && idx < utts.count {
                if !m.text.isEmpty {
                    utts[idx].tr = m.text
                    utts[idx].tLang = m.f.count > 2 ? m.f[2] : ""
                    trPane.commit(Entry(ch: ch, spk: utts[idx].spk, labeled: utts[idx].labeled, text: m.text))
                } else { trPane.clearLive(ch) }
                if let url = sessionURL { History.append(utts[idx], to: url); refreshSidebar(select: url) }
            } else { trPane.clearLive(ch) }
        case "X":
            ready = false
            setBusy(false)
            if capturing { stopCapture() }
            statusLbl.stringValue = "The recognition engine stopped. Details: \(Paths.log.path)"
            updateUi()
        default: break
        }
    }

    func windowWillClose(_ notification: Notification) { shutdown(); NSApp.terminate(nil) }
}

// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 jonathanbeiko and contributors
// Digi 003 — menu bar companion for the Digi003 Core Audio driver.
// Status, input meters, ADAT routing, optical mode, latency, clock, sample rate, restart,
// notifications, sleep prevention and a Core MIDI bridge for the 003's MIDI IN/OUT.
import AppKit
import CoreAudio
import CoreMIDI
import IOKit.pwr_mgt
import UserNotifications

// MARK: - Language
// Spanish when the Mac's preferred language is Spanish, English otherwise.
let isSpanish = (Locale.preferredLanguages.first ?? "en").hasPrefix("es") || ProcessInfo.processInfo.environment["DIGI003_LANG"] == "es"
let headless = CommandLine.arguments.contains("--print-menu")   // menu dump only: touch no MIDI or audio
func L(_ es: String, _ en: String) -> String {
    if let forced = ProcessInfo.processInfo.environment["DIGI003_LANG"] { return forced == "es" ? es : en }
    return isSpanish ? es : en
}

// MARK: - Core Audio helpers

func fourCC(_ s: String) -> AudioObjectPropertySelector { s.utf8.reduce(0) { ($0 << 8) | UInt32($1) } }
let selStatus = fourCC("d3st"), selLevels = fourCC("d3lv"), selRoute = fourCC("d3rt"), selLowLat = fourCC("d3ll"),
    selOptical = fourCC("d3op"), selRestart = fourCC("d3rs"), selKeepAlive = fourCC("d3ka"),
    selMidiIn = fourCC("d3mi"), selMidiOut = fourCC("d3mo")

func addr(_ sel: AudioObjectPropertySelector) -> AudioObjectPropertyAddress {
    AudioObjectPropertyAddress(mSelector: sel, mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
}

enum CA {
    static func devices() -> [AudioObjectID] {
        var a = addr(kAudioHardwarePropertyDevices); var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size) == noErr else { return [] }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / 4)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a, 0, nil, &size, &ids) == noErr else { return [] }
        return ids
    }
    static func string(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> String? {
        var a = addr(sel); var s: Unmanaged<CFString>?; var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &s) == noErr, let v = s else { return nil }
        return v.takeRetainedValue() as String
    }
    static func uint32(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector) -> UInt32? {
        var a = addr(sel); var v: UInt32 = 0; var size = UInt32(4)
        return AudioObjectGetPropertyData(obj, &a, 0, nil, &size, &v) == noErr ? v : nil
    }
    @discardableResult static func setUInt32(_ obj: AudioObjectID, _ sel: AudioObjectPropertySelector, _ value: UInt32) -> OSStatus {
        var a = addr(sel); var v = value
        return AudioObjectSetPropertyData(obj, &a, 0, nil, 4, &v)
    }
    @discardableResult static func setRate(_ obj: AudioObjectID, _ rate: Double) -> OSStatus {
        var a = addr(kAudioDevicePropertyNominalSampleRate); var v = rate
        return AudioObjectSetPropertyData(obj, &a, 0, nil, 8, &v)
    }
    static func controls(_ dev: AudioObjectID) -> [AudioObjectID] {
        var a = addr(kAudioObjectPropertyControlList); var ids = [AudioObjectID](repeating: 0, count: 8); var size = UInt32(32)
        guard AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &ids) == noErr else { return [] }
        return Array(ids.prefix(Int(size) / 4))
    }
    static func plist(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector) -> Any? {
        var a = addr(sel); var p: Unmanaged<CFPropertyList>?; var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &p) == noErr, let v = p else { return nil }
        return v.takeRetainedValue()
    }
    @discardableResult static func setPlist(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector, _ value: CFPropertyList) -> OSStatus {
        var a = addr(sel); var v: CFPropertyList = value
        return AudioObjectSetPropertyData(dev, &a, 0, nil, UInt32(MemoryLayout<CFPropertyList>.size), &v)
    }
}

struct Digi003 {
    let device: AudioObjectID
    let clockControl: AudioObjectID?

    static func find() -> Digi003? {
        for d in CA.devices() where CA.string(d, kAudioDevicePropertyDeviceUID)?.hasPrefix("Digi003:") == true {
            let clock = CA.controls(d).first { CA.uint32($0, kAudioObjectPropertyClass) == kAudioClockSourceControlClassID }
            return Digi003(device: d, clockControl: clock)
        }
        return nil
    }
    var status: [String: Any] { (CA.plist(device, selStatus) as? [String: Any]) ?? [:] }
}

// MARK: - Meters view

final class MeterView: NSView {
    var levels = [Double](repeating: -144, count: 8)   // dBFS, analog inputs 1-8
    var holds = [Double](repeating: -144, count: 8)
    var holdAge = [Int](repeating: 0, count: 8)
    var clip = [Bool](repeating: false, count: 8)

    override var intrinsicContentSize: NSSize { NSSize(width: 280, height: 96) }

    func update(_ db: [Double]) {
        for c in 0..<8 {
            let v = c < db.count ? db[c] : -144
            levels[c] = max(v, levels[c] - 3)                 // ~50 dB/s fall at 16 fps
            if v >= holds[c] { holds[c] = v; holdAge[c] = 0 } else { holdAge[c] += 1; if holdAge[c] > 24 { holds[c] -= 2 } }
            if v >= -0.1 { clip[c] = true }
        }
        needsDisplay = true
    }
    func resetClips() { clip = [Bool](repeating: false, count: 8); needsDisplay = true }

    override func mouseDown(with event: NSEvent) { resetClips() }

    override func draw(_ dirty: NSRect) {
        let left: CGFloat = 20, top: CGFloat = 8, barH: CGFloat = 60, gap: CGFloat = 6
        let barW = (bounds.width - left - 14 - gap * 7) / 8
        let floor = -60.0
        func y(_ db: Double) -> CGFloat { CGFloat(max(0, min(1, (db - floor) / -floor))) * barH }
        let label: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedDigitSystemFont(ofSize: 9, weight: .regular), .foregroundColor: NSColor.secondaryLabelColor]
        for c in 0..<8 {
            let x = left + CGFloat(c) * (barW + gap)
            let base = bounds.height - top - barH
            NSColor.quaternaryLabelColor.setFill()
            NSBezierPath(roundedRect: NSRect(x: x, y: base, width: barW, height: barH), xRadius: 2, yRadius: 2).fill()
            let h = y(levels[c])
            let color: NSColor = levels[c] > -3 ? .systemRed : levels[c] > -12 ? .systemYellow : .systemGreen
            color.setFill()
            NSBezierPath(roundedRect: NSRect(x: x, y: base, width: barW, height: h), xRadius: 2, yRadius: 2).fill()
            if holds[c] > floor { NSColor.labelColor.setFill(); NSRect(x: x, y: base + y(holds[c]) - 1, width: barW, height: 1.5).fill() }
            if clip[c] { NSColor.systemRed.setFill(); NSRect(x: x, y: base + barH + 2, width: barW, height: 3).fill() }
            let t = "\(c + 1)" as NSString
            let sz = t.size(withAttributes: label)
            t.draw(at: NSPoint(x: x + (barW - sz.width) / 2, y: base - 14), withAttributes: label)
        }
        // scale
        for db in [0.0, -12, -24, -48] {
            let s = "\(Int(db))" as NSString
            s.draw(at: NSPoint(x: 0, y: bounds.height - top - barH + y(db) - 5), withAttributes: label)
        }
        let cap = L("Entradas analógicas (clic = borrar clip)", "Analog inputs (click = clear clip)") as NSString
        cap.draw(at: NSPoint(x: left, y: 0), withAttributes: label)
    }
}

// MARK: - MIDI bridge (Core MIDI <-> driver custom properties)

final class MIDIBridge {
    private var client = MIDIClientRef(), source = MIDIEndpointRef(), dest = MIDIEndpointRef()
    private var device: AudioObjectID = 0
    private var listener: AudioObjectPropertyListenerBlock?
    private let queue = DispatchQueue(label: "Digi003.midi")
    private var keepAlive: Timer?
    private(set) var running = false
    // incoming byte stream -> complete MIDI messages
    private var status: UInt8 = 0, need = 0, msg: [UInt8] = [], sysex: [UInt8] = [], inSysex = false

    func start() {
        guard !running else { return }
        MIDIClientCreateWithBlock("Digi 003" as CFString, &client) { _ in }
        MIDISourceCreate(client, "Digi 003 MIDI In" as CFString, &source)
        MIDIDestinationCreateWithBlock(client, "Digi 003 MIDI Out" as CFString, &dest) { [weak self] list, _ in
            var bytes: [UInt8] = []
            for p in UnsafeMutablePointer(mutating: list).unsafeSequence() {
                withUnsafeBytes(of: p.pointee.data) { raw in bytes.append(contentsOf: raw.prefix(Int(p.pointee.length))) }
            }
            self?.send(bytes)
        }
        running = true
        keepAlive = Timer.scheduledTimer(withTimeInterval: 4, repeats: true) { [weak self] _ in self?.ping() }
        RunLoop.main.add(keepAlive!, forMode: .common)
        attach()
    }

    func stop() {
        guard running else { return }
        keepAlive?.invalidate(); keepAlive = nil
        detach()
        if device != 0 { CA.setPlist(device, selKeepAlive, 0 as NSNumber) }
        MIDIEndpointDispose(source); MIDIEndpointDispose(dest); MIDIClientDispose(client)
        running = false
    }

    // (Re)binds to the current device id; called periodically so reconnects are picked up.
    func attach() {
        guard running else { return }
        let id = Digi003.find()?.device ?? 0
        if id == device { if id != 0 { ping() }; return }
        detach()
        device = id
        guard id != 0 else { return }
        var a = addr(selMidiIn)
        let block: AudioObjectPropertyListenerBlock = { [weak self] _, _ in self?.pull() }
        AudioObjectAddPropertyListenerBlock(id, &a, queue, block)
        listener = block
        ping(); pull()
    }
    private func detach() {
        if device != 0, let l = listener { var a = addr(selMidiIn); AudioObjectRemovePropertyListenerBlock(device, &a, queue, l) }
        listener = nil; device = 0
    }
    private func ping() { if device != 0 { CA.setPlist(device, selKeepAlive, 10 as NSNumber) } }

    private func send(_ bytes: [UInt8]) {
        let filtered = bytes.filter { $0 != 0xFE }      // active sensing is pointless over this link
        guard device != 0, !filtered.isEmpty else { return }
        CA.setPlist(device, selMidiOut, Data(filtered) as CFData)
    }

    private func pull() {
        guard device != 0, let d = CA.plist(device, selMidiIn) as? Data, !d.isEmpty else { return }
        for b in d { parse(b) }
    }

    private func emit(_ m: [UInt8]) {
        var list = MIDIPacketList()
        let p = MIDIPacketListInit(&list)
        _ = MIDIPacketListAdd(&list, MemoryLayout<MIDIPacketList>.size, p, 0, m.count, m)
        MIDIReceived(source, &list)
    }

    private func parse(_ b: UInt8) {
        if b >= 0xF8 { emit([b]); return }                                  // realtime, may interleave anything
        if inSysex {
            if b == 0xF7 { sysex.append(b); emit(sysex); sysex = []; inSysex = false; return }
            if b & 0x80 != 0 { sysex = []; inSysex = false }                // aborted sysex, fall through
            else { sysex.append(b); if sysex.count >= 200 { emit(sysex); sysex = [] }; return }
        }
        if b == 0xF0 { inSysex = true; sysex = [b]; return }
        if b & 0x80 != 0 {
            switch b {
            case 0xF1, 0xF3: status = 0; msg = [b]; need = 1
            case 0xF2: status = 0; msg = [b]; need = 2
            case 0xF6: emit([b]); status = 0; need = 0
            case 0xF4, 0xF5, 0xF7: status = 0; need = 0
            default: status = b; msg = [b]; need = (b & 0xF0 == 0xC0 || b & 0xF0 == 0xD0) ? 1 : 2
            }
            return
        }
        if need == 0 { guard status != 0 else { return }; msg = [status]; need = (status & 0xF0 == 0xC0 || status & 0xF0 == 0xD0) ? 1 : 2 }
        msg.append(b); need -= 1
        if need == 0 { emit(msg); msg = [] }
    }
}

extension UnsafeMutablePointer where Pointee == MIDIPacketList {
    func unsafeSequence() -> [UnsafePointer<MIDIPacket>] {
        var out: [UnsafePointer<MIDIPacket>] = []
        var p = UnsafeMutableRawPointer(self).advanced(by: MemoryLayout<UInt32>.size).assumingMemoryBound(to: MIDIPacket.self)
        for _ in 0..<pointee.numPackets { out.append(UnsafePointer(p)); p = MIDIPacketNext(p) }
        return out
    }
}

// MARK: - Keep-alive client
// With no HAL client, macOS throttles the driver's host process (priority 4) and the 003's real-time tx thread misses
// its deadlines, which makes the 003 buffer stale packets (latency jumps + clicks). A silent input-only IOProc keeps the
// host process at audio priority while preamp mode or MIDI needs the stream without any app using the 003.
final class KeepAliveClient {
    private var device: AudioObjectID = 0
    private var procID: AudioDeviceIOProcID?
    var active: Bool { procID != nil }

    func update(want: Bool) {
        let dev = want ? (Digi003.find()?.device ?? 0) : 0
        if dev == device && (dev == 0) == (procID == nil) { return }
        stop()
        guard dev != 0 else { return }
        var id: AudioDeviceIOProcID?
        guard AudioDeviceCreateIOProcIDWithBlock(&id, dev, nil, { _, _, _, _, _ in }) == noErr, let pid = id else { return }
        // input only: an output stream would also hold the "audio playing" sleep assertion
        var a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyIOProcStreamUsage, mScope: kAudioObjectPropertyScopeOutput, mElement: kAudioObjectPropertyElementMain)
        var size: UInt32 = 0
        if AudioObjectGetPropertyDataSize(dev, &a, 0, nil, &size) == noErr, size > 0 {
            let raw = UnsafeMutableRawPointer.allocate(byteCount: Int(size), alignment: 8); defer { raw.deallocate() }
            let usage = raw.assumingMemoryBound(to: AudioHardwareIOProcStreamUsage.self)
            usage.pointee.mIOProc = unsafeBitCast(pid, to: UnsafeMutableRawPointer.self)
            if AudioObjectGetPropertyData(dev, &a, 0, nil, &size, raw) == noErr {
                let n = Int(usage.pointee.mNumberStreams)
                withUnsafeMutablePointer(to: &usage.pointee.mStreamIsOn) { p in
                    let on = UnsafeMutableRawPointer(p).assumingMemoryBound(to: UInt32.self)
                    for i in 0..<n { on[i] = 0 }
                }
                AudioObjectSetPropertyData(dev, &a, 0, nil, size, raw)
            }
        }
        guard AudioDeviceStart(dev, pid) == noErr else { AudioDeviceDestroyIOProcID(dev, pid); return }
        device = dev; procID = pid
    }
    func stop() {
        if let p = procID, device != 0 { AudioDeviceStop(device, p); AudioDeviceDestroyIOProcID(device, p) }
        procID = nil; device = 0
    }
}

// MARK: - FireWire health (works even when the driver has no device)
// A hung 003 stays electrically on the bus (it shows up in the self-ID packets) but stops answering, so no
// IOFireWireDevice is published for it. Seen twice in testing; only a power cycle of the 003 recovers it.
enum BusState { case noAdapter, noDevice, hung, notResponding, ok }

final class FireWireHealth {
    private var hungSince: Date?, stalledSince: Date?

    /// Number of nodes on the FireWire bus (self-ID packets), or nil when there is no FireWire controller.
    func nodeCount() -> Int? {
        var it: io_iterator_t = 0
        guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOFireWireController"), &it) == KERN_SUCCESS else { return nil }
        defer { IOObjectRelease(it) }
        var result: Int?
        while case let svc = IOIteratorNext(it), svc != 0 {
            defer { IOObjectRelease(svc) }
            guard let d = IORegistryEntryCreateCFProperty(svc, "FireWire Self IDs" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? Data else { result = result ?? 1; continue }
            var phys = Set<UInt32>()
            for i in stride(from: 0, to: d.count - 3, by: 4) {
                let q = UInt32(d[i]) << 24 | UInt32(d[i + 1]) << 16 | UInt32(d[i + 2]) << 8 | UInt32(d[i + 3])
                if q >> 30 == 2 && (q >> 23) & 1 == 0 { phys.insert((q >> 24) & 0x3f) }   // self-ID packet #0 of a node
            }
            result = max(result ?? 0, max(phys.count, 1))
        }
        return result
    }
    /// Is a Digidesign unit published on the bus (i.e. its config ROM answered)?
    func digiOnBus() -> Bool {
        let m = IOServiceMatching("IOFireWireDevice") as NSMutableDictionary
        m["Vendor_ID"] = 0x00a07e
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, m)
        if svc != 0 { IOObjectRelease(svc); return true }
        return false
    }

    func evaluate(halPresent: Bool, wantStream: Bool, streaming: Bool) -> BusState {
        let now = Date()
        guard let nodes = nodeCount() else { hungSince = nil; stalledSince = nil; return .noAdapter }
        if !digiOnBus() {
            stalledSince = nil
            if nodes >= 2 {                                  // something is on the cable but never answered
                if hungSince == nil { hungSince = now }
                return now.timeIntervalSince(hungSince!) > 10 ? .hung : .noDevice
            }
            hungSince = nil
            return .noDevice
        }
        hungSince = nil
        if halPresent && wantStream && !streaming {
            if stalledSince == nil { stalledSince = now }
            return now.timeIntervalSince(stalledSince!) > 15 ? .notResponding : .ok
        }
        stalledSince = nil
        return .ok
    }
}

// MARK: - App

let clockNames = [L("Interno", "Internal"), "S/PDIF", "ADAT", "Word Clock"]
let agentLabel = "io.github.jonathanbeiko.digi003.menu"
let agentURL = FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/LaunchAgents/\(agentLabel).plist")
let defaults = UserDefaults.standard

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
    let menu = NSMenu()
    let meters = MeterView(frame: NSRect(x: 0, y: 0, width: 280, height: 96))
    let midi = MIDIBridge()
    let keepAlive = KeepAliveClient()
    let health = FireWireHealth()
    var bus: BusState = .ok, wasBus: BusState = .ok
    var timer: Timer?, meterTimer: Timer?
    var sleepAssertion: IOPMAssertionID = 0
    // previous state for notifications
    var wasPresent: Bool?, wasLocked: Bool?

    var notifyOn: Bool { defaults.object(forKey: "notifications") as? Bool ?? true }
    var preventSleep: Bool { defaults.bool(forKey: "preventSleep") }
    var midiOn: Bool { defaults.object(forKey: "midi") as? Bool ?? true }

    func applicationDidFinishLaunching(_ n: Notification) {
        menu.delegate = self
        item.menu = menu
        if notifyOn { UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in } }
        if midiOn && !headless { midi.start() }
        tick()
        if CommandLine.arguments.contains("--print-menu") {     // headless check of the menu texts
            build()
            func dump(_ m: NSMenu, _ indent: String) {
                for it in m.items {
                    if it.isSeparatorItem { print(indent + "────"); continue }
                    if it.view != nil { print(indent + "[medidores / meters]"); continue }
                    print(indent + (it.state == .on ? "✓ " : "  ") + it.title)
                    if let sub = it.submenu { dump(sub, indent + "    ") }
                }
            }
            dump(menu, ""); print("ícono / icon: " + (item.button?.toolTip ?? ""))
            exit(0)
        }
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(timer!, forMode: .common)
    }

    // Periodic: icon, notifications, sleep assertion, MIDI rebind.
    func tick() {
        let dev = Digi003.find()
        let st = dev?.status ?? [:]
        let present = dev != nil
        let streaming = st["streaming"] as? Bool ?? false
        let clock = Int(st["clock"] as? Double ?? 0)
        let route = Int(st["route"] as? Double ?? 0)
        let locked = !(streaming && clock != 0) || (st["extDetected"] as? Bool ?? false)
        bus = health.evaluate(halPresent: present, wantStream: route != 0 || midi.running, streaming: streaming)

        let symbol: String, tip: String
        if bus == .hung || bus == .notResponding { symbol = "exclamationmark.octagon"; tip = L("003: no responde, apágalo y préndelo", "003: not responding, power-cycle it") }
        else if bus == .noAdapter { symbol = "waveform.slash"; tip = L("003: no hay adaptador FireWire", "003: no FireWire adapter") }
        else if !present { symbol = "waveform.slash"; tip = L("003: no conectado", "003: not connected") }
        else if !locked { symbol = "exclamationmark.triangle"; tip = L("003: sin reloj externo", "003: no external clock") }
        else if streaming { symbol = "waveform.circle.fill"; tip = L("003: transmitiendo", "003: streaming") }
        else { symbol = "waveform.circle"; tip = L("003: conectado, en reposo", "003: connected, idle") }
        item.button?.image = NSImage(systemSymbolName: symbol, accessibilityDescription: tip)
        item.button?.image?.isTemplate = true
        item.button?.toolTip = tip

        if notifyOn {
            if let was = wasPresent, was != present { post(present ? L("Digi 003 conectado", "Digi 003 connected") : L("Digi 003 desconectado", "Digi 003 disconnected"), present ? L("El 003 volvió a aparecer en el bus FireWire.", "The 003 is back on the FireWire bus.") : L("Revisa el cable FireWire y que el 003 esté encendido.", "Check the FireWire cable and that the 003 is powered on.")) }
            if present, let was = wasLocked, was != locked { post(locked ? L("Reloj externo recuperado", "External clock recovered") : L("Se perdió el reloj externo", "External clock lost"), locked ? L("El 003 está enganchado otra vez.", "The 003 is locked again.") : L("El 003 no recibe \(clockNames[min(clock, 3)]). Revisa el cable óptico o la frecuencia de la interfaz maestra.", "The 003 is not receiving \(clockNames[min(clock, 3)]). Check the optical cable or the clock master's sample rate.")) }
        }
        if notifyOn, bus != wasBus {
            switch bus {
            case .hung: post(L("El Digi 003 no responde", "The Digi 003 is not responding"), L("Está conectado al FireWire pero no contesta. Apágalo, espera 10 segundos y préndelo.", "It is on the FireWire bus but does not answer. Turn it off, wait 10 seconds and turn it back on."))
            case .notResponding: post(L("El Digi 003 dejó de transmitir", "The Digi 003 stopped streaming"), L("El driver no logra arrancar el stream. Prueba \"Reiniciar 003\"; si no funciona, apágalo y préndelo.", "The driver cannot start the stream. Try \"Restart 003\"; if that fails, power-cycle it."))
            case .ok where wasBus == .hung || wasBus == .notResponding: post(L("Digi 003 recuperado", "Digi 003 recovered"), L("El 003 volvió a responder.", "The 003 is responding again."))
            default: break
            }
        }
        wasBus = bus
        wasPresent = present; wasLocked = present ? locked : nil

        let wantAwake = preventSleep && present && route != 0
        if wantAwake && sleepAssertion == 0 {
            IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep as CFString, IOPMAssertionLevel(kIOPMAssertionLevelOn), L("Digi 003: previos por ADAT", "Digi 003: preamps over ADAT") as CFString, &sleepAssertion)
        } else if !wantAwake && sleepAssertion != 0 { IOPMAssertionRelease(sleepAssertion); sleepAssertion = 0 }

        midi.attach()
        if !headless { keepAlive.update(want: present && (route != 0 || midi.running)) }
    }

    func post(_ title: String, _ body: String) {
        let c = UNMutableNotificationContent(); c.title = title; c.body = body
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: UUID().uuidString, content: c, trigger: nil))
    }

    // MARK: menu

    func menuNeedsUpdate(_ menu: NSMenu) { build() }
    func menuWillOpen(_ menu: NSMenu) {
        meterTimer = Timer(timeInterval: 1.0 / 16, repeats: true) { [weak self] _ in self?.pollMeters() }
        RunLoop.main.add(meterTimer!, forMode: .common)
    }
    func menuDidClose(_ menu: NSMenu) { meterTimer?.invalidate(); meterTimer = nil }

    func pollMeters() {
        guard let d = Digi003.find(), let lv = CA.plist(d.device, selLevels) as? [String: Any], let input = lv["in"] as? [Double] else { return }
        meters.update(Array(input.prefix(8)))
    }

    func build() {
        menu.removeAllItems()
        guard let dev = Digi003.find() else {
            switch bus {
            case .noAdapter:
                menu.addItem(disabled: L("Digi 003 Rack — no hay adaptador FireWire", "Digi 003 Rack — no FireWire adapter"))
                menu.addItem(disabled: L("Conecta el adaptador Thunderbolt→FireWire en un puerto TRASERO", "Plug the Thunderbolt→FireWire adapter into a real Thunderbolt port"))
            case .hung:
                menu.addItem(disabled: L("⚠︎ Digi 003 Rack — conectado pero NO RESPONDE", "⚠︎ Digi 003 Rack — connected but NOT RESPONDING"))
                menu.addItem(disabled: L("Apágalo, espera 10 segundos y préndelo", "Turn it off, wait 10 seconds and turn it back on"))
            default:
                menu.addItem(disabled: L("Digi 003 Rack — no conectado", "Digi 003 Rack — not connected"))
                menu.addItem(disabled: L("Revisa que esté encendido y el cable FireWire (se afloja fácil)", "Check that it is on and the FireWire cable (it comes loose easily)"))
            }
            footer(); return
        }
        let st = dev.status
        let streaming = st["streaming"] as? Bool ?? false
        let rate = st["rate"] as? Double ?? 0
        let clock = Int(st["clock"] as? Double ?? 0)
        let route = Int(st["route"] as? Double ?? 0)
        let optical = Int(st["optical"] as? Double ?? 0)
        let lowLat = st["lowLatency"] as? Bool ?? false
        let clients = Int(st["clients"] as? Double ?? 0)

        menu.addItem(disabled: "Digi 003 Rack — " + (streaming ? L("transmitiendo", "streaming") : (st["asleep"] as? Bool == true ? L("Mac en reposo", "Mac asleep") : L("conectado, en reposo", "connected, idle"))))
        if bus == .notResponding {
            menu.addItem(disabled: L("⚠︎ No logra transmitir: usa \"Reiniciar 003\"; si sigue, apágalo y préndelo", "⚠︎ Not streaming: use \"Restart 003\"; if it persists, power-cycle it"))
        }
        var line = String(format: L("%.1f kHz · reloj %@", "%.1f kHz · clock %@"), rate / 1000, clockNames[min(max(clock, 0), 3)])
        if streaming && clock != 0 {
            if st["extDetected"] as? Bool == true {
                let er = st["extRate"] as? Double ?? 0
                line += er == rate ? L(" · enganchado ✓", " · locked ✓") : String(format: L(" · llega %.1f kHz ⚠︎", " · receiving %.1f kHz ⚠︎"), er / 1000)
            } else { line += L(" · sin señal ⚠︎", " · no signal ⚠︎") }
        }
        menu.addItem(disabled: line)
        if streaming {
            var info = clients > 0 ? L("\(clients) app(s) usando el 003", "\(clients) app(s) using the 003") : (route != 0 ? L("sin apps (modo previos)", "no apps (preamp mode)") : L("sin apps (MIDI)", "no apps (MIDI)"))
            if let ms = st["loopMs"] as? Double, route != 0 { info += String(format: L(" · previos +%.1f ms", " · preamps +%.1f ms"), ms) }
            let errs = Int(st["rxErrors"] as? Double ?? 0) + Int(st["rxOverruns"] as? Double ?? 0)
            if errs > 0 { info += " · \(errs) err." }
            menu.addItem(disabled: info)
        }

        if streaming {
            menu.addItem(.separator())
            let mi = NSMenuItem(); mi.view = meters
            menu.addItem(mi)
        }
        menu.addItem(.separator())

        // ADAT routing
        let adat = NSMenu()
        for (title, value, tip) in [(L("Desde el DAW (interfaz normal)", "From the DAW (normal interface)"), 0, L("Las salidas ADAT 1-8 del 003 llevan lo que mande tu DAW", "The 003 ADAT outputs 1-8 carry what your DAW sends")),
                                    (L("Previos 1-8 → salida ADAT", "Preamps 1-8 → ADAT out"), 8, L("Las entradas analógicas 1-8 salen por ADAT 1-8, aunque ninguna app use el 003", "Analog inputs 1-8 go out ADAT 1-8, even when no app uses the 003")),
                                    (L("Previos 1-4 → salida ADAT, DAW en 5-8", "Preamps 1-4 → ADAT out, DAW on 5-8"), 4, L("Analógicas 1-4 por ADAT 1-4; ADAT 5-8 quedan para el DAW", "Analog 1-4 on ADAT 1-4; ADAT 5-8 stay with the DAW"))] {
            let m = NSMenuItem(title: title, action: #selector(setRoute(_:)), keyEquivalent: "")
            m.target = self; m.tag = value; m.state = value == route ? .on : .off; m.toolTip = tip; m.isEnabled = optical == 0
            adat.addItem(m)
        }
        menu.addItem(submenu: L("Salida ADAT", "ADAT output") + (route != 0 ? L(" (previos)", " (preamps)") : ""), adat)

        let lat = NSMenu()
        for (title, value) in [(L("Segura (~7.5 ms)", "Safe (~7.5 ms)"), false), (L("Baja (~5.5 ms, más riesgo de clics)", "Low (~5.5 ms, more risk of clicks)"), true)] {
            let m = NSMenuItem(title: title, action: #selector(setLatency(_:)), keyEquivalent: "")
            m.target = self; m.representedObject = value; m.state = value == lowLat ? .on : .off
            lat.addItem(m)
        }
        menu.addItem(submenu: L("Latencia", "Latency"), lat)

        let clockMenu = NSMenu()
        for (i, name) in clockNames.enumerated() {
            let m = NSMenuItem(title: name, action: #selector(setClock(_:)), keyEquivalent: "")
            m.target = self; m.tag = i; m.state = i == clock ? .on : .off
            m.isEnabled = !(optical == 1 && i == 2)
            clockMenu.addItem(m)
        }
        menu.addItem(submenu: L("Reloj", "Clock"), clockMenu)

        let rateMenu = NSMenu()
        for r in [44100.0, 48000.0] {
            let m = NSMenuItem(title: String(format: "%.1f kHz", r / 1000), action: #selector(setRate(_:)), keyEquivalent: "")
            m.target = self; m.representedObject = r; m.state = r == rate ? .on : .off
            rateMenu.addItem(m)
        }
        let rateItem = menu.addItem(submenu: L("Frecuencia", "Sample rate"), rateMenu)
        if clock != 0 { rateItem.toolTip = L("Con reloj externo el 003 sigue la frecuencia que le llega", "With an external clock the 003 follows the incoming rate") }

        let opt = NSMenu()
        for (title, value) in [(L("ADAT (8 canales)", "ADAT (8 channels)"), 0), (L("S/PDIF óptico (2 canales)", "Optical S/PDIF (2 channels)"), 1)] {
            let m = NSMenuItem(title: title, action: #selector(setOptical(_:)), keyEquivalent: "")
            m.target = self; m.tag = value; m.state = value == optical ? .on : .off
            opt.addItem(m)
        }
        menu.addItem(submenu: L("Puerto óptico", "Optical port"), opt)

        menu.addItem(.separator())
        menu.addItem(toggle: L("MIDI (puertos \"Digi 003 MIDI\")", "MIDI (\"Digi 003 MIDI\" ports)"), on: midi.running, target: self, action: #selector(toggleMidi))
        menu.addItem(toggle: L("Evitar reposo con previos activos", "Prevent sleep while preamps are active"), on: preventSleep, target: self, action: #selector(togglePreventSleep))
        menu.addItem(toggle: L("Avisos", "Notifications"), on: notifyOn, target: self, action: #selector(toggleNotify))
        let rs = NSMenuItem(title: L("Reiniciar 003", "Restart 003"), action: #selector(restart), keyEquivalent: ""); rs.target = self; rs.isEnabled = streaming || bus == .notResponding
        menu.addItem(rs)
        footer()
    }

    func footer() {
        menu.addItem(.separator())
        let ams = NSMenuItem(title: L("Abrir Configuración de Audio MIDI", "Open Audio MIDI Setup"), action: #selector(openAMS), keyEquivalent: ""); ams.target = self
        menu.addItem(ams)
        menu.addItem(toggle: L("Abrir al iniciar sesión", "Open at login"), on: FileManager.default.fileExists(atPath: agentURL.path), target: self, action: #selector(toggleLogin))
        menu.addItem(.separator())
        menu.addItem(NSMenuItem(title: L("Salir", "Quit"), action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q"))
    }

    // MARK: actions

    @objc func setRoute(_ s: NSMenuItem) { if let d = Digi003.find() { CA.setPlist(d.device, selRoute, s.tag as NSNumber) }; tick() }
    @objc func setLatency(_ s: NSMenuItem) { if let d = Digi003.find(), let v = s.representedObject as? Bool { CA.setPlist(d.device, selLowLat, (v ? 1 : 0) as NSNumber) } }
    @objc func setOptical(_ s: NSMenuItem) { if let d = Digi003.find() { CA.setPlist(d.device, selOptical, s.tag as NSNumber) } }
    @objc func restart() { if let d = Digi003.find() { CA.setPlist(d.device, selRestart, 1 as NSNumber) } }
    @objc func setClock(_ s: NSMenuItem) {
        guard let d = Digi003.find(), let c = d.clockControl else { return }
        CA.setUInt32(c, kAudioSelectorControlPropertyCurrentItem, UInt32(s.tag)); tick()
    }
    @objc func setRate(_ s: NSMenuItem) { if let d = Digi003.find(), let r = s.representedObject as? Double { CA.setRate(d.device, r) } }
    @objc func toggleMidi() { if midi.running { midi.stop() } else { midi.start() }; defaults.set(midi.running, forKey: "midi") }
    @objc func togglePreventSleep() { defaults.set(!preventSleep, forKey: "preventSleep"); tick() }
    @objc func toggleNotify() {
        defaults.set(!notifyOn, forKey: "notifications")
        if notifyOn { UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in } }
    }
    @objc func openAMS() { NSWorkspace.shared.open(URL(fileURLWithPath: "/System/Applications/Utilities/Audio MIDI Setup.app")) }
    @objc func toggleLogin() {
        let fm = FileManager.default
        if fm.fileExists(atPath: agentURL.path) { try? fm.removeItem(at: agentURL); return }
        let exe = Bundle.main.executablePath ?? CommandLine.arguments[0]
        let plist: [String: Any] = ["Label": agentLabel, "ProgramArguments": [exe], "RunAtLoad": true, "LimitLoadToSessionType": "Aqua"]
        try? fm.createDirectory(at: agentURL.deletingLastPathComponent(), withIntermediateDirectories: true)
        (plist as NSDictionary).write(to: agentURL, atomically: true)
    }
}

extension NSMenu {
    func addItem(disabled title: String) { let m = NSMenuItem(title: title, action: nil, keyEquivalent: ""); m.isEnabled = false; addItem(m) }
    @discardableResult func addItem(submenu title: String, _ sub: NSMenu) -> NSMenuItem {
        let m = NSMenuItem(title: title, action: nil, keyEquivalent: ""); m.submenu = sub; addItem(m); return m
    }
    func addItem(toggle title: String, on: Bool, target: AnyObject, action: Selector) {
        let m = NSMenuItem(title: title, action: action, keyEquivalent: ""); m.target = target; m.state = on ? .on : .off; addItem(m)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()

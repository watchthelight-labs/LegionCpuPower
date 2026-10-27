// <wtl> project: legioncpupower | file: legionfan/LegionFan.swift | author: watchthelight </wtl>
// LegionFan: menu bar fan and CPU control (Liquid Glass panel, macOS 26) for the Legion 5 15IAX10. Shows both fan speeds and the CPU, switches the fans
// between Auto (the EC's curve), Low, Full and a custom curve (with an editor), and sets the CPU profile, the P-core
// preference, the heat guard and (with nvgfx-boost installed) the NVIDIA GPU boost. It writes /Library/Application Support/LegionFan/settings; the root LaunchDaemon
// org.watchthelight.legionfan applies it to LegionCpuPower (also at boot, before login).
import SwiftUI
import AppKit
import Combine
import Darwin

private let settingsPath = "/Library/Application Support/LegionFan/settings"

func sysctlString(_ name: String) -> String? {
    var len = 0
    guard sysctlbyname(name, nil, &len, nil, 0) == 0, len > 0 else { return nil }
    var buf = [CChar](repeating: 0, count: len)
    guard sysctlbyname(name, &buf, &len, nil, 0) == 0 else { return nil }
    return String(cString: buf)
}

enum Mode: String, CaseIterable {
    case auto, low, full, curve
    var title: String {
        switch self {
        case .auto: return "Auto (firmware curve)"
        case .low: return "Low"
        case .full: return "Full speed"
        case .curve: return "Custom curve"
        }
    }
    var short: String { ["auto": "Auto", "low": "Low", "full": "Full", "curve": "Curve"][rawValue]! }
}

enum Profile: String, CaseIterable {
    case performance, balanced, saver
    var title: String {
        switch self {
        case .performance: return "Performance"
        case .balanced: return "Balanced"
        case .saver: return "Battery saver"
        }
    }
    var short: String { ["performance": "Performance", "balanced": "Balanced", "saver": "Saver"][rawValue]! }
}

final class Fan: ObservableObject {
    @Published var rpm1 = 0
    @Published var rpm2 = 0
    @Published var kextMode = -1
    @Published var hot = false
    @Published var mode: Mode = .auto
    @Published var curve: [Double] = Array(repeating: 5, count: 10)
    @Published var profile: Profile = .balanced
    @Published var sched = true
    @Published var guardOn = true
    @Published var cpuTemp = "-"
    @Published var cpuWatts = ""
    @Published var gpuBoost = true
    @Published var gpuBoostHeld: Bool?
    let gpuBoostInstalled = FileManager.default.isExecutableFile(atPath: "/usr/local/libexec/nvgfx-boost")
    @Published var error: String?
    private var timer: Timer?

    init() {
        load()
        refresh()
        timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.refresh() }
    }

    func load() {
        guard let text = try? String(contentsOfFile: settingsPath, encoding: .utf8) else { return }
        for line in text.split(separator: "\n") {
            let kv = line.split(separator: "=", maxSplits: 1).map(String.init)
            guard kv.count == 2 else { continue }
            if kv[0] == "mode", let m = Mode(rawValue: kv[1]) { mode = m }
            if kv[0] == "curve" {
                let v = kv[1].split(separator: ",").compactMap { Double($0) }
                if v.count == 10 { curve = v }
            }
            if kv[0] == "profile", let p = Profile(rawValue: kv[1]) { profile = p }
            if kv[0] == "sched" { sched = kv[1] != "0" }
            if kv[0] == "guard" { guardOn = kv[1] != "0" }
            if kv[0] == "gpuboost" { gpuBoost = kv[1] != "0" }
        }
    }

    func refresh() {
        guard let s = sysctlString("debug.legioncpu.status") else { error = "LegionCpuPower not loaded"; return }
        error = nil
        if let r = s.range(of: #"\((\d+) / (\d+) rpm\)"#, options: .regularExpression) {
            let n = s[r].split(whereSeparator: { !$0.isNumber }).compactMap { Int($0) }
            if n.count == 2 { rpm1 = n[0]; rpm2 = n[1] }
        }
        if let r = s.range(of: #"fan mode \d"#, options: .regularExpression), let d = s[r].last?.wholeNumberValue {
            kextMode = d
        }
        hot = s.contains("(hot: full)")
        if let st = try? String(contentsOfFile: "/Library/Application Support/LegionFan/gpuboost.state", encoding: .utf8) {
            gpuBoostHeld = st.hasPrefix("boost 1")
        }
        if let r = s.range(of: #"pkg -?\d+ C -?\d+ W"#, options: .regularExpression) {
            let n = s[r].split(separator: " ")
            if n.count == 5 { cpuTemp = "\(n[1]) °C"; cpuWatts = "\(n[3]) W" }
        }
    }

    // the EC's current curve, to start editing from
    func curveFromKext() {
        guard let c = sysctlString("debug.legioncpu.fancurve") else { return }
        let v = c.split(separator: ",").compactMap { Double($0.trimmingCharacters(in: .whitespacesAndNewlines)) }
        if v.count == 10 { curve = v }
    }

    var upper: Double { max(10, (curve.max() ?? 10) * 1.5).rounded() }

    func save() {
        let c = curve.map { String(Int($0.rounded())) }.joined(separator: ",")
        let text = "mode=\(mode.rawValue)\ncurve=\(c)\nprofile=\(profile.rawValue)\nsched=\(sched ? 1 : 0)\n"
            + "guard=\(guardOn ? 1 : 0)\ngpuboost=\(gpuBoost ? 1 : 0)\n"
        // write beside, then rename: the daemon never reads a half-written file
        let tmp = settingsPath + ".new"
        do {
            try text.write(toFile: tmp, atomically: false, encoding: .utf8)
            _ = rename(tmp, settingsPath)
        } catch {
            self.error = "cannot write settings (run install.sh)"
        }
    }

    func set(_ m: Mode) { mode = m; save() }
    func set(_ p: Profile) { profile = p; save() }
}

// Control Center style: no panel background. Every capsule and circle sits on AppKit's NSGlassEffectView, the same
// view the system uses there: it samples and refracts what is behind the window (the desktop, other apps), with the lit,
// bending rim. SwiftUI's glassEffect only sees this window's own content, which is clear here, so it draws flat.
final class GlassView: NSGlassEffectView {
    var capsule = true
    override func layout() {
        super.layout()
        if capsule { cornerRadius = min(bounds.height, bounds.width) / 2 }
    }
}

struct NativeGlass: NSViewRepresentable {
    var cornerRadius: CGFloat? = nil   // nil = capsule / circle
    // clear glass: little blur, full refraction and rim light; the dark tint cancels the clear style's brightening
    var tint: NSColor? = NSColor.black.withAlphaComponent(0.3)
    var opacity: CGFloat = 1
    func makeNSView(context: Context) -> GlassView {
        let v = GlassView()
        v.style = .clear
        return v
    }
    func updateNSView(_ v: GlassView, context: Context) {
        v.capsule = cornerRadius == nil
        if let r = cornerRadius { v.cornerRadius = r }
        v.tintColor = tint
        v.alphaValue = opacity
        v.needsLayout = true
    }
}

// the round icon of a module: white with an accent symbol when on (like AirDrop), glass when off
struct IconDot: View {
    let symbol: String
    var on: Bool
    var size: CGFloat = 38
    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.42, weight: .semibold))
            .foregroundStyle(on ? Color.accentColor : .primary)
            .frame(width: size, height: size)
            .background { if on { Circle().fill(.white) } else { NativeGlass() } }
    }
}

// a capsule module like Wi-Fi / Bluetooth / AirDrop
struct Module: View {
    let symbol: String, title: String, subtitle: String
    var on: Bool
    var action: (() -> Void)? = nil
    var body: some View {
        HStack(spacing: 10) {
            IconDot(symbol: symbol, on: on)
            VStack(alignment: .leading, spacing: 1) {
                Text(title).font(.headline).lineLimit(1)
                Text(subtitle).font(.subheadline).foregroundStyle(.secondary).monospacedDigit().lineLimit(1)
                    .contentTransition(.numericText())
            }
            .minimumScaleFactor(0.8)
            Spacer(minLength: 0)
        }
        .padding(8)
        .frame(maxWidth: .infinity)
        .background(NativeGlass())
        .contentShape(.capsule)
        .onTapGesture { action?() }
    }
}

// a title and a row of glass capsules; the chosen one is filled with the accent colour
struct Choices<T: Hashable>: View {
    let title: String, items: [T], selected: T, label: (T) -> String, pick: (T) -> Void
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline).padding(.leading, 6)
            HStack(spacing: 6) {
                ForEach(items, id: \.self) { item in
                    let on = item == selected
                    Text(label(item))
                        .font(.system(size: 13, weight: on ? .semibold : .regular))
                        .foregroundStyle(on ? .white : .primary)
                        .lineLimit(1).minimumScaleFactor(0.8)
                        .frame(maxWidth: .infinity, minHeight: 32)
                        .background { if on { Capsule().fill(Color.accentColor) } else { NativeGlass() } }
                        .contentShape(.capsule)
                        .onTapGesture { withAnimation(.snappy) { pick(item) } }
                }
            }
        }
    }
}

// a round glass button
struct RoundButton: View {
    let symbol: String, help: String, action: () -> Void
    var body: some View {
        IconDot(symbol: symbol, on: false, size: 34).contentShape(.circle).onTapGesture(perform: action).help(help)
    }
}

struct PanelView: View {
    @ObservedObject var fan: Fan
    var showCurve: () -> Void

    var body: some View {
        Group {
            VStack(spacing: 12) {
                if let e = fan.error {
                    Module(symbol: "exclamationmark.triangle.fill", title: "LegionCpuPower", subtitle: e, on: true)
                }
                HStack(spacing: 10) {
                    Module(symbol: fan.hot || fan.mode == .full ? "fan.fill" : "fan", title: "Fans",
                           subtitle: "\(fan.rpm1) · \(fan.rpm2) rpm", on: fan.mode == .full || fan.hot) {
                        fan.set(fan.mode == .full ? .auto : .full)
                    }
                    Module(symbol: "cpu", title: "CPU", subtitle: "\(fan.cpuTemp) · \(fan.cpuWatts)", on: fan.profile == .performance) {
                        fan.set(fan.profile == .performance ? .balanced : .performance)
                    }
                }
                Choices(title: "Fan mode", items: Mode.allCases, selected: fan.mode, label: { $0.short }, pick: { fan.set($0) })
                Choices(title: "CPU profile", items: Profile.allCases, selected: fan.profile, label: { $0.short }, pick: { fan.set($0) })
                HStack(spacing: 10) {
                    Module(symbol: "bolt.fill", title: "P-cores", subtitle: fan.sched && fan.profile != .saver ? "On" : "Off", on: fan.sched && fan.profile != .saver) {
                        if fan.profile != .saver { fan.sched.toggle(); fan.save() }
                    }
                    Module(symbol: "thermometer.medium", title: "Heat guard", subtitle: fan.guardOn ? "On" : "Off (benchmarks)", on: fan.guardOn) { fan.guardOn.toggle(); fan.save() }
                }
                HStack(spacing: 10) {
                    if fan.gpuBoostInstalled {
                        Module(symbol: "gauge.with.dots.needle.67percent", title: "GPU boost",
                               subtitle: fan.gpuBoost ? (fan.gpuBoostHeld == false ? "On (not held)" : "On") : "Off", on: fan.gpuBoost) { fan.gpuBoost.toggle(); fan.save() }
                    }
                    Spacer(minLength: 0)
                    RoundButton(symbol: "slider.vertical.3", help: "Edit fan curve", action: showCurve)
                    RoundButton(symbol: "power", help: "Quit LegionFan") { NSApp.terminate(nil) }
                }
            }
            .padding(12)
        }
        .background(NativeGlass(cornerRadius: 28))
        .frame(width: 330)
        .fixedSize(horizontal: false, vertical: true)
    }
}

struct CurveView: View {
    @ObservedObject var fan: Fan

    var body: some View {
        GlassEffectContainer(spacing: 12) {
            VStack(alignment: .leading, spacing: 14) {
                Text("Ten points, cooler on the left, hotter on the right, in the EC's fan levels.")
                    .font(.callout).foregroundStyle(.secondary)
                HStack(alignment: .bottom, spacing: 10) {
                    ForEach(0..<10, id: \.self) { i in
                        VStack(spacing: 6) {
                            Text("\(Int(fan.curve[i].rounded()))").monospacedDigit().font(.callout.weight(.semibold))
                            Slider(value: $fan.curve[i], in: 0...fan.upper, step: 1)
                                .rotationEffect(.degrees(-90))
                                .frame(width: 140, height: 24)
                                .frame(width: 24, height: 140)
                            Text("\(i + 1)").font(.caption).foregroundStyle(.secondary)
                        }
                        .padding(.vertical, 10).padding(.horizontal, 6)
                        .glassEffect(.regular.interactive())
                    }
                }
                HStack {
                    Button("Load current") { fan.curveFromKext() }.buttonStyle(.glass)
                    Spacer()
                    Button("Apply as custom curve") { fan.set(.curve) }.buttonStyle(.glassProminent)
                        .keyboardShortcut(.defaultAction)
                }
                Text("At 95 C for 10 s the fans go to full speed until the CPU is below 80 C for a minute.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            .padding(20)
        }
        .frame(minWidth: 480)
    }
}

// A borderless panel that can become key: glass and tints only draw in their active form in the key window.
final class KeyPanel: NSPanel {
    override var canBecomeKey: Bool { true }
}

// Own status item and a borderless clear panel: MenuBarExtra always draws its own material behind the content.
final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    let fan = Fan()
    private var item: NSStatusItem!
    private var panel: NSPanel!
    private var curveWindow: NSWindow?
    private var monitor: Any?
    private var sink: Any?

    func applicationDidFinishLaunching(_ note: Notification) {
        item = NSStatusBar.system.statusItem(withLength: NSStatusItem.squareLength)
        item.button?.target = self
        item.button?.action = #selector(toggle)
        updateIcon()
        sink = fan.objectWillChange.sink { [weak self] in DispatchQueue.main.async { self?.updateIcon() } }

        panel = KeyPanel(contentRect: .zero, styleMask: [.borderless], backing: .buffered, defer: true)
        panel.delegate = self
        panel.isOpaque = false
        panel.backgroundColor = .clear
        panel.hasShadow = false
        panel.level = .statusBar
        panel.collectionBehavior = [.canJoinAllSpaces, .transient, .ignoresCycle]
        let host = NSHostingView(rootView: PanelView(fan: fan, showCurve: { [weak self] in self?.showCurve() }))
        host.sizingOptions = [.preferredContentSize]
        panel.contentView = host
    }

    private func updateIcon() {
        let on = fan.mode == .full || fan.hot
        item.button?.image = NSImage(systemSymbolName: on ? "fan.fill" : "fan", accessibilityDescription: "LegionFan")
    }

    @objc private func toggle() {
        if panel.isVisible { close(); return }
        guard let button = item.button, let bw = button.window else { return }
        let size = panel.contentView?.fittingSize ?? NSSize(width: 330, height: 400)
        let b = bw.convertToScreen(button.convert(button.bounds, to: nil))
        let screen = bw.screen?.visibleFrame ?? .zero
        var x = b.midX - size.width / 2
        x = min(max(x, screen.minX + 6), screen.maxX - size.width - 6)
        panel.setFrame(NSRect(x: x, y: b.minY - size.height - 4, width: size.width, height: size.height), display: true)
        NSApp.activate(ignoringOtherApps: true)
        panel.makeKeyAndOrderFront(nil)
        monitor = NSEvent.addGlobalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown]) { [weak self] _ in self?.close() }
    }

    func windowDidResignKey(_ note: Notification) {
        if (note.object as? NSWindow) === panel { close() }
    }

    private func close() {
        panel.orderOut(nil)
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
    }

    private func showCurve() {
        close()
        if curveWindow == nil {
            let w = NSWindow(contentRect: .zero, styleMask: [.titled, .closable, .fullSizeContentView], backing: .buffered, defer: true)
            w.title = "Fan curve"
            w.titlebarAppearsTransparent = true
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: CurveView(fan: fan))
            w.center()
            curveWindow = w
        }
        NSApp.activate(ignoringOtherApps: true)
        curveWindow?.makeKeyAndOrderFront(nil)
    }
}

@main
struct LegionFanApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    var body: some Scene { Settings { EmptyView() } }
}

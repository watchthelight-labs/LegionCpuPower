// <wtl> project: legion-hackintosh | file: native/tools/legionfan/LegionFan.swift | author: watchthelight </wtl>
// LegionFan: menu bar fan control for the Legion 5 15IAX10. Shows both fan speeds, switches between Auto (the EC's
// curve), Low, Full and a custom curve, and edits that curve. It writes /Library/Application Support/LegionFan/settings;
// the root LaunchDaemon org.watchthelight.legionfan applies it to LegionCpuPower (also at boot, before login).
import SwiftUI
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
}

final class Fan: ObservableObject {
    @Published var rpm1 = 0
    @Published var rpm2 = 0
    @Published var kextMode = -1
    @Published var hot = false
    @Published var mode: Mode = .auto
    @Published var curve: [Double] = Array(repeating: 5, count: 10)
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
        let text = "mode=\(mode.rawValue)\ncurve=\(c)\n"
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
}

struct MenuView: View {
    @ObservedObject var fan: Fan
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        if let e = fan.error { Text(e) }
        Text("Fans \(fan.rpm1) / \(fan.rpm2) rpm" + (fan.hot ? " (hot: full speed)" : ""))
        Divider()
        ForEach(Mode.allCases, id: \.self) { m in
            Button { fan.set(m) } label: {
                Text((fan.mode == m ? "✓ " : "    ") + m.title)
            }
        }
        Divider()
        Button("Edit curve…") {
            openWindow(id: "curve")
            NSApp.activate(ignoringOtherApps: true)
        }
        Divider()
        Button("Quit") { NSApp.terminate(nil) }
    }
}

struct CurveView: View {
    @ObservedObject var fan: Fan

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Ten points, cooler on the left, hotter on the right. Values are the EC's own units.")
                .font(.callout).foregroundStyle(.secondary)
            HStack(alignment: .bottom, spacing: 14) {
                ForEach(0..<10, id: \.self) { i in
                    VStack {
                        Text("\(Int(fan.curve[i].rounded()))").monospacedDigit()
                        Slider(value: $fan.curve[i], in: 0...fan.upper, step: 1)
                            .rotationEffect(.degrees(-90))
                            .frame(width: 140, height: 24)
                            .frame(width: 24, height: 140)
                        Text("\(i + 1)").font(.caption).foregroundStyle(.secondary)
                    }
                }
            }
            HStack {
                Button("Load current") { fan.curveFromKext() }
                Spacer()
                Button("Apply as custom curve") { fan.set(.curve) }.keyboardShortcut(.defaultAction)
            }
            Text("At 95 C for 10 s the fans go to full speed until the CPU is below 80 C for a minute.")
                .font(.caption).foregroundStyle(.secondary)
        }
        .padding(20)
        .frame(minWidth: 460)
    }
}

@main
struct LegionFanApp: App {
    @StateObject private var fan = Fan()

    var body: some Scene {
        MenuBarExtra {
            MenuView(fan: fan)
        } label: {
            Image(systemName: fan.mode == .full || fan.hot ? "fan.fill" : "fan")
        }
        Window("Fan curve", id: "curve") {
            CurveView(fan: fan)
        }
        .windowResizability(.contentSize)
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// Config.toml as the Settings page edits it: every change is written at once, as the Quest
/// launcher's settings page does, and the game reads the file when it is loaded at Play.
@MainActor
final class ConfigStore: ObservableObject {
    @Published private(set) var config = TomlConfig(text: "")
    @Published private(set) var error = ""

    func reload() {
        GameStorage.prepare()
        config = GameStorage.loadConfig() ?? TomlConfig(text: "")
    }

    func edit(_ change: (inout TomlConfig) -> Void) {
        var updated = config
        change(&updated)
        config = updated
        error = GameStorage.save(updated) ?? ""
    }

    // Readers with the runtime's defaults and ranges (runtime/include/runtime_config.h).
    func bool(_ section: String, _ key: String, default value: Bool) -> Bool {
        config.bool(section, key) ?? value
    }
    func number(_ section: String, _ key: String, in range: ClosedRange<Double>, default value: Double) -> Double {
        config.number(section, key).flatMap { range.contains($0) ? $0 : nil } ?? value
    }
    /// Unrecognised strings fall back to the default option, as in the runtime.
    func index(_ section: String, _ key: String, of values: [String], default value: Int = 0) -> Int {
        config.string(section, key).flatMap { values.firstIndex(of: $0) } ?? value
    }
}

/// Settings for the things the Vision Pro build supports, laid out as the Quest launcher's
/// Settings page (android/.../launcher/SettingsPage.kt). Quest-only options (foveation, the
/// runtime performance level, Touch-controller hand tracking) are left out.
struct SettingsView: View {
    @StateObject private var store = ConfigStore()
    @EnvironmentObject private var model: GameModel

    private static let rotations = ["yaw", "yaw_pitch", "full"]
    private static let seats = ["cockpit", "custom"]
    private static let controllerModes = ["wii_remote", "gamepad", "none"]
    private static let interpolation: [Int64] = [0, 1, 72, 90, 120]
    private static let resolutions: [Double] = [1.0, 1.5, 2.0, 3.0, 4.0]
    private static let supportedResolutions: [Double] = [0.0, 1.0, 1.5, 2.0, 3.0, 4.0, 6.0, 8.0]
    private static let bloomPath: Int64 = 0x10
    private static let volumes: [(String, String)] = [
        ("Master", "volume"), ("Music", "music_volume"), ("Sound effects", "sound_effects_volume"),
        ("Voices", "voices_volume"), ("Menu sounds", "ui_volume"),
    ]

    var body: some View {
        Form {
            Section {
                Text("Changes are saved to Config.toml at once and apply when you press Play. Most of them can also be changed during the game in the settings panel.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                if !store.error.isEmpty {
                    Label(store.error, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                }
            }
            camera
            headset
            screen
            rendering
            controls
            volume
            files
            diagnostics
        }
        .onAppear { store.reload() }
    }

    // MARK: Sections

    private var immersive: Bool { !store.bool("vr", "flat_screen", default: false) }
    private var window: Bool { immersive && store.bool("vr", "immersive_window", default: false) }
    private var firstPerson: Bool { immersive && store.bool("vr", "first_person", default: false) }
    private var cockpit: Bool { firstPerson && store.index("vr", "first_person_seat", of: Self.seats) == 0 }
    private var handSteering: Bool { cockpit && store.bool("vr", "hand_steering", default: true) }

    private var camera: some View {
        Section("Camera") {
            // One setting in two keys, read as runtime_config.h's VrRaceView reads them.
            ChoiceRow(title: "Race view",
                      help: "Immersive plays races all around you in stereo. Immersive window keeps that stereo view but shows it only through a window where the menu screen sits, with your room around it. Flat screen plays races on the menu screen; the camera settings below do not apply to it.",
                      options: ["Immersive", "Immersive window", "Flat screen"],
                      selection: binding(get: { !immersive ? 2 : (window ? 1 : 0) },
                                         set: { index in store.edit {
                                             $0.setBool("vr", "flat_screen", index == 2)
                                             $0.setBool("vr", "immersive_window", index == 1)
                                         } }))
            ChoiceRow(title: "Camera", help: "Ride behind the kart like the game, or sit in the driver's seat.",
                      options: ["Chase camera", "First person"],
                      selection: binding(get: { store.bool("vr", "first_person", default: false) ? 1 : 0 },
                                         set: { index in store.edit { $0.setBool("vr", "first_person", index == 1) } }))
                .disabled(!immersive)
            ChoiceRow(title: "Follow kart rotation",
                      help: "In first person, follow turning only (the comfortable choice), turning and climbing, or all kart rotation including banking.",
                      options: ["Turning", "Turning and climbing", "Everything"],
                      selection: stringChoice("vr", "first_person_rotation", Self.rotations, default: 1))
                .disabled(!firstPerson)
            // One setting in two keys, presented as the in-game panel's two tick boxes are.
            ChoiceRow(title: "Hide in first person", help: "Your driver sits where your eyes are. Other racers are never hidden.",
                      options: ["Nothing", "Driver", "Driver and kart"],
                      selection: binding(get: {
                          let hide = store.bool("vr", "first_person_hide_driver", default: true)
                          let model = store.config.integer("vr", "first_person_hidden_model").flatMap { (-1...31).contains($0) ? $0 : nil } ?? 0
                          return !hide ? 0 : (model < 0 ? 2 : 1)
                      }, set: { index in store.edit {
                          $0.setBool("vr", "first_person_hide_driver", index != 0)
                          if index != 0 { $0.setInteger("vr", "first_person_hidden_model", index == 1 ? 0 : -1) }
                      } }))
                .disabled(!firstPerson)
            ChoiceRow(title: "Seat",
                      help: "The cockpit puts you at the driver's eyes, life-size, with the steering wheel or handlebar turning within reach. Custom uses the head offsets from the in-game settings panel.",
                      options: ["Cockpit", "Custom"],
                      selection: stringChoice("vr", "first_person_seat", Self.seats))
                .disabled(!firstPerson)
            ToggleRow(title: "Hand steering",
                      help: "In the cockpit, close a hand on the steering wheel or handlebar to take hold of it, and turn it to steer. Hand steering by heurazy.",
                      isOn: boolBinding("vr", "hand_steering", default: true))
                .disabled(!cockpit)
            ToggleRow(title: "Drive with your hands",
                      help: "Race with no controller: a hand closed on the wheel holds the gas, a pinch with an open free hand uses an item, flicking your hands up does a trick, and a little-finger pinch pauses. In menus, a pinch is A. Choose Automatic drift. Off, the finger pinches are a controller's buttons and a fist near the wheel still takes hold of it.",
                      isOn: boolBinding("vr", "hand_tracking", default: true))
                .disabled(!handSteering)
            SliderRow(title: "Lean back angle", help: "Tilts the race view back for playing reclined. 0 applies no tilt.",
                      range: -45...45, step: 1, format: { String(format: "%.0f°", $0) },
                      value: numberBinding("vr", "lean_back_degrees", in: -45...45, default: 0))
                .disabled(!immersive)
        }
    }

    private var headset: some View {
        Section("Headset") {
            SliderRow(title: "Headset render scale",
                      help: "Scales the eye resolution the Vision Pro recommends. Higher is sharper and costs GPU time; lower it if races stutter.",
                      range: 0.25...2.0, step: 0.05, format: { String(format: "%.2fx", $0) },
                      value: numberBinding("vr", "render_scale", in: 0.25...2.0, default: 1.0))
            ChoiceRow(title: "VR frame interpolation",
                      help: "Renders extra frames between game frames at the headset's rate. Needs GPU headroom and adds one game frame of latency.",
                      options: ["Off", "Auto", "72 FPS", "90 FPS", "120 FPS"],
                      selection: binding(get: { Self.interpolation.firstIndex(of: interpolationFps) ?? 0 },
                                         set: { index in store.edit { $0.setInteger("vr", "frame_interpolation_fps", Self.interpolation[index]) } }))
        }
    }

    private var screen: some View {
        Section("Virtual screen") {
            // The immersive window is that screen and always carries the HUD.
            ToggleRow(title: "Race HUD on the virtual screen",
                      help: "Puts the minimap, position and item roulette on a screen in front of you instead of stretching them across the view.",
                      isOn: boolBinding("vr", "hud_virtual_screen", default: true))
                .disabled(!immersive || window)
            SliderRow(title: "Screen distance", help: "How far the menu screen and race HUD sit in front of you.",
                      range: 0.5...5.0, step: 0.1, format: { String(format: "%.1f m", $0) },
                      value: numberBinding("vr", "hud_distance_meters", in: 0.25...10.0, default: 2.0))
            SliderRow(title: "Screen width", help: "How wide the menu screen and race HUD are.",
                      range: 1.0...6.0, step: 0.1, format: { String(format: "%.1f m", $0) },
                      value: numberBinding("vr", "hud_width_meters", in: 0.25...20.0, default: 2.4))
            // Not in Config.toml: the style the app opens its immersive space with (GameModel).
            ToggleRow(title: "Show my room around the menu screen",
                      help: "Opens the game in your room (mixed immersion) rather than fully immersive. Races are fully immersive either way. Applies the next time you press Play.",
                      isOn: Binding(get: { model.wantsRoom }, set: { model.wantsRoom = $0 }))
                .disabled(model.phase == .running)
            ToggleRow(title: "Passthrough around the menu screen",
                      help: "Shows your room around the menus instead of black, when \"Show my room\" above is on. Immersive races stay fully virtual; the immersive window and the flat screen race have the room around them too.",
                      isOn: boolBinding("vr", "passthrough", default: true))
        }
    }

    private var rendering: some View {
        Section("Rendering") {
            ChoiceRow(title: "Internal resolution",
                      help: "The game's own render size. Native keeps the headset at its best frame rate.",
                      options: Self.resolutions.map { $0 == 1.0 ? "Native" : Self.multiplier($0) },
                      selection: binding(get: { Self.resolutions.firstIndex(of: resolution) ?? -1 },
                                         set: { index in store.edit { $0.setFloat("video", "resolution_multiplier", Self.resolutions[index]) } }),
                      custom: Self.resolutions.contains(resolution) ? nil : (resolution == 0 ? "Auto" : Self.multiplier(resolution)))
            ToggleRow(title: "Widescreen", help: "16:9 menus and races instead of 4:3.",
                      isOn: boolBinding("video", "widescreen", default: true))
            ToggleRow(title: "Disable bloom", help: "Bloom's bright glow reads poorly in a headset, so it starts off.",
                      isOn: binding(get: { disabledPostProcessing & Self.bloomPath != 0 },
                                    set: { on in store.edit { $0.setInteger("video", "disabled_post_processing_paths", on ? Self.bloomPath : 0) } }))
            ToggleRow(title: "Prevent shader stutters",
                      help: "Skips a draw for a moment while its shader compiles instead of pausing the game.",
                      isOn: boolBinding("video", "skip_unready_pipelines", default: true))
            // runtime_config.h's GxThread defaults off everywhere but Android.
            ToggleRow(title: "Graphics thread",
                      help: "Prepares the drawing on a second CPU core so busy scenes keep their speed.",
                      isOn: boolBinding("video", "gx_thread", default: false))
        }
    }

    private var controls: some View {
        Section("Controls") {
            ChoiceRow(title: "Hands as controllers",
                      help: "What your hands are to the game. None leaves the game to a Bluetooth game controller, the way to race; look at a menu button, pinch, adjust by moving your hand, and let go to press it either way.",
                      options: ["Wii Remote + Nunchuk", "Gamepad", "None"],
                      selection: stringChoice("vr", "controller_mode", Self.controllerModes))
            ToggleRow(title: "Vibration", help: "The game's rumble vibrates a connected game controller.",
                      isOn: boolBinding("controller", "rumble", default: true))
            InfoRow(title: "Pointer", value: "Look and pinch, adjust by hand, release to press")
            InfoRow(title: "A (right) / X (left)", value: "Middle-finger pinch")
            InfoRow(title: "B (right) / Y (left)", value: "Ring-finger pinch")
            InfoRow(title: "Menu", value: "Little-finger pinch")
            InfoRow(title: "Grip", value: "Fist")
        }
    }

    private var volume: some View {
        Section("Volume") {
            ForEach(Self.volumes, id: \.1) { label, key in
                SliderRow(title: label, help: nil, range: 0...100, step: 1, format: { String(format: "%.0f%%", $0) },
                          value: binding(get: { store.number("audio", key, in: 0...1, default: 1) * 100 },
                                         set: { value in store.edit { $0.setFloat("audio", key, value / 100) } }))
            }
            ToggleRow(title: "Mute", help: "Silences the game without changing the volumes above.",
                      isOn: boolBinding("audio", "muted", default: false))
        }
    }

    private var files: some View {
        Section("Files") {
            let status: String = switch GameStorage.discStatus {
            case .ready: "Extracted disc found"
            case .incomplete: "Incomplete: DATA needs both sys/ and files/"
            case .missing: "Missing"
            }
            InfoRow(title: "Game data", value: status)
            InfoRow(title: "Disc (DATA)", value: GameStorage.discDirectory.path, monospaced: true)
            InfoRow(title: "Config.toml", value: GameStorage.configFile.path, monospaced: true)
            InfoRow(title: "Logs", value: GameStorage.logsDirectory.path, monospaced: true)
        }
    }

    private var diagnostics: some View {
        Section("Diagnostics") {
            ToggleRow(title: "OpenXR diagnostic logging",
                      help: "Writes VR frame timing to the run log once per second: late, skipped and black frames, tracking loss. Turn it on to report stutter, and off again afterwards.",
                      isOn: boolBinding("diagnostics", "openxr_logging", default: false))
        }
    }

    // MARK: Values the runtime derives

    private var resolution: Double {
        store.config.number("video", "resolution_multiplier").flatMap { Self.supportedResolutions.contains($0) ? $0 : nil } ?? 1.0
    }

    /// RuntimeUserConfig's disabledPostProcessingPaths: only the bloom bit is accepted.
    private var disabledPostProcessing: Int64 {
        store.config.integer("video", "disabled_post_processing_paths")
            .flatMap { (0...0xFFFF_FFFF).contains($0) && ($0 & ~Self.bloomPath) == 0 ? $0 : nil } ?? Self.bloomPath
    }

    /// vr.frame_interpolation_fps, with the legacy frame_interpolation switch.
    private var interpolationFps: Int64 {
        let value = store.config.integer("vr", "frame_interpolation_fps").flatMap { (0...0xFFFF_FFFF).contains($0) ? $0 : nil }
            ?? store.config.bool("vr", "frame_interpolation").map { $0 ? 1 : 0 } ?? 0
        return Self.interpolation.contains(value) ? value : 0
    }

    private static func multiplier(_ value: Double) -> String {
        var text = TomlConfig.formatFloat(value)
        if text.hasSuffix(".0") { text.removeLast(2) }
        return text + "x"
    }

    // MARK: Bindings

    private func binding<T>(get: @escaping () -> T, set: @escaping (T) -> Void) -> Binding<T> {
        Binding(get: get, set: set)
    }
    private func boolBinding(_ section: String, _ key: String, default value: Bool) -> Binding<Bool> {
        binding(get: { store.bool(section, key, default: value) },
                set: { on in store.edit { $0.setBool(section, key, on) } })
    }
    private func numberBinding(_ section: String, _ key: String, in range: ClosedRange<Double>, default value: Double) -> Binding<Double> {
        binding(get: { store.number(section, key, in: range, default: value) },
                set: { number in store.edit { $0.setFloat(section, key, number) } })
    }
    private func stringChoice(_ section: String, _ key: String, _ values: [String], default value: Int = 0) -> Binding<Int> {
        binding(get: { store.index(section, key, of: values, default: value) },
                set: { index in store.edit { $0.setString(section, key, values[index]) } })
    }
}

// MARK: Rows

private struct HelpText: View {
    let text: String?
    var body: some View {
        if let text {
            Text(text).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// Title and description on the left, the control alone on the right. visionOS draws controls
/// a little in front of the window's glass, so seen at an angle they drift over whatever lies
/// next to them; keeping the text out of the control's column is what keeps it readable.
private struct ControlRow<Control: View>: View {
    let title: String
    let help: String?
    @ViewBuilder let control: () -> Control
    var body: some View {
        HStack(alignment: .center, spacing: 20) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title)
                HelpText(text: help)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            control()
                .fixedSize()
        }
        .padding(.vertical, 4)
    }
}

private struct ToggleRow: View {
    let title: String
    let help: String?
    @Binding var isOn: Bool
    var body: some View {
        ControlRow(title: title, help: help) {
            Toggle(title, isOn: $isOn).labelsHidden()
        }
    }
}

private struct ChoiceRow: View {
    let title: String
    let help: String?
    let options: [String]
    @Binding var selection: Int
    /// Shown when the file holds a value the options do not list (selection < 0).
    var custom: String? = nil
    var body: some View {
        ControlRow(title: title, help: help) {
            Picker(title, selection: $selection) {
                ForEach(options.indices, id: \.self) { Text(options[$0]).tag($0) }
                if let custom, selection < 0 {
                    Text(custom).tag(-1)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
        }
    }
}

private struct SliderRow: View {
    let title: String
    let help: String?
    let range: ClosedRange<Double>
    let step: Double
    let format: (Double) -> String
    @Binding var value: Double
    /// Written to the file when the slider is let go, not at every step of the drag.
    @State private var dragging: Double?
    // The text first and the bar last, so nothing lies beneath the bar for it to cover.
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(title)
                Spacer()
                Text(format(dragging ?? value)).monospacedDigit().foregroundStyle(.secondary)
            }
            HelpText(text: help)
            Slider(value: Binding(get: { dragging ?? min(max(value, range.lowerBound), range.upperBound) },
                                  set: { dragging = $0 }),
                   in: range, step: step) { editing in
                if !editing, let final = dragging {
                    value = final
                    dragging = nil
                }
            }
            .padding(.top, 4)
        }
        .padding(.vertical, 4)
    }
}

private struct InfoRow: View {
    let title: String
    let value: String
    var monospaced = false
    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title)
            Spacer(minLength: 16)
            Text(value)
                .font(monospaced ? .caption.monospaced() : .body)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.trailing)
                .textSelection(.enabled)
                .lineLimit(3)
                .truncationMode(.head)
        }
    }
}

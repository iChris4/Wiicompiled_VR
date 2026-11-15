// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// The window the app opens with: a Play tab (where the game files go, whether they
/// are there, and the button that opens the immersive space), a Profiles tab (Retro
/// Rewind's licences as it plays them online), a Miis tab (the game's Mii database and
/// its editor) and a Settings tab.
struct LauncherView: View {
    @EnvironmentObject private var model: GameModel

    var body: some View {
        TabView {
            PlayView()
                .tabItem { Label("Play", systemImage: "play.fill") }
            // Profiles are Retro Rewind's save, which a build without the mod never has.
            if model.offersRetroRewind {
                NavigationStack {
                    ProfilesView()
                }
                .tabItem { Label("Profiles", systemImage: "person.text.rectangle") }
            }
            NavigationStack {
                MiisView()
            }
            .tabItem { Label("Miis", systemImage: "person.crop.square") }
            NavigationStack {
                SettingsView()
                    .navigationTitle("Settings")
            }
            .tabItem { Label("Settings", systemImage: "gearshape") }
        }
    }
}

private struct PlayView: View {
    @EnvironmentObject private var model: GameModel
    @Environment(\.openImmersiveSpace) private var openImmersiveSpace
    @Environment(\.dismissImmersiveSpace) private var dismissImmersiveSpace
    @Environment(\.dismissWindow) private var dismissWindow

    var body: some View {
        ScrollView {
        VStack(alignment: .leading, spacing: 20) {
            Text("\(model.gameTitle) for Apple Vision Pro")
                .font(.largeTitle.bold())

            if model.offersRetroRewind {
                GroupBox("Game") {
                    VStack(alignment: .leading, spacing: 8) {
                        Picker("Game", selection: $model.selectedGame) {
                            ForEach(GameChoice.allCases) { choice in
                                Text(choice.pickerTitle).tag(choice)
                            }
                        }
                        .pickerStyle(.segmented)
                        .labelsHidden()
                        .disabled(model.phase != .idle)
                        Text("Retro Rewind is the community's expansion: the retro tracks, custom tracks and its own online play. It needs the extracted disc as well, plus its pack below.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            }

            GroupBox("Game files") {
                VStack(alignment: .leading, spacing: 8) {
                    Label(model.discPresent ? "Extracted disc found" : "No extracted disc yet",
                          systemImage: model.discPresent ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(model.discPresent ? .green : .orange)
                    Text("Copy the extracted Mario Kart Wii (PAL) disc, the folder holding sys/ and files/, into the DATA folder below with the Files app or through Finder file sharing. The Settings tab edits Config.toml next to it.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    PathRow(title: "Disc (DATA)", path: model.discDirectory)
                    PathRow(title: "Config.toml", path: model.configPath)
                    Button("Check again") { model.refreshDisc() }
                        .buttonStyle(.bordered)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            if model.selectedGame == .retroRewind {
                RetroRewindPackView()
            }

            GroupBox("How to play") {
                VStack(alignment: .leading, spacing: 8) {
                    Text("In the menus, look at a button and pinch; move your hand to adjust the pointer while pinching, and let go to press. Race with a Bluetooth game controller or with your hands (Settings > Camera). Press the Digital Crown to end the game. Whether your room shows around the menus is in Settings > Virtual screen.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }

            statusView

            HStack {
                Button {
                    Task { await start() }
                } label: {
                    Label("Play", systemImage: "play.fill")
                        .frame(minWidth: 120)
                }
                .buttonStyle(.borderedProminent)
                .disabled(!model.canStart)

                if model.phase == .running {
                    Button("Quit game", role: .destructive) {
                        model.requestQuit()
                        Task { await dismissImmersiveSpace() }
                    }
                    .buttonStyle(.bordered)
                }
            }
        }
        .padding(28)
        .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear {
            model.refreshDisc()
            // `simctl launch ... --autoplay` (or an Xcode scheme argument) presses Play for
            // an unattended run on the simulator, where nothing can pinch the button.
            if CommandLine.arguments.contains("--autoplay"), model.canStart {
                Task { await start() }
            }
        }
    }

    @ViewBuilder
    private var statusView: some View {
        switch model.phase {
        case .idle:
            if !model.lastError.isEmpty {
                Label(model.lastError, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
            }
        case .opening:
            Label("Opening the immersive space…", systemImage: "hourglass")
        case .running:
            Label("The game is running in the immersive space.", systemImage: "visionpro")
        case .ended(let code):
            Label(code == 0 ? "The game ended. Relaunch the app to play again."
                            : "The game ended with code \(code); see the Logs folder. Relaunch the app to play again.",
                  systemImage: "flag.checkered")
        case .failed(let message):
            Label(message.isEmpty ? "The game could not start." : message, systemImage: "xmark.octagon.fill")
                .foregroundStyle(.red)
        }
    }

    private func start() async {
        // The game framework is loaded now, after the Settings tab had its say.
        guard model.loadGame() else { return }
        model.markOpening()
        switch await openImmersiveSpace(id: GameModel.immersiveSpaceID) {
        case .opened:
            // Out of the way once the game is on: visionOS only lets the last scene go
            // after the space has opened, which is now. The game's audio is anchored to
            // the listener, not this window (GameModel), so it plays on without it.
            dismissWindow(id: GameModel.launcherWindowID)
        case .userCancelled:
            model.openingFailed("The immersive space was not opened.")
        case .error:
            model.openingFailed("visionOS could not open the immersive space.")
        @unknown default:
            model.openingFailed("visionOS could not open the immersive space.")
        }
    }
}

/// Retro Rewind's pack: what is installed, what the server offers this build, and the
/// download or update with its progress (GameModel, RetroRewindPack).
private struct RetroRewindPackView: View {
    @EnvironmentObject private var model: GameModel

    var body: some View {
        GroupBox("Retro Rewind pack") {
            VStack(alignment: .leading, spacing: 8) {
                statusLabel
                Text(description)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                PathRow(title: "Pack", path: model.packDirectory)

                if let progress = model.packProgress {
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(progress.title).font(.callout.bold())
                            Spacer()
                            Text(progress.detail).font(.callout).foregroundStyle(.secondary)
                        }
                        if let fraction = progress.fraction {
                            ProgressView(value: fraction)
                        } else {
                            ProgressView()
                        }
                    }
                    .padding(.top, 4)
                }

                if !model.packError.isEmpty {
                    Label(model.packError, systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                }

                HStack {
                    if model.packBusy {
                        Button("Cancel", role: .cancel) { model.cancelPackInstall() }
                            .buttonStyle(.bordered)
                    } else {
                        if let action = actionTitle {
                            Button(action) { model.installPack() }
                                .buttonStyle(.borderedProminent)
                                .disabled(model.phase != .idle)
                        }
                        Button("Check again") { model.refreshPack() }
                            .buttonStyle(.bordered)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .onAppear { model.refreshPack() }
    }

    @ViewBuilder
    private var statusLabel: some View {
        switch model.packStatus {
        case .notInstalled:
            Label("No Retro Rewind pack yet", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        case .ready(let version):
            if let update = model.packUpdate {
                Label("Retro Rewind \(version) installed; \(update) is available", systemImage: "arrow.down.circle.fill")
                    .foregroundStyle(.blue)
            } else {
                Label("Retro Rewind \(version) installed", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
            }
        case .mismatch(let version):
            Label("Retro Rewind \(version) does not match this app", systemImage: "xmark.octagon.fill").foregroundStyle(.red)
        }
    }

    private var description: String {
        let built = RetroRewindBuild.packVersion.isEmpty ? "" : " This app was built for Retro Rewind \(RetroRewindBuild.packVersion), so the pack stays at that version until the app is rebuilt."
        switch model.packStatus {
        case .notInstalled:
            return "The pack (about 2 GB: tracks, characters, menus) is downloaded from Retro Rewind's own server into the folder below; it is not part of this app. Downloading takes a while and needs about 5 GB free." + built
        case .ready:
            return "The pack is what the modded game reads next to the extracted disc." + built
        case .mismatch:
            return "The pack's Binaries/Code.pul is not the one this app's Retro Rewind was translated from, and the game cannot run on it. Rebuild the app from this pack (visionos/Build-VisionOS.sh with the RetroRewind6 folder), or remove the folder below with the Files app and download the pack again." + built
        }
    }

    private var actionTitle: String? {
        switch model.packStatus {
        case .notInstalled: return "Download Retro Rewind"
        case .ready: return model.packUpdate.map { "Update to \($0)" }
        case .mismatch: return nil
        }
    }
}

private struct PathRow: View {
    let title: String
    let path: String

    var body: some View {
        HStack(alignment: .firstTextBaseline) {
            Text(title).font(.callout.bold()).frame(width: 110, alignment: .leading)
            Text(path)
                .font(.caption.monospaced())
                .textSelection(.enabled)
                .lineLimit(2)
                .truncationMode(.head)
        }
    }
}

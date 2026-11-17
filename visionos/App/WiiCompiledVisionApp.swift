// SPDX-License-Identifier: GPL-3.0-or-later

import CompositorServices
import SwiftUI

/// The app: a launcher window (Play and Settings) and the immersive space the game
/// renders into. The game runtime is a framework the app embeds and loads at Play
/// (GameLibrary, runtime/cmake/PublicProducts.cmake), driven through GameModel.
@main
struct WiiCompiledVisionApp: App {
    @StateObject private var model = GameModel()
    @Environment(\.scenePhase) private var scenePhase

    var body: some Scene {
        WindowGroup(id: GameModel.launcherWindowID) {
            LauncherView()
                .environmentObject(model)
        }
        .windowResizability(.contentMinSize)
        .defaultSize(width: 820, height: 760)

        ImmersiveSpace(id: GameModel.immersiveSpaceID) {
            ImmersiveGame.layer(for: model)
        }
        // Always mixed immersion: the game's live [vr] passthrough setting alone
        // decides whether the room shows around the virtual screen and the
        // immersive window (the provider clears the frame transparent there) or
        // black does. Immersive races draw opaque eyes and cover it either way.
        .immersionStyle(selection: $model.immersionStyle, in: .mixed)
        // The wearer's own arms over the race, or the character body in their place
        // ([vr] real_arms, which the runtime reads too).
        .upperLimbVisibility(model.realArms ? .visible : .hidden)
    }
}

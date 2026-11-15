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
        // Mixed immersion shows the room around the virtual screen and the
        // immersive window when the game's passthrough setting asks for it (the
        // provider clears the frame transparent there); full immersion is the
        // headset with nothing else. The launcher offers the choice.
        .immersionStyle(selection: $model.immersionStyle, in: .mixed, .full)
        .upperLimbVisibility(.visible)
    }
}

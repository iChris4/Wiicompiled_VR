// SPDX-License-Identifier: GPL-3.0-or-later

import CompositorServices
import SwiftUI

/// The frame the game renders into. CompositorServices hands the layer renderer
/// to the OpenXR provider (runtime/src/vr/visionos), which paces frames, places
/// the eyes and composites the game's layers into the drawable. A
/// CompositorLayer is immersive space content, not a view, so the App's
/// ImmersiveSpace builds it through this helper.
enum ImmersiveGame {
    static func layer(for model: GameModel) -> CompositorLayer {
        CompositorLayer(configuration: GameLayerConfiguration()) { layerRenderer in
            model.attach(layerRenderer)
        }
    }
}

struct GameLayerConfiguration: CompositorLayerConfiguration {
    func makeConfiguration(capabilities: LayerRenderer.Capabilities,
                           configuration: inout LayerRenderer.Configuration) {
        // One texture per eye; the provider reads each view's texture index and viewport
        // from the drawable, so any layout would do, and this one keeps the two clear.
        configuration.layout = .dedicated
        // The compositor works in sRGB; the provider samples the game's sRGB swapchain images.
        configuration.colorFormat = .bgra8Unorm_srgb
        configuration.depthFormat = .depth32Float
        // Foveation would have the provider render through a rasterization rate map; it
        // draws full-view quads of the game's eye images instead, at the eyes' own resolution.
        configuration.isFoveationEnabled = false
    }
}

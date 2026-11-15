// SPDX-License-Identifier: GPL-3.0-or-later

import CoreGraphics
import Foundation
import SwiftUI

/// Mii pictures for the Miis tab and its editor, drawn by MiiRenderer off the main thread and
/// kept in a memory cache (MiiImagesSingletonService on the PC, MiiImages.kt on the Quest).
/// Nothing is drawn until the player has downloaded the Mii parts (MiiRenderResource); the
/// views then show their placeholder. Miis have their bodies once those are downloaded too.
final class MiiImages: @unchecked Sendable {
    static let shared = MiiImages()

    private final class Picture {
        let image: CGImage
        init(_ image: CGImage) { self.image = image }
    }

    /// A request a view may give up on before its turn, such as a tile scrolled away.
    private final class Request: @unchecked Sendable {
        private let lock = NSLock()
        private var cancelled = false
        var isCancelled: Bool {
            lock.lock()
            defer { lock.unlock() }
            return cancelled
        }
        func cancel() {
            lock.lock()
            cancelled = true
            lock.unlock()
        }
    }

    private let cache: NSCache<NSString, Picture> = {
        let cache = NSCache<NSString, Picture>()
        cache.totalCostLimit = 192 << 20
        return cache
    }()
    private let renderers = MiiImages.queue("MiiImages", width: 2)
    /// The editor's face: one at a time, so a burst of changes only draws the newest.
    private let previewer = MiiImages.queue("MiiPreview", width: 1)
    private let lock = NSLock()
    /// Changes with `clear`: a picture drawn before is shown but not kept.
    private var generation = 0

    private static func queue(_ name: String, width: Int) -> OperationQueue {
        let queue = OperationQueue()
        queue.name = name
        queue.maxConcurrentOperationCount = width
        queue.qualityOfService = .userInitiated
        return queue
    }

    static func headKey(_ mii: Mii, size: Int, withBody: Bool, pose: MiiRenderer.Pose = .front) -> String {
        "\(withBody ? "mii" : "head"):\(size):\(pose.key):\(mii.lookKey)"
    }

    static func partKey(_ mii: Mii, part: MiiRenderer.Part, index: Int, size: Int) -> String {
        "part:\(part):\(index):\(size):\(colour(of: mii, for: part))"
    }

    /// The colour a part's icon is drawn in, which is all of the Mii an icon depends on.
    private static func colour(of mii: Mii, for part: MiiRenderer.Part) -> Int {
        switch part {
        case .eyebrow: return mii.eyebrowColor
        case .eye: return mii.eyeColor
        case .mouth: return mii.lipColor
        case .glasses: return mii.glassesColor
        case .mustache: return mii.facialHairColor
        case .nose: return 0
        }
    }

    func cached(_ key: String) -> CGImage? {
        cache.object(forKey: key as NSString)?.image
    }

    /// `mii`, `size` pixels square and turned as `pose`, with its upper body unless not `withBody`.
    func head(_ mii: Mii, size: Int, withBody: Bool = true, pose: MiiRenderer.Pose = .front, preview: Bool = false) async -> CGImage? {
        let key = Self.headKey(mii, size: size, withBody: withBody, pose: pose)
        return await draw(key, on: preview ? previewer : renderers) { resource in
            try MiiRenderer.render(resource, mii, size: size, pose: pose, bodies: withBody ? MiiRenderResource.bodies() : nil)
        }
    }

    /// One choice of a face part (MiiRenderer.partIcon); nil for "none".
    func part(_ mii: Mii, part: MiiRenderer.Part, index: Int, size: Int) async -> CGImage? {
        await draw(Self.partKey(mii, part: part, index: index, size: size), on: renderers) { resource in
            try MiiRenderer.partIcon(resource, mii, part: part, index: index, size: size)
        }
    }

    /// Forgets every picture, for when the parts were just installed and placeholders can become Miis.
    func clear() {
        lock.lock()
        generation += 1
        lock.unlock()
        cache.removeAllObjects()
    }

    private var currentGeneration: Int {
        lock.lock()
        defer { lock.unlock() }
        return generation
    }

    private func draw(_ key: String, on queue: OperationQueue,
                      render: @escaping @Sendable (FflResource) throws -> [UInt32]?) async -> CGImage? {
        if let hit = cached(key) { return hit }
        let request = Request()
        let generation = currentGeneration
        return await withTaskCancellationHandler {
            await withCheckedContinuation { continuation in
                queue.addOperation {
                    // Given up on before its turn: a newer picture is wanted instead.
                    guard !request.isCancelled else {
                        continuation.resume(returning: nil)
                        return
                    }
                    continuation.resume(returning: self.drawNow(key, generation: generation, render: render))
                }
            }
        } onCancel: {
            request.cancel()
        }
    }

    private func drawNow(_ key: String, generation: Int, render: (FflResource) throws -> [UInt32]?) -> CGImage? {
        if let hit = cached(key) { return hit }
        guard let resource = MiiRenderResource.load(),
              let pixels = try? render(resource),
              let image = Self.image(pixels) else { return nil }
        // Drawn from what was installed before a clear: shown, but not kept.
        if currentGeneration == generation {
            cache.setObject(Picture(image), forKey: key as NSString, cost: pixels.count * 4)
        }
        return image
    }

    /// The renderer's non-premultiplied ARGB words as an image: little-endian words with alpha first.
    static func image(_ pixels: [UInt32]) -> CGImage? {
        let size = Int(Double(pixels.count).squareRoot())
        guard size > 0, size * size == pixels.count,
              let space = CGColorSpace(name: CGColorSpace.sRGB),
              let provider = CGDataProvider(data: pixels.withUnsafeBufferPointer({ Data(buffer: $0) }) as CFData) else { return nil }
        let info = CGBitmapInfo(rawValue: CGImageAlphaInfo.first.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)
        return CGImage(width: size, height: size, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: size * 4,
                       space: space, bitmapInfo: info, provider: provider, decode: nil, shouldInterpolate: true,
                       intent: .defaultIntent)
    }
}

/// A Mii's picture, drawn when it first shows; `placeholder` stands in until then, and for good
/// before the Mii parts are downloaded.
struct MiiPicture<Placeholder: View>: View {
    let mii: Mii
    /// The side of the square, in points.
    let side: CGFloat
    var withBody = true
    /// The editor's face: only its newest look is drawn.
    var preview = false
    /// Drawn this much larger than the square and cropped to it, so a head alone fills its cell.
    var zoom: CGFloat = 1
    /// How the Mii is turned: straight ahead in the lists, three-quarters on a profile.
    var pose: MiiRenderer.Pose = .front
    /// Whether the parts are installed, which the view is redrawn for when it changes.
    let installed: Bool
    @ViewBuilder let placeholder: () -> Placeholder

    @Environment(\.displayScale) private var displayScale
    @State private var image: CGImage?
    @State private var shownKey = ""

    private var pixels: Int {
        // The renderer takes even sizes from 16 to 4096.
        min(max(Int(side * zoom * displayScale) & ~1, 16), 4096)
    }

    private var key: String { MiiImages.headKey(mii, size: pixels, withBody: withBody, pose: pose) + (installed ? "" : ":none") }

    var body: some View {
        ZStack {
            if let image {
                Image(decorative: image, scale: displayScale)
                    .resizable()
                    .interpolation(.high)
                    .frame(width: side * zoom, height: side * zoom)
                    .offset(y: zoom > 1 ? -side * 0.07 : 0)
            } else if !installed || shownKey == key {
                placeholder()
            }
        }
        .frame(width: side, height: side)
        .clipped()
        .task(id: key) {
            guard installed else {
                image = nil
                return
            }
            // A picture already drawn shows at once; the editor keeps its last face until the next.
            if let hit = MiiImages.shared.cached(MiiImages.headKey(mii, size: pixels, withBody: withBody, pose: pose)) {
                image = hit
                shownKey = key
                return
            }
            if !preview { image = nil }
            let drawn = await MiiImages.shared.head(mii, size: pixels, withBody: withBody, pose: pose, preview: preview)
            guard !Task.isCancelled else { return }
            if drawn != nil || !preview { image = drawn }
            shownKey = key
        }
    }
}

/// One choice of a flat face part, drawn from the Mii parts in the Mii's colour.
struct MiiPartIcon: View {
    let mii: Mii
    let part: MiiRenderer.Part
    let index: Int
    let side: CGFloat

    @Environment(\.displayScale) private var displayScale
    @State private var image: CGImage?

    private var pixels: Int { min(max(Int(side * displayScale) & ~1, 16), 4096) }
    private var key: String { MiiImages.partKey(mii, part: part, index: index, size: pixels) }

    var body: some View {
        ZStack {
            if let image {
                Image(decorative: image, scale: displayScale).resizable().interpolation(.high)
            }
        }
        .frame(width: side, height: side)
        .task(id: key) {
            if let hit = MiiImages.shared.cached(key) {
                image = hit
                return
            }
            image = nil
            let drawn = await MiiImages.shared.part(mii, part: part, index: index, size: pixels)
            if !Task.isCancelled { image = drawn }
        }
    }
}

// SPDX-License-Identifier: GPL-3.0-or-later

import Dispatch
import Foundation

/// Draws a Mii from `FflResource` on the CPU: the PC launcher's NativeMiiRenderer, by way of the
/// Quest launcher's MiiRenderer.kt, with the same parts, face and mask textures, colours, camera,
/// lighting and rasteriser, so a Mii looks the same in every launcher.
///
/// Pictures are the PC's face framing: the head, and below it the upper body in the Mii's
/// favourite colour, from the 3DS bodies (`MiiBodies`) once they are downloaded; without them,
/// the head alone (the PC's face_only). One deliberate fix: the PC colours a beard with the hair
/// colour; here it has its facial hair colour, as on the Wii.
///
/// The arithmetic is the Kotlin's, operation for operation in single precision, so a picture is
/// the Quest's pixel for pixel: the helpers at the end of this file reproduce kotlin.math on the
/// JVM (float trigonometry and powers through double precision, round half to even, Math.max's
/// NaN and signed-zero rules, and Float.toInt()'s saturation).
enum MiiRenderer {
    /// How the Mii is turned and seen, in degrees, as the PC's MiiImageSpecifications turn it:
    /// the Mii about its X, Y and Z axes, and the camera orbiting its head.
    struct Pose: Hashable, Sendable {
        var characterX: Float
        var characterY: Float
        var characterZ: Float
        var cameraX: Float
        var cameraY: Float
        var cameraZ: Float
        /// The camera's distance as a share of the PC's (its CameraZoom): below 1 the Mii fills more.
        var zoom: Float

        init(characterX: Float = 0, characterY: Float = 0, characterZ: Float = 0,
             cameraX: Float = 0, cameraY: Float = 0, cameraZ: Float = 0, zoom: Float = 1) {
            self.characterX = characterX
            self.characterY = characterY
            self.characterZ = characterZ
            self.cameraX = cameraX
            self.cameraY = cameraY
            self.cameraZ = cameraZ
            self.zoom = zoom
        }

        /// Tells pictures of the same Mii in different poses apart.
        var key: String { "\(characterX),\(characterY),\(characterZ),\(cameraX),\(cameraY),\(cameraZ),\(zoom)" }

        /// Straight ahead, as My Miis and the editor show a Mii.
        static let front = Pose()

        /// CurrentUserSideProfile and FriendsSideProfile: the profile page's and sidebar's three-quarter view.
        static let side = Pose(characterX: 350, characterY: 15, characterZ: 355, cameraX: 12)
    }

    /// The Mii at `size` x `size`, as non-premultiplied ARGB colours (alpha in the top byte),
    /// transparent around it: the head, with the upper body below it when `bodies` are given, or
    /// with `fullBody` the whole Mii (the PC's all_body) when they are.
    static func render(_ resource: FflResource, _ mii: Mii, size: Int, pose: Pose = .front, bodies: MiiBodies? = nil,
                       fullBody: Bool = false, expression: Int = 0) throws -> [UInt32] {
        try render(resource, FflCharInfo.of(mii), size: size, expression: expression, pose: pose, bodies: bodies,
                   fullBody: fullBody)
    }

    static func render(_ resource: FflResource, _ info: FflCharInfo, size: Int, expression: Int = 0, pose: Pose = .front,
                       bodies: MiiBodies? = nil, fullBody: Bool = false) throws -> [UInt32] {
        // The Kotlin's require(), which a caller can only fail by a mistake of its own.
        guard (16...4096).contains(size), size % 2 == 0 else { throw MiiError("Unsupported picture size \(size)") }
        let resolution = size <= 384 ? 256 : 512
        let draws = try buildDraws(resource, info, resolution, expression)
        if draws.isEmpty { throw MiiError("The renderer produced no drawable meshes for this Mii.") }
        let body = bodies.flatMap { self.body($0, info) }

        let target = Target(width: size, height: size, withDepth: true)
        target.fill(255, 255, 255, 0)
        // The PC's face view: 15 degrees of field of view on the head, the camera orbiting it
        // (CalculateCameraOrbitPosition) and the head turned about its own origin. With a body
        // the head sits on its shoulders, and the camera rises with it; the body turns about its feet.
        // Its all_body view stands further back, looking at the whole Mii from a fixed place.
        let wholeBody = fullBody && body != nil
        let y: Float = wholeBody ? 90 : 4.805 / 0.14
        let distance: Float = wholeBody ? 760 : 57.553 / 0.14
        let z = distance * pose.zoom
        let camera = radians(pose.cameraX, pose.cameraY, pose.cameraZ)
        var position: [Float] = [
            z * -sinF(camera[1]) * cosF(camera[0]),
            z * sinF(camera[0]),
            z * cosF(camera[1]) * cosF(camera[0]),
        ]
        position[1] += y
        var lookAt: [Float] = [0, wholeBody ? 95 : y, 0]
        if let body, !wholeBody {
            for c in 0..<3 {
                position[c] += body.headTranslation[c]
                lookAt[c] += body.headTranslation[c]
            }
        }
        let up: [Float] = [sinF(camera[2]), cosF(camera[2]), 0]
        let view = Mat4.lookAt(position, lookAt, up)
        let projection = Mat4.perspective(15 * (piF / 180), 1, 10, 1200)
        let rotation = Mat4.rotation(radians(pose.characterX, pose.characterY, pose.characterZ))
        var meshes: [Prepared] = []
        // The body first, so the head is drawn over the collar.
        if let body {
            let bodyModel = rotation * body.scale
            meshes += body.draws.compactMap { prepare($0, size, size, bodyModel, view, projection) }
        }
        let headModel = body.map { rotation * Mat4.translation($0.headTranslation) } ?? rotation
        meshes += draws.compactMap { prepare($0, size, size, headModel, view, projection) }
        drawAll(target, meshes, light: true, .over)
        return target.argb()
    }

    /// `info`'s body (TryCreateBodyRenderData), or nil when `bodies` has none for its gender.
    private static func body(_ bodies: MiiBodies, _ info: FflCharInfo) -> Body? {
        // Math.floorMod: a negative gender counts from the top.
        let meshes = ((info.gender % 2) + 2) % 2 == 1 ? bodies.female : bodies.male
        if meshes.isEmpty { return nil }
        // CalculateBodyScale: build widens the body, height stretches it.
        let build = coerce(Float(info.build), 0, 127)
        let height = coerce(Float(info.height), 0, 127)
        let widened = build * (height * 0.003671875 + 0.4)
        let scaleX = widened / 128.0 + height * 0.001796875 + 0.4
        let scaleY = height * 0.006015625 + 0.5
        let headTranslation: [Float] = [0, MiiBodies.headY * scaleY * MiiBodies.modelScale, 0]
        let shirt = MiiColors.favorite(info.favoriteColor)
        let draws = meshes.map { mesh -> Draw in
            let count = mesh.positions.count / 3
            let type = mesh.pants ? typePants : typeBody
            return Draw(
                positions: mesh.positions,
                texcoords: mesh.texcoords,
                normals: mesh.normals,
                tangents: [Float](repeating: 0, count: count * 3),
                // The PC's vertex parameters for the body: full specular and rim.
                parameters: (0..<count * 4).map { $0 % 4 == 2 ? 0 : 1 },
                indices: mesh.indices,
                cull: cullBack,
                modulate: Modulate(0, type, r: mesh.pants ? pants : shirt),
                material: materials[type])
        }
        return Body(draws: draws, scale: Mat4.scale([scaleX, scaleY, scaleX]), headTranslation: headTranslation)
    }

    /// ConvertDegreesToRadians: each angle brought into -180..180 by an IEEE remainder first.
    private static func radians(_ x: Float, _ y: Float, _ z: Float) -> [Float] {
        [x, y, z].map { Float(Double($0).remainder(dividingBy: 360)) * (piF / 180) }
    }

    /// Draws `meshes` in order. A large picture is split into bands of rows drawn in parallel,
    /// each band drawing every mesh in order, so the picture is the same as one pass.
    private static func drawAll(_ target: Target, _ meshes: [Prepared], light: Bool, _ blend: Blend) {
        let height = target.height
        let bands = height >= parallelSize ? Self.bands : 1
        if bands == 1 {
            for mesh in meshes { rasterize(target, mesh, light, blend, 0, height) }
            return
        }
        let rows = (height + bands - 1) / bands
        DispatchQueue.concurrentPerform(iterations: bands) { band in
            for mesh in meshes { rasterize(target, mesh, light, blend, band * rows, min(height, (band + 1) * rows)) }
        }
    }

    private static let parallelSize = 256
    private static let bands = min(max(ProcessInfo.processInfo.activeProcessorCount, 1), 4)

    /// The face parts the editor pictures from their textures rather than as a head.
    enum Part: Hashable, Sendable, CaseIterable {
        case eyebrow, eye, nose, mouth, glasses, mustache
    }

    /// One choice of a face part, drawn alone from its texture in the Mii's colours as the face
    /// would show it, `size` x `size` and transparent around it: the pictures on the editor's
    /// choice buttons, which the PC draws from icons of its own. Nil for a choice that is no part
    /// at all, such as no glasses.
    static func partIcon(_ resource: FflResource, _ mii: Mii, part: Part, index: Int, size: Int) throws -> [UInt32]? {
        var chosen = mii
        switch part {
        case .eyebrow: chosen.eyebrowType = index
        case .eye: chosen.eyeType = index
        case .nose: chosen.noseType = index
        case .mouth: chosen.lipType = index
        case .glasses: chosen.glassesType = index
        case .mustache: chosen.mustacheType = index
        }
        let info = FflCharInfo.of(chosen)
        // The texture, how it is coloured, whether its other half is its mirror image, and the
        // part of it worth showing (noses sit small in the middle of theirs).
        let texture: FflTexture
        let modulate: Modulate
        var mirrored = false
        var window: Float = 0
        switch part {
        case .eyebrow:
            guard let found = try resource.texture(FflResource.textureEyebrow, info.eyebrowType) else { return nil }
            texture = found
            modulate = Modulate(3, typeMask, r: MiiColors.hair(info.eyebrowColor), texture: found)
        case .eye:
            guard let found = try resource.texture(FflResource.textureEye, info.eyeType) else { return nil }
            texture = found
            modulate = eyeModulate(info, info.eyeType, found)
        case .nose:
            guard let found = try resource.texture(FflResource.textureNoseline, info.noseType) else { return nil }
            texture = found
            modulate = Modulate(3, typeNoseline, r: black, texture: found)
            window = 0.25
        case .mouth:
            guard let found = try resource.texture(FflResource.textureMouth, info.mouthType) else { return nil }
            texture = found
            modulate = info.mouthType > 36
                ? Modulate(1, typeMask, texture: found)
                : Modulate(2, typeMask, r: MiiColors.mouthR(info.mouthColor), g: MiiColors.mouthG(info.mouthColor), b: white,
                           texture: found)
        case .glasses:
            guard let found = try resource.texture(FflResource.textureGlass, info.glassType) else { return nil }
            texture = found
            modulate = Modulate(4, typeGlass, r: MiiColors.glass(info.glassColor), texture: found)
            mirrored = true
        case .mustache:
            guard let found = try resource.texture(FflResource.textureMustache, info.mustacheType) else { return nil }
            texture = found
            modulate = Modulate(3, typeMask, r: MiiColors.hair(info.beardColor), texture: found)
            mirrored = true
        }
        // "None" is an 8x8 blank.
        if texture.width <= 8 && texture.height <= 8 { return nil }

        let span = 1 - 2 * window
        let across: Float = mirrored ? 2 : 1
        let sourceWidth = Float(texture.width) * span * across
        let sourceHeight = Float(texture.height) * span
        let scale = Float(size) * 0.9 / maxF(sourceWidth, sourceHeight)
        let width = sourceWidth * scale
        let height = sourceHeight * scale
        let left = (Float(size) - width) / 2
        let top = (Float(size) - height) / 2
        var out = [UInt32](repeating: 0, count: size * size)
        let scratch = Scratch()
        defer { scratch.free() }
        let sample = scratch.color
        // A negative size gives the Kotlin's size * size transparent pixels, its loops never running.
        let rows = max(size, 0)
        withView(texture) { view in
            let shading = ModulateView(modulate, view)
            for y in 0..<rows {
                for x in 0..<rows {
                    // 2x2 samples per pixel, averaged with their alpha.
                    var r: Float = 0
                    var g: Float = 0
                    var b: Float = 0
                    var a: Float = 0
                    for sy in 0..<2 {
                        for sx in 0..<2 {
                            let fx = (Float(x) + 0.25 + Float(sx) * 0.5 - left) / width
                            let fy = (Float(y) + 0.25 + Float(sy) * 0.5 - top) / height
                            if fx < 0 || fx >= 1 || fy < 0 || fy >= 1 { continue }
                            let u = window + fx * span * across
                            let v = window + fy * span
                            if self.modulate(shading, u, v, sample, scratch) { clampColor(sample) }
                            r += sample[0] * sample[3]
                            g += sample[1] * sample[3]
                            b += sample[2] * sample[3]
                            a += sample[3]
                        }
                    }
                    if a <= 0 { continue }
                    let alpha = a / 4
                    out[y * size + x] = UInt32(toByte(alpha)) << 24 | UInt32(toByte(r / a)) << 16
                        | UInt32(toByte(g / a)) << 8 | UInt32(toByte(b / a))
                }
            }
        }
        return out
    }

    // MARK: Building the head (BuildManagedDrawParams)

    private static func buildDraws(_ resource: FflResource, _ info: FflCharInfo, _ resolution: Int,
                                   _ expression: Int) throws -> [Draw] {
        var draws: [Draw] = []
        draws.reserveCapacity(12)
        guard let faceline = resource.shape(FflResource.shapeFaceline, info.faceType) else {
            throw MiiError("Faceline shape \(info.faceType) is missing.")
        }
        let zero: [Float] = [0, 0, 0]
        let hairPos = faceline.translates?[0] ?? zero
        let faceCenter = faceline.translates?[1] ?? zero
        let beardPos = faceline.translates?[2] ?? zero

        let facelineTexture = try self.facelineTexture(resource, info, resolution)
        let maskTexture = try self.maskTexture(resource, info, resolution, expression)
        let skin = MiiColors.faceline(info.facelineColor)

        draws.append(try draw(faceline, 1, 1, nil, false, cullBack,
                              facelineTexture.map { Modulate(1, typeFaceline, texture: $0) } ?? Modulate(0, typeFaceline, r: skin)))

        let hairColor = MiiColors.hair(info.hairColor)
        let hairFlip = info.hairDir > 0
        let hairCull = hairFlip ? cullFront : cullBack
        if let shape = resource.shape(FflResource.shapeHair, info.hairType) {
            draws.append(try draw(shape, 1, 1, hairPos, hairFlip, hairCull, Modulate(0, typeHair, r: hairColor)))
        }
        if let shape = resource.shape(FflResource.shapeForehead, info.hairType) {
            draws.append(try draw(shape, 1, 1, hairPos, hairFlip, hairCull, Modulate(0, typeForehead, r: skin)))
        }
        if let cap = try resource.texture(FflResource.textureCap, info.hairType),
           let shape = resource.shape(FflResource.shapeHat, info.hairType) {
            draws.append(try draw(shape, 1, 1, hairPos, hairFlip, hairCull,
                                  Modulate(5, typeCap, r: MiiColors.favorite(info.favoriteColor), texture: cap)))
        }
        if (0..<4).contains(info.beardType), let shape = resource.shape(FflResource.shapeBeard, info.beardType) {
            draws.append(try draw(shape, 1, 1, beardPos, false, cullBack, Modulate(0, typeBeard, r: MiiColors.hair(info.beardColor))))
        }

        if !noNoseExpressions.contains(expression) {
            let noseScale = Float(info.noseScale) * 0.175 + 0.4
            let nosePos: [Float] = [faceCenter[0], faceCenter[1] + Float(info.nosePositionY - 8) * -1.5, faceCenter[2]]
            if let shape = resource.shape(FflResource.shapeNose, info.noseType) {
                draws.append(try draw(shape, noseScale, noseScale, nosePos, false, cullBack, Modulate(0, typeNose, r: skin)))
            }
            if let line = try resource.texture(FflResource.textureNoseline, info.noseType),
               let shape = resource.shape(FflResource.shapeNoseline, info.noseType) {
                draws.append(try draw(shape, noseScale, noseScale, nosePos, false, cullBack,
                                      Modulate(3, typeNoseline, r: black, texture: line)))
            }
            if let maskTexture, let shape = resource.shape(FflResource.shapeMask, info.faceType) {
                let cull = resource.linearTextures ? cullNone : cullBack
                draws.append(try draw(shape, 1, 1, nil, false, cull, Modulate(1, typeMask, texture: maskTexture)))
            }
        }

        if info.glassType > 0, let glass = try resource.texture(FflResource.textureGlass, info.glassType) {
            let scale = Float(info.glassScale) * (resource.linearTextures ? 0.175 : 0.15) + 0.4
            let position: [Float] = [
                faceCenter[0],
                faceCenter[1] + Float(info.glassPositionY - 11) * -1.5 + 5.0,
                faceCenter[2] + 2.0,
            ]
            if let shape = resource.shape(FflResource.shapeGlass, 0) {
                draws.append(try draw(shape, scale, scale, position, false, cullNone,
                                      Modulate(4, typeGlass, r: MiiColors.glass(info.glassColor), texture: glass)))
            }
        }
        return draws
    }

    /// Wrinkles, make-up and textured beards on the skin colour (BuildManagedFacelineTexture).
    private static func facelineTexture(_ resource: FflResource, _ info: FflCharInfo, _ resolution: Int) throws -> FflTexture? {
        if info.faceLine == 0 && info.faceMakeup == 0 && info.beardType < 4 { return nil }
        var overlays: [Draw] = []
        if info.faceMakeup > 0, let texture = try resource.texture(FflResource.textureMakeup, info.faceMakeup) {
            overlays.append(try fullScreen(Modulate(1, typeFaceline, texture: texture)))
        }
        if info.faceLine > 0, let texture = try resource.texture(FflResource.textureFaceline, info.faceLine) {
            overlays.append(try fullScreen(Modulate(3, typeFaceline, r: black, texture: texture)))
        }
        if info.beardType >= 4, let texture = try resource.texture(FflResource.textureBeard, info.beardType - 3) {
            overlays.append(try fullScreen(Modulate(3, typeFaceline, r: MiiColors.hair(info.beardColor), texture: texture)))
        }
        if overlays.isEmpty { return nil }
        return overlayTexture(overlays, max(1, resolution / 2), max(1, resolution), MiiColors.faceline(info.facelineColor), .faceline)
    }

    /// Eyes, eyebrows, mouth, mustache and mole, placed on FFL's 64-unit face grid (BuildManagedMaskTexture).
    private static func maskTexture(_ resource: FflResource, _ info: FflCharInfo, _ resolution: Int,
                                    _ expression: Int) throws -> FflTexture? {
        let element = expressionElements[coerce(expression, 0, 18)]
        let eyeIndexR = eyeTexture(info, element[0])
        let eyeIndexL = eyeTexture(info, element[1])
        let mouthIndex = mouthTexture(info, element[2])
        let eyebrowIndex = element[3] == 0 ? info.eyebrowType : element[3]
        let parts = MaskParts(info)
        var overlays: [Draw] = []
        overlays.reserveCapacity(8)

        if info.mustacheType != 0, let texture = try resource.texture(FflResource.textureMustache, info.mustacheType) {
            let modulate = Modulate(3, typeMask, r: MiiColors.hair(info.beardColor), texture: texture)
            overlays.append(try maskQuad(parts.mustacheR, modulate))
            overlays.append(try maskQuad(parts.mustacheL, modulate))
        }
        if let texture = try resource.texture(FflResource.textureMouth, mouthIndex) {
            overlays.append(try maskQuad(
                parts.mouth,
                mouthIndex > 36
                    ? Modulate(1, typeMask, texture: texture)
                    : Modulate(2, typeMask, r: MiiColors.mouthR(info.mouthColor), g: MiiColors.mouthG(info.mouthColor), b: white,
                               texture: texture)))
        }
        if eyebrowIndex != 23, let texture = try resource.texture(FflResource.textureEyebrow, eyebrowIndex) {
            let modulate = Modulate(3, typeMask, r: MiiColors.hair(info.eyebrowColor), texture: texture)
            overlays.append(try maskQuad(parts.eyebrowR, modulate))
            overlays.append(try maskQuad(parts.eyebrowL, modulate))
        }
        let eyeR = try resource.texture(FflResource.textureEye, eyeIndexR)
        let eyeL = try resource.texture(FflResource.textureEye, eyeIndexL)
        if let eyeR { overlays.append(try maskQuad(parts.eyeR, eyeModulate(info, eyeIndexR, eyeR))) }
        if let eyeL { overlays.append(try maskQuad(parts.eyeL, eyeModulate(info, eyeIndexL, eyeL))) }
        if info.moleType != 0, let texture = try resource.texture(FflResource.textureMole, info.moleType) {
            overlays.append(try maskQuad(parts.mole, Modulate(3, typeMask, r: mole, texture: texture)))
        }
        if overlays.isEmpty { return nil }
        let size = max(1, resolution)
        return overlayTexture(overlays, size, size, SIMD4<Float>(0, 0, 0, 0), .mask)
    }

    private static func eyeModulate(_ info: FflCharInfo, _ index: Int, _ texture: FflTexture) -> Modulate {
        directEyes.contains(index)
            ? Modulate(1, typeMask, texture: texture)
            : Modulate(2, typeMask, r: SIMD4<Float>(0, 1, 1, 1), g: white, b: MiiColors.eyeB(info.eyeColor), texture: texture)
    }

    private static func eyeTexture(_ info: FflCharInfo, _ type: Int) -> Int {
        switch type {
        case 1: return 60
        case 3: return 61
        case 4: return 26
        case 5: return 47
        default: return info.eyeType
        }
    }

    private static func mouthTexture(_ info: FflCharInfo, _ type: Int) -> Int {
        switch type {
        case 1: return 10
        case 2: return 12
        case 3: return 36
        case 5: return 19
        default: return info.mouthType
        }
    }

    private static func overlayTexture(_ overlays: [Draw], _ width: Int, _ height: Int, _ clear: SIMD4<Float>,
                                       _ blend: Blend) -> FflTexture {
        let target = Target(width: width, height: height, withDepth: false)
        target.fill(toByte(clear[0]), toByte(clear[1]), toByte(clear[2]), toByte(clear[3]))
        drawAll(target, overlays.compactMap { prepare($0, width, height, .identity, .identity, .identity) }, light: false, blend)
        return FflTexture(width: width, height: height, format: FflTexture.rgba8, pixels: target.bytes())
    }

    private static func fullScreen(_ modulate: Modulate) throws -> Draw {
        let shape = FflShape(
            positions: [-1, 1, 0, 1, 1, 0, -1, -1, 0, 1, -1, 0],
            texcoords: [0, 0, 1, 0, 0, 1, 1, 1],
            normals: flatNormals,
            tangents: [Float](repeating: 0, count: 12),
            parameters: flatParameters,
            indices: [0, 1, 2, 2, 1, 3],
            translates: nil)
        return try draw(shape, 1, 1, nil, false, cullNone, modulate)
    }

    /// A part's quad on the mask (CreateRawMaskOverlayDrawParam), in the mask's clip space.
    private static func maskQuad(_ part: MaskPart, _ modulate: Modulate) throws -> Draw {
        let posXAdd: Float
        switch part.origin {
        case .center: posXAdd = -0.5
        case .left: posXAdd = -1
        case .right: posXAdd = 0
        }
        let tex01: Float = part.origin == .right ? 0 : 1
        let tex23: Float = part.origin == .right ? 1 : 0
        let baseX: [Float] = [1, 1, 0, 0]
        let baseY: [Float] = [-0.5, 0.5, 0.5, -0.5]
        let uvY: [Float] = [0, 1, 1, 0]
        let radians = part.rotation * (piF / 180)
        let cos = cosF(radians)
        let sin = sinF(radians)
        let grid = Float(2) / 64
        var positions = [Float](repeating: 0, count: 12)
        var texcoords = [Float](repeating: 0, count: 8)
        for i in 0..<4 {
            let lx = baseX[i] + posXAdd
            let ly = baseY[i]
            let xr = lx * part.scaleX * cos - ly * part.scaleY * sin
            let yr = lx * part.scaleX * sin + ly * part.scaleY * cos
            let xw = 0.88961464 * xr + part.x
            let yw = 0.9276675 * yr + part.y
            positions[i * 3] = xw * grid - 1
            positions[i * 3 + 1] = 1 - yw * grid
            texcoords[i * 2] = i < 2 ? tex01 : tex23
            texcoords[i * 2 + 1] = uvY[i]
        }
        let shape = FflShape(
            positions: positions,
            texcoords: texcoords,
            normals: flatNormals,
            tangents: [Float](repeating: 0, count: 12),
            parameters: flatParameters,
            indices: [2, 1, 3, 1, 3, 0],
            translates: nil)
        return try draw(shape, 1, 1, nil, false, cullNone, modulate)
    }

    /// A shape placed on the head (BuildManagedShapeDrawParam). The PC passes normals, tangents and
    /// vertex parameters through FFL's packed vertex formats, so they are quantised the same way.
    private static func draw(_ shape: FflShape, _ scaleX: Float, _ scaleY: Float, _ translate: [Float]?, _ flipX: Bool,
                             _ cull: Int, _ modulate: Modulate) throws -> Draw {
        let count = shape.vertexCount
        if count == 0 || shape.indices.count < 3 { throw MiiError("Shape has no drawable geometry.") }
        let scaleZ = (scaleX + scaleY) * 0.5
        let tx = translate?[0] ?? 0
        let ty = translate?[1] ?? 0
        let tz = translate?[2] ?? 0
        let source = shape.positions
        let sourceNormals = shape.normals
        let sourceTangents = shape.tangents
        let sourceParameters = shape.parameters
        var positions = [Float](repeating: 0, count: count * 3)
        var normals = [Float](repeating: 0, count: count * 3)
        var tangents = [Float](repeating: 0, count: count * 3)
        var parameters = [Float](repeating: 0, count: count * 4)
        for i in 0..<count {
            let x = flipX ? -source[i * 3] : source[i * 3]
            positions[i * 3] = x * scaleX + tx
            positions[i * 3 + 1] = source[i * 3 + 1] * scaleY + ty
            positions[i * 3 + 2] = source[i * 3 + 2] * scaleZ + tz

            let nx = flipX ? -sourceNormals[i * 3] : sourceNormals[i * 3]
            let packed = pack10(nx) | (pack10(sourceNormals[i * 3 + 1]) << 10) | (pack10(sourceNormals[i * 3 + 2]) << 20)
            let normal = FflResource.decodeInt2101010(packed)
            normals[i * 3] = normal.0
            normals[i * 3 + 1] = normal.1
            normals[i * 3 + 2] = normal.2

            let tangentX = flipX ? -sourceTangents[i * 3] : sourceTangents[i * 3]
            let tangent = FflResource.normalized(
                Float(pack8(tangentX)) / 127, Float(pack8(sourceTangents[i * 3 + 1])) / 127,
                Float(pack8(sourceTangents[i * 3 + 2])) / 127, threshold: 1e-8, fallbackZ: 0)
            tangents[i * 3] = tangent.0
            tangents[i * 3 + 1] = tangent.1
            tangents[i * 3 + 2] = tangent.2
            for c in 0..<4 { parameters[i * 4 + c] = Float(toByte(sourceParameters[i * 4 + c])) / 255 }
        }
        return Draw(positions: positions, texcoords: shape.texcoords, normals: normals, tangents: tangents,
                    parameters: parameters, indices: shape.indices, cull: cull, modulate: modulate,
                    material: materials[modulate.type])
    }

    private static func pack10(_ value: Float) -> Int {
        coerce(toInt(roundF(coerce(value, -1, 1) * 511)), -512, 511) & 0x3FF
    }

    private static func pack8(_ value: Float) -> Int {
        coerce(toInt(roundF(coerce(value, -1, 1) * 127)), -127, 127)
    }

    // MARK: Rasterising (PrepareMesh, RasterizeTriangle, EvaluateModulateColor, BlendPixel)

    private static func prepare(_ draw: Draw, _ width: Int, _ height: Int, _ model: Mat4, _ view: Mat4,
                                _ projection: Mat4) -> Prepared? {
        let count = draw.positions.count / 3
        let modelView = model * view
        let positions = draw.positions
        let texcoords = draw.texcoords
        let normals = draw.normals
        let tangents = draw.tangents
        let parameters = draw.parameters
        var vertices = [Float](repeating: 0, count: count * vertexStride)
        let complete = vertices.withUnsafeMutableBufferPointer { out -> Bool in
            for i in 0..<count {
                let world = model.transformPoint(positions[i * 3], positions[i * 3 + 1], positions[i * 3 + 2])
                let eye = view.transformPoint(world.0, world.1, world.2)
                let clip = projection.transform4(eye.0, eye.1, eye.2)
                if abs(clip.3) <= 1e-6 { return false }
                let invW = 1 / clip.3
                let ndcX = clip.0 * invW
                let ndcY = clip.1 * invW
                let ndcZ = clip.2 * invW
                let o = i * vertexStride
                out[o] = (ndcX * 0.5 + 0.5) * Float(width)
                out[o + 1] = (1 - (ndcY * 0.5 + 0.5)) * Float(height)
                out[o + 2] = ndcZ * 0.5 + 0.5
                out[o + 3] = invW
                out[o + 4] = i * 2 < texcoords.count ? texcoords[i * 2] : 0
                out[o + 5] = i * 2 + 1 < texcoords.count ? texcoords[i * 2 + 1] : 0
                out[o + 6] = eye.0
                out[o + 7] = eye.1
                out[o + 8] = eye.2
                let normal = modelView.transformNormal(normals[i * 3], normals[i * 3 + 1], normals[i * 3 + 2])
                out[o + 9] = normal.0
                out[o + 10] = normal.1
                out[o + 11] = normal.2
                let tangent = modelView.transformNormal(tangents[i * 3], tangents[i * 3 + 1], tangents[i * 3 + 2])
                out[o + 12] = tangent.0
                out[o + 13] = tangent.1
                out[o + 14] = tangent.2
                for c in 0..<4 { out[o + 15 + c] = parameters[i * 4 + c] }
            }
            return true
        }
        return complete ? Prepared(draw: draw, vertices: vertices) : nil
    }

    /// Draws `mesh`'s triangles into rows `rowStart` to `rowEnd` of `target`.
    private static func rasterize(_ target: Target, _ mesh: Prepared, _ light: Bool, _ blend: Blend, _ rowStart: Int, _ rowEnd: Int) {
        let scratch = Scratch()
        defer { scratch.free() }
        let draw = mesh.draw
        let raster = RasterTarget(width: target.width, height: target.height, pixels: target.pixels, depth: target.depth)
        let material = draw.material
        let cull = draw.cull
        mesh.vertices.withUnsafeBufferPointer { vertices in
            draw.indices.withUnsafeBufferPointer { indices in
                withView(draw.modulate.texture) { view in
                    let modulate = ModulateView(draw.modulate, view)
                    var i = 0
                    while i + 2 < indices.count {
                        triangle(raster, vertices, cull, modulate, material, light, blend, indices[i], indices[i + 1], indices[i + 2],
                                 scratch, rowStart, rowEnd)
                        i += 3
                    }
                }
            }
        }
    }

    private static func triangle(_ target: RasterTarget, _ v: UnsafeBufferPointer<Float>, _ cull: Int, _ modulate: ModulateView,
                                 _ material: Material, _ light: Bool, _ blend: Blend, _ ia: Int, _ ib: Int, _ ic: Int,
                                 _ scratch: Scratch, _ rowStart: Int, _ rowEnd: Int) {
        let count = v.count / vertexStride
        if ia < 0 || ia >= count || ib < 0 || ib >= count || ic < 0 || ic >= count { return }
        let a = ia * vertexStride
        let b = ib * vertexStride
        let c = ic * vertexStride
        let ax = v[a]
        let ay = v[a + 1]
        let bx = v[b]
        let by = v[b + 1]
        let cx = v[c]
        let cy = v[c + 1]
        let area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if abs(area) < 1e-6 { return }
        if cull == cullBack && area >= 0 { return }
        if cull == cullFront && area <= 0 { return }

        let width = target.width
        let height = target.height
        let minX = coerce(toInt(floorF(minF(ax, minF(bx, cx)))), 0, width - 1)
        let maxX = coerce(toInt(ceilF(maxF(ax, maxF(bx, cx)))), 0, width - 1)
        let minY = coerce(toInt(floorF(minF(ay, minF(by, cy)))), 0, height - 1)
        let maxY = coerce(toInt(ceilF(maxF(ay, maxF(by, cy)))), 0, height - 1)
        let firstRow = max(minY, rowStart)
        let lastRow = min(maxY, rowEnd - 1)
        if firstRow > lastRow { return }

        let invArea = 1 / area
        let sampleX = Float(minX) + 0.5
        let sampleY = Float(minY) + 0.5
        let e0x = by - cy
        let e0y = cx - bx
        let e0c = bx * cy - by * cx
        let e1x = cy - ay
        let e1y = ax - cx
        let e1c = cx * ay - cy * ax
        let e2x = ay - by
        let e2y = bx - ax
        let e2c = ax * by - ay * bx
        var e0Row = e0x * sampleX + e0y * sampleY + e0c
        var e1Row = e1x * sampleX + e1y * sampleY + e1c
        var e2Row = e2x * sampleX + e2y * sampleY + e2c
        // Rows before the band are stepped as the PC steps them, so every row's edges round the same.
        var skipped = minY
        while skipped < firstRow {
            e0Row += e0y
            e1Row += e1y
            e2Row += e2y
            skipped += 1
        }
        let aw = v[a + 3]
        let bw = v[b + 3]
        let cw = v[c + 3]
        let depth = target.depth
        let pixels = target.pixels
        let at = scratch.at
        let color = scratch.color

        var y = firstRow
        while y <= lastRow {
            var e0 = e0Row
            var e1 = e1Row
            var e2 = e2Row
            var pixel = y * width + minX
            var x = minX
            while x <= maxX {
                let negative = e0 < 0 || e1 < 0 || e2 < 0
                let positive = e0 > 0 || e1 > 0 || e2 > 0
                if !(negative && positive) {
                    let w0 = e0 * invArea
                    let w1 = e1 * invArea
                    let w2 = e2 * invArea
                    let denominator = w0 * aw + w1 * bw + w2 * cw
                    if abs(denominator) >= 1e-8 {
                        let z = (w0 * v[a + 2] * aw + w1 * v[b + 2] * bw + w2 * v[c + 2] * cw) / denominator
                        if z >= 0 && z <= 1 && (depth == nil || z <= depth![pixel]) {
                            at[4] = (w0 * v[a + 4] * aw + w1 * v[b + 4] * bw + w2 * v[c + 4] * cw) / denominator
                            at[5] = (w0 * v[a + 5] * aw + w1 * v[b + 5] * bw + w2 * v[c + 5] * cw) / denominator
                            // The rest only matters for a texel that shows.
                            if self.modulate(modulate, at[4], at[5], color, scratch) {
                                if light {
                                    var k = 6
                                    while k < vertexStride {
                                        at[k] = (w0 * v[a + k] * aw + w1 * v[b + k] * bw + w2 * v[c + k] * cw) / denominator
                                        k += 1
                                    }
                                    shade(material, at, color)
                                } else {
                                    clampColor(color)
                                }
                            }
                            if color[3] > 0 {
                                blendPixel(pixels, pixel * 4, color, blend)
                                if let depth { depth[pixel] = z }
                            }
                        }
                    }
                }
                e0 += e0x
                e1 += e1x
                e2 += e2x
                pixel += 1
                x += 1
            }
            e0Row += e0y
            e1Row += e1y
            e2Row += e2y
            y += 1
        }
    }

    /// The texel's colour before lighting (EvaluateModulateColor's modulate modes), not yet clamped.
    /// False when it is fully transparent, which draws nothing.
    @inline(__always)
    private static func modulate(_ modulate: ModulateView, _ u: Float, _ v: Float, _ out: UnsafeMutablePointer<Float>,
                                 _ scratch: Scratch) -> Bool {
        let texel = scratch.texel
        sample(modulate.texture, u, v, texel, scratch.corners)
        let r = modulate.r
        let g = modulate.g
        let b = modulate.b
        switch modulate.mode {
        case 0:
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 1
        case 1:
            out[0] = texel[0]; out[1] = texel[1]; out[2] = texel[2]; out[3] = texel[3]
        case 2:
            out[0] = texel[0] * r[0] + texel[1] * g[0] + texel[2] * b[0]
            out[1] = texel[0] * r[1] + texel[1] * g[1] + texel[2] * b[1]
            out[2] = texel[0] * r[2] + texel[1] * g[2] + texel[2] * b[2]
            out[3] = texel[3]
        case 3:
            out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = texel[0]
        case 4:
            out[0] = texel[1] * r[0]; out[1] = texel[1] * r[1]; out[2] = texel[1] * r[2]; out[3] = texel[0]
        case 5:
            out[0] = texel[0] * r[0]; out[1] = texel[0] * r[1]; out[2] = texel[0] * r[2]; out[3] = 1
        default:
            out[0] = 1; out[1] = 1; out[2] = 1; out[3] = 1
        }
        if modulate.mode != 0 && out[3] <= 0 {
            out[0] = 0; out[1] = 0; out[2] = 0; out[3] = 0
            return false
        }
        return true
    }

    /// Lights the modulated colour in `color` (EvaluateModulateColor's lit path). Specular maths is
    /// skipped for materials without specular, where the PC multiplies it by zero.
    @inline(__always)
    private static func shade(_ material: Material, _ at: UnsafeMutablePointer<Float>, _ color: UnsafeMutablePointer<Float>) {
        let baseR = color[0]
        let baseG = color[1]
        let baseB = color[2]
        let n = FflResource.normalized(at[9], at[10], at[11], threshold: 1e-8, fallbackZ: 1)
        let diffuseDot = maxF(lightX * n.0 + lightY * n.1 + lightZ * n.2, profileDiffuseFloor)
        let diffuseFactor = 1 + (diffuseDot - 1) * profileDirectional
        var reflection: Float = 0
        var strength: Float = 0
        if material.hasSpecular {
            let e = FflResource.normalized(-at[6], -at[7], -at[8], threshold: 1e-8, fallbackZ: 1)
            // Reflect(-light, n) = -light - 2 * dot(-light, n) * n.
            let dotReflect = -lightX * n.0 + -lightY * n.1 + -lightZ * n.2
            let reflectX = -lightX - 2 * dotReflect * n.0
            let reflectY = -lightY - 2 * dotReflect * n.1
            let reflectZ = -lightZ - 2 * dotReflect * n.2
            let blinn = power(maxF(reflectX * e.0 + reflectY * e.1 + reflectZ * e.2, 0), material.specularPower)
            if material.anisotropic {
                let tangentLength = at[12] * at[12] + at[13] * at[13] + at[14] * at[14]
                let t: (Float, Float, Float) = tangentLength < 1e-8
                    ? (1, 0, 0)
                    : FflResource.normalized(at[12], at[13], at[14], threshold: 1e-8, fallbackZ: 0)
                let dotLt = lightX * t.0 + lightY * t.1 + lightZ * t.2
                let dotVt = e.0 * t.0 + e.1 * t.1 + e.2 * t.2
                let dotLn = maxF(0, 1 - dotLt * dotLt).squareRoot()
                let dotVr = dotLn * maxF(0, 1 - dotVt * dotVt).squareRoot() - dotLt * dotVt
                let anisotropic = power(maxF(0, dotVr), material.specularPower)
                reflection = anisotropic + (blinn - anisotropic) * at[15]
                strength = at[16]
            } else {
                reflection = blinn
                strength = 1
            }
        }
        let rimFactor = power(maxF(0, at[18] * (1 - abs(n.2))), profileRimPower)
        let rim = rimFactor * profileRimScale
        let diffuseScale = diffuseFactor * profileDiffuseScale
        for channel in 0..<3 {
            let ambient = material.ambient[channel] * profileAmbient
            let diffuse = material.diffuse[channel] * diffuseScale
            let specular = material.specular[channel] * reflection * strength * profileSpecular
            let base = channel == 0 ? baseR : channel == 1 ? baseG : baseB
            color[channel] = clamp01((ambient + diffuse) * base + specular + material.rim[channel] * rim)
        }
        color[3] = clamp01(color[3])
    }

    /// The unlit colour, clamped as the PC returns it.
    @inline(__always)
    private static func clampColor(_ color: UnsafeMutablePointer<Float>) {
        for c in 0..<4 { color[c] = clamp01(color[c]) }
    }

    /// Float pow as the PC's MathF.Pow gives it; 0 to a positive power is 0 without the call.
    @inline(__always)
    private static func power(_ base: Float, _ exponent: Float) -> Float { base == 0 ? 0 : powF(base, exponent) }

    /// Bilinear with mirrored repeat, texel centres on the edges (SampleTexture).
    @inline(__always)
    private static func sample(_ texture: TextureView?, _ u: Float, _ v: Float, _ out: UnsafeMutablePointer<Float>,
                               _ corners: UnsafeMutablePointer<Float>) {
        guard let texture else {
            out[0] = 1; out[1] = 1; out[2] = 1; out[3] = 1
            return
        }
        if texture.width <= 1 || texture.height <= 1 {
            texel(texture, 0, 0, out)
            return
        }
        let fx = mirror(u) * Float(texture.width - 1)
        let fy = mirror(v) * Float(texture.height - 1)
        let x0 = coerce(toInt(floorF(fx)), 0, texture.width - 1)
        let y0 = coerce(toInt(floorF(fy)), 0, texture.height - 1)
        let x1 = min(x0 + 1, texture.width - 1)
        let y1 = min(y0 + 1, texture.height - 1)
        let tx = fx - Float(x0)
        let ty = fy - Float(y0)
        texel(texture, x0, y0, corners)
        texel(texture, x1, y0, corners + 4)
        texel(texture, x0, y1, corners + 8)
        texel(texture, x1, y1, corners + 12)
        for c in 0..<4 {
            let top = lerp(corners[c], corners[4 + c], tx)
            let bottom = lerp(corners[8 + c], corners[12 + c], tx)
            out[c] = lerp(top, bottom, ty)
        }
    }

    @inline(__always)
    private static func lerp(_ a: Float, _ b: Float, _ t: Float) -> Float { a * (1 - t) + b * t }

    @inline(__always)
    private static func mirror(_ value: Float) -> Float {
        // Kotlin's Float % is fmod: truncated, with the sign of the dividend.
        var wrapped = value.truncatingRemainder(dividingBy: 2)
        if wrapped < 0 { wrapped += 2 }
        return wrapped <= 1 ? wrapped : 2 - wrapped
    }

    @inline(__always)
    private static func texel(_ texture: TextureView, _ x: Int, _ y: Int, _ out: UnsafeMutablePointer<Float>) {
        let index = (y * texture.width + x) * texture.stride
        let p = texture.pixels
        if index < 0 || index + texture.stride > p.count {
            out[0] = 1; out[1] = 1; out[2] = 1; out[3] = 1
            return
        }
        switch texture.format {
        case FflTexture.r8:
            let value = Float(p[index]) / 255
            out[0] = value
            out[1] = value
            out[2] = value
            out[3] = 1
        case FflTexture.rg8:
            out[0] = Float(p[index]) / 255
            out[1] = Float(p[index + 1]) / 255
            out[2] = 0
            out[3] = 1
        default:
            for c in 0..<4 { out[c] = Float(p[index + c]) / 255 }
        }
    }

    @inline(__always)
    private static func clamp01(_ value: Float) -> Float { coerce(value, 0, 1) }

    @inline(__always)
    private static func toByte(_ value: Float) -> Int { coerce(toInt(roundF(clamp01(value) * 255)), 0, 255) }

    /// Target.blend in the Kotlin: `src` over the RGBA8 pixel at byte `i`.
    @inline(__always)
    private static func blendPixel(_ pixels: UnsafeMutablePointer<UInt8>, _ i: Int, _ src: UnsafeMutablePointer<Float>, _ mode: Blend) {
        let srcA = clamp01(src[3])
        if srcA <= 0 { return }
        if mode == .over && srcA >= 0.999 {
            pixels[i] = UInt8(truncatingIfNeeded: toByte(src[0]))
            pixels[i + 1] = UInt8(truncatingIfNeeded: toByte(src[1]))
            pixels[i + 2] = UInt8(truncatingIfNeeded: toByte(src[2]))
            pixels[i + 3] = 255
            return
        }
        let dstR = Float(pixels[i]) / 255
        let dstG = Float(pixels[i + 1]) / 255
        let dstB = Float(pixels[i + 2]) / 255
        let dstA = Float(pixels[i + 3]) / 255
        let outR: Float
        let outG: Float
        let outB: Float
        let outA: Float
        switch mode {
        case .faceline:
            outR = src[0] * srcA + dstR * (1 - srcA)
            outG = src[1] * srcA + dstG * (1 - srcA)
            outB = src[2] * srcA + dstB * (1 - srcA)
            outA = srcA + dstA
        case .mask:
            outR = src[0] * (1 - dstA) + dstR * dstA
            outG = src[1] * (1 - dstA) + dstG * dstA
            outB = src[2] * (1 - dstA) + dstB * dstA
            outA = srcA * srcA + dstA * dstA
        case .over:
            outA = srcA + dstA * (1 - srcA)
            if outA <= 0 { return }
            outR = (src[0] * srcA + dstR * dstA * (1 - srcA)) / outA
            outG = (src[1] * srcA + dstG * dstA * (1 - srcA)) / outA
            outB = (src[2] * srcA + dstB * dstA * (1 - srcA)) / outA
        }
        pixels[i] = UInt8(truncatingIfNeeded: toByte(outR))
        pixels[i + 1] = UInt8(truncatingIfNeeded: toByte(outG))
        pixels[i + 2] = UInt8(truncatingIfNeeded: toByte(outB))
        pixels[i + 3] = UInt8(truncatingIfNeeded: toByte(outA))
    }

    private static func withView<R>(_ texture: FflTexture?, _ body: (TextureView?) -> R) -> R {
        guard let texture else { return body(nil) }
        return texture.pixels.withUnsafeBufferPointer { pixels in
            body(TextureView(width: texture.width, height: texture.height, format: texture.format, stride: texture.stride,
                             pixels: pixels))
        }
    }

    // MARK: Types and tables

    private enum Blend { case over, faceline, mask }

    /// Per-call working memory, so shading a pixel allocates nothing: the interpolated vertex
    /// (`at`, the Kotlin's array of the same name), the colour, one texel and a bilinear
    /// sample's four corners.
    private struct Scratch {
        let base: UnsafeMutablePointer<Float>

        init() {
            base = .allocate(capacity: 43)
            base.initialize(repeating: 0, count: 43)
        }

        func free() { base.deallocate() }

        var at: UnsafeMutablePointer<Float> { base }
        var color: UnsafeMutablePointer<Float> { base + 19 }
        var texel: UnsafeMutablePointer<Float> { base + 23 }
        var corners: UnsafeMutablePointer<Float> { base + 27 }
    }

    /// An RGBA8 picture being drawn, with a depth buffer for the head. Its memory is raw, so the
    /// bands of rows drawn in parallel each write their own rows without Swift's exclusivity checks.
    private final class Target: @unchecked Sendable {
        let width: Int
        let height: Int
        let pixels: UnsafeMutablePointer<UInt8>
        let depth: UnsafeMutablePointer<Float>?

        init(width: Int, height: Int, withDepth: Bool) {
            self.width = width
            self.height = height
            pixels = .allocate(capacity: width * height * 4)
            pixels.initialize(repeating: 0, count: width * height * 4)
            if withDepth {
                let depth = UnsafeMutablePointer<Float>.allocate(capacity: width * height)
                depth.initialize(repeating: 1, count: width * height)
                self.depth = depth
            } else {
                depth = nil
            }
        }

        deinit {
            pixels.deallocate()
            depth?.deallocate()
        }

        func fill(_ r: Int, _ g: Int, _ b: Int, _ a: Int) {
            for i in 0..<width * height {
                pixels[i * 4] = UInt8(truncatingIfNeeded: r)
                pixels[i * 4 + 1] = UInt8(truncatingIfNeeded: g)
                pixels[i * 4 + 2] = UInt8(truncatingIfNeeded: b)
                pixels[i * 4 + 3] = UInt8(truncatingIfNeeded: a)
            }
        }

        func bytes() -> [UInt8] { Array(UnsafeBufferPointer(start: pixels, count: width * height * 4)) }

        func argb() -> [UInt32] {
            (0..<width * height).map { i in
                UInt32(pixels[i * 4 + 3]) << 24 | UInt32(pixels[i * 4]) << 16 | UInt32(pixels[i * 4 + 1]) << 8
                    | UInt32(pixels[i * 4 + 2])
            }
        }
    }

    /// What a band of rows needs of its `Target`, as plain values.
    private struct RasterTarget {
        let width: Int
        let height: Int
        let pixels: UnsafeMutablePointer<UInt8>
        let depth: UnsafeMutablePointer<Float>?
    }

    /// A texture's pixels while one mesh is drawn.
    private struct TextureView {
        let width: Int
        let height: Int
        let format: Int
        let stride: Int
        let pixels: UnsafeBufferPointer<UInt8>
    }

    private struct Modulate {
        let mode: Int
        let type: Int
        let r: SIMD4<Float>
        let g: SIMD4<Float>
        let b: SIMD4<Float>
        let texture: FflTexture?

        init(_ mode: Int, _ type: Int, r: SIMD4<Float>? = nil, g: SIMD4<Float>? = nil, b: SIMD4<Float>? = nil,
             texture: FflTexture? = nil) {
            self.mode = mode
            self.type = type
            // A colour the PC leaves out reads as white.
            self.r = r.map(Modulate.clamped) ?? white
            self.g = g.map(Modulate.clamped) ?? white
            self.b = b.map(Modulate.clamped) ?? white
            self.texture = texture
        }

        private static func clamped(_ colour: SIMD4<Float>) -> SIMD4<Float> {
            SIMD4(clamp01(colour[0]), clamp01(colour[1]), clamp01(colour[2]), clamp01(colour[3]))
        }
    }

    /// A `Modulate` while one mesh is drawn: plain values, so shading a pixel retains nothing.
    private struct ModulateView {
        let mode: Int
        let r: SIMD4<Float>
        let g: SIMD4<Float>
        let b: SIMD4<Float>
        let texture: TextureView?

        init(_ modulate: Modulate, _ texture: TextureView?) {
            mode = modulate.mode
            r = modulate.r
            g = modulate.g
            b = modulate.b
            self.texture = texture
        }
    }

    /// A material's light products per channel (the fourth lane is unused).
    private struct Material {
        let ambient: SIMD4<Float>
        let diffuse: SIMD4<Float>
        let specular: SIMD4<Float>
        let specularPower: Float
        let anisotropic: Bool
        let rim: SIMD4<Float>
        let hasSpecular: Bool
    }

    private struct Draw {
        let positions: [Float]
        let texcoords: [Float]
        let normals: [Float]
        let tangents: [Float]
        let parameters: [Float]
        let indices: [Int]
        let cull: Int
        let modulate: Modulate
        let material: Material
    }

    /// Per vertex: screen x, y, depth, 1/w, u, v, view position, normal, tangent and the four parameters.
    private struct Prepared {
        let draw: Draw
        let vertices: [Float]
    }

    /// A Mii's body (BodyRenderData): its meshes, their scale, and where the head sits on them.
    private struct Body {
        let draws: [Draw]
        let scale: Mat4
        let headTranslation: [Float]
    }

    private enum Origin { case center, left, right }

    private struct MaskPart {
        let x: Float
        let y: Float
        let scaleX: Float
        let scaleY: Float
        let rotation: Float
        let origin: Origin
    }

    /// Where each face part sits on the mask (BuildRawMaskParts).
    private struct MaskParts {
        let eyeR: MaskPart
        let eyeL: MaskPart
        let eyebrowR: MaskPart
        let eyebrowL: MaskPart
        let mouth: MaskPart
        let mustacheR: MaskPart
        let mustacheL: MaskPart
        let mole: MaskPart

        init(_ info: FflCharInfo) {
            let posXAdd: Float = 3.5323312
            let posYAdd: Float = 4.629278
            let spacingMul: Float = 0.88961464
            let posXMul: Float = 1.7792293
            let posYMul: Float = 1.0760943
            let posYAddEye = posYAdd + 13.822246
            let posYAddEyebrow = posYAdd + 11.920528
            let posYAddMouth = posYAdd + 24.629572
            let posYAddMustache = posYAdd + 27.134275
            let posXAddMole = posXAdd + 14.233834
            let posYAddMole = posYAdd + 11.178394 + 2 * posYMul

            let eyeSpacing = Float(info.eyeSpacingX) * spacingMul
            let eyeBase = 0.4 * Float(info.eyeScale) + 1
            let eyeBaseY = 0.12 * Float(info.eyeScaleY) + 0.64
            let eyeScaleX = 5.34375 * eyeBase
            let eyeScaleY = 4.5 * eyeBase * eyeBaseY
            let eyeY = Float(info.eyePositionY) * posYMul + posYAddEye
            let eyeTurn = (info.eyeRotate + 32 - eyeRotate[coerce(info.eyeType, 0, eyeRotate.count - 1)]) % 32
            let eyeRotation = Float(eyeTurn) * (Float(360) / 32)

            let browSpacing = Float(info.eyebrowSpacingX) * spacingMul
            let browBase = 0.4 * Float(info.eyebrowScale) + 1
            let browBaseY = 0.12 * Float(info.eyebrowScaleY) + 0.64
            let browScaleX = 5.0625 * browBase
            let browScaleY = 4.5 * browBase * browBaseY
            let browY = Float(info.eyebrowPositionY) * posYMul + posYAddEyebrow
            let browTurn = (info.eyebrowRotate + 32 - eyebrowRotate[coerce(info.eyebrowType, 0, eyebrowRotate.count - 1)]) % 32
            let browRotation = Float(browTurn) * (Float(360) / 32)

            let mouthBase = 0.4 * Float(info.mouthScale) + 1
            let mouthBaseY = 0.12 * Float(info.mouthScaleY) + 0.64
            let mustacheBase = 0.4 * Float(info.mustacheScale) + 1
            let mustacheY = Float(info.mustachePositionY) * posYMul + posYAddMustache
            let moleScale = 0.4 * Float(info.moleScale) + 1

            eyeR = MaskPart(x: 32 - eyeSpacing, y: eyeY, scaleX: eyeScaleX, scaleY: eyeScaleY, rotation: eyeRotation, origin: .left)
            eyeL = MaskPart(x: eyeSpacing + 32, y: eyeY, scaleX: eyeScaleX, scaleY: eyeScaleY, rotation: 360 - eyeRotation,
                            origin: .right)
            eyebrowR = MaskPart(x: 32 - browSpacing, y: browY, scaleX: browScaleX, scaleY: browScaleY, rotation: browRotation,
                                origin: .left)
            eyebrowL = MaskPart(x: browSpacing + 32, y: browY, scaleX: browScaleX, scaleY: browScaleY,
                                rotation: 360 - browRotation, origin: .right)
            mouth = MaskPart(x: 32, y: Float(info.mouthPositionY) * posYMul + posYAddMouth, scaleX: 6.1875 * mouthBase,
                             scaleY: 4.5 * mouthBase * mouthBaseY, rotation: 0, origin: .center)
            mustacheR = MaskPart(x: 32, y: mustacheY, scaleX: 4.5 * mustacheBase, scaleY: 9.0 * mustacheBase, rotation: 0,
                                 origin: .left)
            mustacheL = MaskPart(x: 32, y: mustacheY, scaleX: 4.5 * mustacheBase, scaleY: 9.0 * mustacheBase, rotation: 0,
                                 origin: .right)
            mole = MaskPart(x: Float(info.molePositionX) * posXMul + posXAddMole,
                            y: Float(info.molePositionY) * posYMul + posYAddMole,
                            scaleX: moleScale, scaleY: moleScale, rotation: 0, origin: .center)
        }
    }

    private static let vertexStride = 19
    private static let cullNone = 0
    private static let cullBack = 1
    private static let cullFront = 2
    private static let typeFaceline = 0
    private static let typeBeard = 1
    private static let typeNose = 2
    private static let typeForehead = 3
    private static let typeHair = 4
    private static let typeCap = 5
    private static let typeMask = 6
    private static let typeNoseline = 7
    private static let typeGlass = 8
    private static let typeBody = 9
    private static let typePants = 10

    // MiiLightingProfiles.Default on the PC.
    private static let profileAmbient: Float = 0.71
    private static let profileDirectional: Float = 0.32
    private static let profileDiffuseScale: Float = 1.32
    private static let profileDiffuseFloor: Float = 0.23
    private static let profileSpecular: Float = 0.78
    private static let profileRimScale: Float = 0.88
    private static let profileRimPower: Float = 2.56

    private static let white = SIMD4<Float>(1, 1, 1, 1)
    private static let black = SIMD4<Float>(0, 0, 0, 1)
    private static let mole = SIMD4<Float>(0.071, 0.059, 0.059, 1)
    /// The PC's trousers: the grey of an ordinary Mii's.
    private static let pants = SIMD4<Float>(0.2509804, 0.2745099, 0.30588239, 1)
    private static let light = FflResource.normalized(-0.4531539381, 0.4226179123, 0.7848858833, threshold: 0, fallbackZ: 1)
    private static let lightX = light.0
    private static let lightY = light.1
    private static let lightZ = light.2
    private static let noNoseExpressions: Set<Int> = [49, 50, 51, 52, 61, 62]
    private static let directEyes: Set<Int> = [60, 62, 65, 69, 70, 71, 72, 73, 74, 75, 78, 79]
    private static let flatNormals: [Float] = (0..<12).map { $0 % 3 == 2 ? 1 : 0 }
    private static let flatParameters: [Float] = (0..<16).map { $0 % 4 == 2 ? 0 : 1 }

    /// Right eye, left eye, mouth and eyebrow per expression; 0 keeps the Mii's own part.
    private static let expressionElements: [[Int]] = [
        [0, 0, 0, 0], [1, 1, 0, 0], [0, 0, 1, 0], [2, 2, 2, 0],
        [3, 3, 0, 0], [4, 4, 0, 0], [0, 0, 3, 0], [1, 1, 3, 0],
        [0, 0, 3, 0], [2, 2, 3, 0], [3, 3, 3, 0], [4, 4, 3, 0],
        [5, 0, 0, 0], [0, 5, 0, 0], [5, 0, 3, 0], [0, 5, 3, 0],
        [5, 0, 5, 0], [0, 5, 5, 0], [5, 5, 2, 0],
    ]

    private static let eyeRotate = [
        3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4, 4, 3, 3, 4, 4, 4, 3, 3, 4, 3, 4, 3, 3, 4, 3, 4, 4, 3, 4, 4, 4, 3, 3, 3, 4, 4, 3, 3,
        3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 3, 4, 4, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    ]
    private static let eyebrowRotate = [6, 6, 5, 7, 6, 7, 6, 7, 4, 7, 6, 8, 5, 5, 6, 6, 7, 7, 6, 6, 5, 6, 7, 5, 6, 6, 6, 6]

    private static let materials: [Material] = [
        material([0.85, 0.75, 0.75], 0.75, 0.30, 1.2, false, 0.3),
        material([1.0, 1.0, 1.0], 0.7, 0.0, 40.0, false, 0.3),
        material([0.90, 0.85, 0.85], 0.75, 0.22, 1.5, false, 0.3),
        material([0.85, 0.75, 0.75], 0.75, 0.30, 1.2, false, 0.3),
        material([1.00, 1.00, 1.00], 0.70, 0.10, 10.0, true, 0.3),
        material([0.75, 0.75, 0.75], 0.72, 0.30, 1.5, false, 0.3),
        material([1.0, 1.0, 1.0], 0.7, 0.0, 40.0, true, 0.3),
        material([1.0, 1.0, 1.0], 0.7, 0.0, 40.0, true, 0.3),
        material([1.0, 1.0, 1.0], 0.7, 0.0, 40.0, true, 0.3),
        // The body and the trousers.
        material([0.95622, 0.95622, 0.95622], 0.496733, 0.2409, 3.0, false, 0.4),
        material([0.95622, 0.95622, 0.95622], 1.084967, 0.2409, 3.0, false, 0.4),
    ]

    /// The PC multiplies its light colours into each material per pixel; the products are the same.
    private static func material(_ ambient: [Float], _ diffuse: Float, _ specular: Float, _ power: Float, _ anisotropic: Bool,
                                 _ rim: Float) -> Material {
        let specularProduct: Float = 0.70 * specular
        return Material(
            ambient: SIMD4(0.73 * ambient[0], 0.73 * ambient[1], 0.73 * ambient[2], 0),
            diffuse: SIMD4(0.60 * diffuse, 0.60 * diffuse, 0.60 * diffuse, 0),
            specular: SIMD4(specularProduct, specularProduct, specularProduct, 0),
            specularPower: power,
            anisotropic: anisotropic,
            rim: SIMD4(rim, rim, rim, 0),
            hasSpecular: specularProduct != 0)
    }
}

/// A 4x4 matrix in System.Numerics' layout: row vectors, translation in the last row.
struct Mat4 {
    let m: [Float]

    static func * (lhs: Mat4, rhs: Mat4) -> Mat4 {
        let a = lhs.m
        let o = rhs.m
        var result = [Float](repeating: 0, count: 16)
        for row in 0..<4 {
            for column in 0..<4 {
                let first = a[row * 4] * o[column] + a[row * 4 + 1] * o[4 + column]
                result[row * 4 + column] = first + a[row * 4 + 2] * o[8 + column] + a[row * 4 + 3] * o[12 + column]
            }
        }
        return Mat4(m: result)
    }

    func transformPoint(_ x: Float, _ y: Float, _ z: Float) -> (Float, Float, Float) {
        (x * m[0] + y * m[4] + z * m[8] + m[12],
         x * m[1] + y * m[5] + z * m[9] + m[13],
         x * m[2] + y * m[6] + z * m[10] + m[14])
    }

    func transformNormal(_ x: Float, _ y: Float, _ z: Float) -> (Float, Float, Float) {
        (x * m[0] + y * m[4] + z * m[8],
         x * m[1] + y * m[5] + z * m[9],
         x * m[2] + y * m[6] + z * m[10])
    }

    /// (x, y, z, 1) times this matrix.
    func transform4(_ x: Float, _ y: Float, _ z: Float) -> (Float, Float, Float, Float) {
        (x * m[0] + y * m[4] + z * m[8] + m[12],
         x * m[1] + y * m[5] + z * m[9] + m[13],
         x * m[2] + y * m[6] + z * m[10] + m[14],
         x * m[3] + y * m[7] + z * m[11] + m[15])
    }

    static let identity = Mat4(m: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])

    /// Matrix4x4.CreateScale.
    static func scale(_ scale: [Float]) -> Mat4 {
        Mat4(m: [scale[0], 0, 0, 0, 0, scale[1], 0, 0, 0, 0, scale[2], 0, 0, 0, 0, 1])
    }

    /// Matrix4x4.CreateTranslation.
    static func translation(_ offset: [Float]) -> Mat4 {
        Mat4(m: [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, offset[0], offset[1], offset[2], 1])
    }

    /// CreateRotationMatrix on the PC: CreateRotationX, then Y, then Z, each by `radians`' angle.
    static func rotation(_ radians: [Float]) -> Mat4 {
        let x = (cosF(radians[0]), sinF(radians[0]))
        let y = (cosF(radians[1]), sinF(radians[1]))
        let z = (cosF(radians[2]), sinF(radians[2]))
        let rotateX = Mat4(m: [1, 0, 0, 0, 0, x.0, x.1, 0, 0, -x.1, x.0, 0, 0, 0, 0, 1])
        let rotateY = Mat4(m: [y.0, 0, -y.1, 0, 0, 1, 0, 0, y.1, 0, y.0, 0, 0, 0, 0, 1])
        let rotateZ = Mat4(m: [z.0, z.1, 0, 0, -z.1, z.0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1])
        return rotateX * rotateY * rotateZ
    }

    /// Matrix4x4.CreateLookAt (right-handed).
    static func lookAt(_ position: [Float], _ target: [Float], _ up: [Float]) -> Mat4 {
        let axisZ = FflResource.normalized(position[0] - target[0], position[1] - target[1], position[2] - target[2],
                                           threshold: 0, fallbackZ: 1)
        let crossX = up[1] * axisZ.2 - up[2] * axisZ.1
        let crossY = up[2] * axisZ.0 - up[0] * axisZ.2
        let crossZ = up[0] * axisZ.1 - up[1] * axisZ.0
        let axisX = FflResource.normalized(crossX, crossY, crossZ, threshold: 0, fallbackZ: 1)
        let axisY = (axisZ.1 * axisX.2 - axisZ.2 * axisX.1,
                     axisZ.2 * axisX.0 - axisZ.0 * axisX.2,
                     axisZ.0 * axisX.1 - axisZ.1 * axisX.0)
        func dot(_ axis: (Float, Float, Float)) -> Float {
            axis.0 * -position[0] + axis.1 * -position[1] + axis.2 * -position[2]
        }
        return Mat4(m: [
            axisX.0, axisY.0, axisZ.0, 0,
            axisX.1, axisY.1, axisZ.1, 0,
            axisX.2, axisY.2, axisZ.2, 0,
            dot(axisX), dot(axisY), dot(axisZ), 1,
        ])
    }

    /// Matrix4x4.CreatePerspectiveFieldOfView (right-handed, depth 0 to 1).
    static func perspective(_ fieldOfView: Float, _ aspect: Float, _ near: Float, _ far: Float) -> Mat4 {
        let height = 1 / tanF(fieldOfView * 0.5)
        let width = height / aspect
        let range = far / (near - far)
        return Mat4(m: [
            width, 0, 0, 0,
            0, height, 0, 0,
            0, 0, range, -1,
            0, 0, range * near, 0,
        ])
    }
}

/// FFL's colour tables, as the PC resolves them (sRGB): the Kotlin's Colors object.
enum MiiColors {
    static func faceline(_ index: Int) -> SIMD4<Float> { facelineTable[coerce(index, 0, facelineTable.count - 1)] }
    static func favorite(_ index: Int) -> SIMD4<Float> { favoriteTable[coerce(index, 0, favoriteTable.count - 1)] }
    static func hair(_ encoded: Int) -> SIMD4<Float> { common(encoded, commonTable) ?? palette(encoded, hairTable) }
    static func glass(_ encoded: Int) -> SIMD4<Float> { common(encoded, commonTable) ?? palette(encoded, glassTable) }
    static func eyeB(_ encoded: Int) -> SIMD4<Float> { common(encoded, commonTable) ?? palette(encoded, eyeTable) }
    static func mouthR(_ encoded: Int) -> SIMD4<Float> { common(encoded, commonTable) ?? palette(encoded, mouthRTable) }
    static func mouthG(_ encoded: Int) -> SIMD4<Float> { common(encoded, upperLipTable) ?? palette(encoded, mouthGTable) }

    private static func common(_ encoded: Int, _ table: [SIMD4<Float>]) -> SIMD4<Float>? {
        if encoded & FflCharInfo.commonColor == 0 { return nil }
        let index = encoded & 0xFF
        return index < table.count ? table[index] : table[0]
    }

    private static func palette(_ encoded: Int, _ table: [SIMD4<Float>]) -> SIMD4<Float> {
        let index = encoded & FflCharInfo.commonColor != 0 ? encoded & 0xFF : encoded
        return table[coerce(index, 0, table.count - 1)]
    }

    private static func rgb(_ r: Float, _ g: Float, _ b: Float) -> SIMD4<Float> { SIMD4(r, g, b, 1) }

    private static let facelineTable = [
        rgb(1.000, 0.827, 0.678), rgb(1.000, 0.714, 0.420), rgb(0.870, 0.475, 0.259),
        rgb(1.000, 0.667, 0.549), rgb(0.678, 0.318, 0.161), rgb(0.388, 0.173, 0.094),
    ]
    private static let commonTable = [
        rgb(0.1764706, 0.1568628, 0.1568628), rgb(0.2509804, 0.1254902, 0.0627451), rgb(0.3607844, 0.0941177, 0.0392157),
        rgb(0.4862746, 0.2274510, 0.0784314), rgb(0.4705883, 0.4705883, 0.5019608), rgb(0.3058824, 0.2431373, 0.0627451),
        rgb(0.5333334, 0.3450981, 0.0941177), rgb(0.8156863, 0.6274510, 0.2901961), rgb(0.0000000, 0.0000000, 0.0000000),
        rgb(0.4235295, 0.4392157, 0.4392157), rgb(0.4000000, 0.2352942, 0.1725491), rgb(0.3764706, 0.3686275, 0.1882353),
        rgb(0.2745099, 0.3294118, 0.6588236), rgb(0.2196079, 0.4392157, 0.3450981), rgb(0.3764706, 0.2196079, 0.0627451),
        rgb(0.6588236, 0.0627451, 0.0313726), rgb(0.1254902, 0.1882353, 0.4078432), rgb(0.6588236, 0.3764706, 0.0000000),
        rgb(0.4705883, 0.4392157, 0.4078432), rgb(0.8470589, 0.3215687, 0.0313726), rgb(0.9411765, 0.0470589, 0.0313726),
        rgb(0.9607844, 0.2823530, 0.2823530), rgb(0.9411765, 0.6039216, 0.4549020), rgb(0.5490197, 0.3137255, 0.2509804),
    ]
    private static let upperLipTable = [
        rgb(0.0901961, 0.0784314, 0.0784314), rgb(0.1254902, 0.0627451, 0.0313726), rgb(0.1803922, 0.0470589, 0.0196079),
        rgb(0.2901961, 0.1372550, 0.0470589), rgb(0.3294118, 0.3294118, 0.3529412), rgb(0.1529412, 0.1215687, 0.0313726),
        rgb(0.3215687, 0.2078432, 0.0549020), rgb(0.6941177, 0.5019608, 0.1568628), rgb(0.0000000, 0.0000000, 0.0000000),
        rgb(0.2980393, 0.3058824, 0.3058824), rgb(0.2000000, 0.1176471, 0.0862746), rgb(0.2274510, 0.2196079, 0.1137255),
        rgb(0.1647059, 0.1960785, 0.3960785), rgb(0.1529412, 0.3058824, 0.2431373), rgb(0.1882353, 0.1098040, 0.0313726),
        rgb(0.3960785, 0.0392157, 0.0196079), rgb(0.0627451, 0.0941177, 0.2039216), rgb(0.4627451, 0.2627451, 0.0000000),
        rgb(0.3294118, 0.3058824, 0.2862746), rgb(0.5098040, 0.1882353, 0.0941177), rgb(0.4705883, 0.0470589, 0.0470589),
        rgb(0.5333334, 0.1254902, 0.1568628), rgb(0.8627451, 0.4705883, 0.3137255), rgb(0.2745099, 0.1176471, 0.0392157),
    ]
    private static let hairTable = [
        rgb(0.118, 0.102, 0.094), rgb(0.251, 0.125, 0.063), rgb(0.361, 0.094, 0.039), rgb(0.486, 0.227, 0.078),
        rgb(0.471, 0.471, 0.502), rgb(0.306, 0.243, 0.063), rgb(0.533, 0.345, 0.094), rgb(0.816, 0.627, 0.290),
    ]
    private static let glassTable = [
        rgb(0.094, 0.094, 0.094), rgb(0.376, 0.219, 0.062), rgb(0.658, 0.062, 0.031),
        rgb(0.125, 0.188, 0.407), rgb(0.658, 0.376, 0.000), rgb(0.470, 0.439, 0.407),
    ]
    private static let eyeTable = [
        rgb(0.000, 0.000, 0.000), rgb(0.424, 0.439, 0.439), rgb(0.400, 0.235, 0.173),
        rgb(0.376, 0.369, 0.188), rgb(0.275, 0.329, 0.659), rgb(0.220, 0.439, 0.345),
    ]
    private static let mouthRTable = [
        rgb(0.847, 0.322, 0.031), rgb(0.941, 0.047, 0.031), rgb(0.961, 0.282, 0.282),
        rgb(0.941, 0.604, 0.455), rgb(0.549, 0.314, 0.251),
    ]
    private static let mouthGTable = [
        rgb(0.510, 0.188, 0.094), rgb(0.471, 0.047, 0.047), rgb(0.533, 0.125, 0.157),
        rgb(0.863, 0.471, 0.314), rgb(0.275, 0.118, 0.039),
    ]
    private static let favoriteTable = [
        rgb(0.824, 0.118, 0.078), rgb(1.000, 0.431, 0.098), rgb(1.000, 0.847, 0.125), rgb(0.471, 0.824, 0.125),
        rgb(0.000, 0.471, 0.188), rgb(0.039, 0.282, 0.706), rgb(0.235, 0.667, 0.871), rgb(0.961, 0.353, 0.490),
        rgb(0.451, 0.157, 0.678), rgb(0.282, 0.220, 0.094), rgb(0.878, 0.878, 0.878), rgb(0.094, 0.094, 0.078),
    ]
}

/// FFL's description of a Mii (FFLiCharInfo's parts), made the way the PC makes it: the Wii Mii is
/// turned into Mii Studio's 46 values (MiiStudioDataSerializer.GenerateStudioDataArray), which are
/// then read as FFL parts (NativeMiiRenderer.MapStudioDataToCharInfo).
struct FflCharInfo {
    /// FFL's flag for a colour from the common table: the top bit of a 32-bit int, as in the Kotlin.
    static let commonColor = Int(Int32.min)

    let beardColor: Int
    let beardType: Int
    let build: Int
    let eyeScaleY: Int
    let eyeColor: Int
    let eyeRotate: Int
    let eyeScale: Int
    let eyeType: Int
    let eyeSpacingX: Int
    let eyePositionY: Int
    let eyebrowScaleY: Int
    let eyebrowColor: Int
    let eyebrowRotate: Int
    let eyebrowScale: Int
    let eyebrowType: Int
    let eyebrowSpacingX: Int
    let eyebrowPositionY: Int
    let facelineColor: Int
    let faceMakeup: Int
    let faceType: Int
    let faceLine: Int
    let favoriteColor: Int
    let gender: Int
    let glassColor: Int
    let glassScale: Int
    let glassType: Int
    let glassPositionY: Int
    let hairColor: Int
    let hairDir: Int
    let hairType: Int
    let height: Int
    let moleScale: Int
    let moleType: Int
    let molePositionX: Int
    let molePositionY: Int
    let mouthScaleY: Int
    let mouthColor: Int
    let mouthScale: Int
    let mouthType: Int
    let mouthPositionY: Int
    let mustacheScale: Int
    let mustacheType: Int
    let mustachePositionY: Int
    let noseScale: Int
    let noseType: Int
    let nosePositionY: Int

    init(studio: [Int]) {
        beardColor = studio[0] | FflCharInfo.commonColor
        beardType = studio[1]
        build = studio[2]
        eyeScaleY = studio[3]
        eyeColor = studio[4] | FflCharInfo.commonColor
        eyeRotate = studio[5]
        eyeScale = studio[6]
        eyeType = studio[7]
        eyeSpacingX = studio[8]
        eyePositionY = studio[9]
        eyebrowScaleY = studio[10]
        eyebrowColor = studio[11] | FflCharInfo.commonColor
        eyebrowRotate = studio[12]
        eyebrowScale = studio[13]
        eyebrowType = studio[14]
        eyebrowSpacingX = studio[15]
        eyebrowPositionY = studio[16]
        facelineColor = studio[17]
        faceMakeup = studio[18]
        faceType = studio[19]
        faceLine = studio[20]
        favoriteColor = studio[21]
        gender = studio[22]
        glassColor = studio[23] | FflCharInfo.commonColor
        glassScale = studio[24]
        glassType = studio[25]
        glassPositionY = studio[26]
        hairColor = studio[27] | FflCharInfo.commonColor
        hairDir = studio[28]
        hairType = studio[29]
        height = studio[30]
        moleScale = studio[31]
        moleType = studio[32]
        molePositionX = studio[33]
        molePositionY = studio[34]
        mouthScaleY = studio[35]
        mouthColor = studio[36] | FflCharInfo.commonColor
        mouthScale = studio[37]
        mouthType = studio[38]
        mouthPositionY = studio[39]
        mustacheScale = studio[40]
        mustacheType = studio[41]
        mustachePositionY = studio[42]
        noseScale = studio[43]
        noseType = studio[44]
        nosePositionY = studio[45]
    }

    private static let makeup = [0, 1, 6, 9, 0, 0, 0, 0, 0, 10, 0, 0]
    private static let wrinkles = [0, 0, 0, 0, 5, 2, 3, 7, 8, 0, 9, 11]

    static func of(_ mii: Mii) -> FflCharInfo { FflCharInfo(studio: studio(mii)) }

    /// GenerateStudioDataArray: the Wii Mii's look as Mii Studio's values.
    static func studio(_ mii: Mii) -> [Int] {
        var s = [Int](repeating: 0, count: 46)
        s[0x16] = mii.girl ? 1 : 0
        s[0x15] = mii.favoriteColor
        s[0x1E] = mii.height
        s[2] = mii.weight
        s[0x13] = mii.faceShape
        s[0x11] = mii.skinColor
        s[0x14] = wrinkles.indices.contains(mii.facialFeature) ? wrinkles[mii.facialFeature] : 0
        s[0x12] = makeup.indices.contains(mii.facialFeature) ? makeup[mii.facialFeature] : 0
        s[0x1D] = mii.hairType
        s[0x1B] = mii.hairColor == 0 ? 8 : mii.hairColor
        s[0x1C] = mii.hairFlipped ? 1 : 0
        s[0xE] = mii.eyebrowType
        s[0xC] = mii.eyebrowRotation
        s[0xB] = mii.eyebrowColor == 0 ? 8 : mii.eyebrowColor
        s[0xD] = mii.eyebrowSize
        s[0xA] = 3
        s[0x10] = mii.eyebrowVertical
        s[0xF] = mii.eyebrowSpacing
        s[7] = mii.eyeType
        s[5] = mii.eyeRotation
        s[9] = mii.eyeVertical
        s[4] = mii.eyeColor + 8
        s[6] = mii.eyeSize
        s[3] = 3
        s[8] = mii.eyeSpacing
        s[0x2C] = mii.noseType
        s[0x2B] = mii.noseSize
        s[0x2D] = mii.noseVertical
        s[0x26] = mii.lipType
        s[0x24] = mii.lipColor < 4 ? mii.lipColor + 19 : 0
        s[0x25] = mii.lipSize
        s[0x23] = 3
        s[0x27] = mii.lipVertical
        s[0x29] = mii.mustacheType
        s[1] = mii.beardType
        s[0] = mii.facialHairColor == 0 ? 8 : mii.facialHairColor
        s[0x28] = mii.mustacheSize
        s[0x2A] = mii.mustacheVertical
        s[0x19] = mii.glassesType
        s[0x17] = mii.glassesColor == 0 ? 8 : mii.glassesColor < 6 ? mii.glassesColor + 13 : 0
        s[0x18] = mii.glassesSize
        s[0x1A] = mii.glassesVertical
        s[0x20] = mii.moleEnabled ? 1 : 0
        s[0x1F] = mii.moleSize
        s[0x22] = mii.moleVertical
        s[0x21] = mii.moleHorizontal
        return s
    }
}

// MARK: - kotlin.math on the JVM

/// Kotlin's float trigonometry and powers on the JVM go through double precision (Math.sin and
/// the like) and round back to float; these do the same, so every angle and power matches.
@inline(__always) private func sinF(_ x: Float) -> Float { Float(sin(Double(x))) }
@inline(__always) private func cosF(_ x: Float) -> Float { Float(cos(Double(x))) }
@inline(__always) private func tanF(_ x: Float) -> Float { Float(tan(Double(x))) }
@inline(__always) private func powF(_ x: Float, _ y: Float) -> Float { Float(pow(Double(x), Double(y))) }

/// Math.PI.toFloat(): π rounded to the nearest float. Swift's Float.pi is rounded toward zero,
/// one unit in the last place below it, so it cannot stand in.
private let piF = Float(Double.pi)

/// kotlin.math.round: Math.rint, which rounds halves to the even neighbour.
@inline(__always) private func roundF(_ x: Float) -> Float { x.rounded(.toNearestOrEven) }
@inline(__always) private func floorF(_ x: Float) -> Float { x.rounded(.down) }
@inline(__always) private func ceilF(_ x: Float) -> Float { x.rounded(.up) }

/// Math.max on floats: a NaN wins and +0 is above -0, where Swift's max keeps the first argument.
@inline(__always) private func maxF(_ a: Float, _ b: Float) -> Float {
    if a.isNaN { return a }
    if a == 0 && b == 0 && a.sign == .minus { return b }
    return a >= b ? a : b
}

/// Math.min on floats: a NaN wins and -0 is below +0.
@inline(__always) private func minF(_ a: Float, _ b: Float) -> Float {
    if a.isNaN { return a }
    if a == 0 && b == 0 && b.sign == .minus { return b }
    return a <= b ? a : b
}

/// Kotlin's Float.toInt(): toward zero, NaN as 0, and saturating at a 32-bit Int's range where
/// Swift's Int(_:) would trap.
@inline(__always) private func toInt(_ x: Float) -> Int {
    if x.isNaN { return 0 }
    if x >= 2_147_483_648 { return Int(Int32.max) }
    if x <= -2_147_483_648 { return Int(Int32.min) }
    return Int(x)
}

/// coerceIn: a NaN passes through.
@inline(__always) private func coerce(_ x: Float, _ low: Float, _ high: Float) -> Float {
    x < low ? low : (x > high ? high : x)
}

@inline(__always) private func coerce(_ x: Int, _ low: Int, _ high: Int) -> Int {
    x < low ? low : (x > high ? high : x)
}

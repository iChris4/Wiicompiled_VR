package org.wiicompiled.quest.launcher

import java.io.IOException
import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.round
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * Draws a Mii from [FflResource] on the CPU: the PC launcher's NativeMiiRenderer ported to
 * Kotlin, with the same parts, face and mask textures, colours, camera, lighting and rasteriser,
 * so a Mii looks the same in both launchers.
 *
 * Pictures are the PC's face framing: the head, and below it the upper body in the Mii's
 * favourite colour, from the 3DS bodies ([MiiBodies]) once they are downloaded; without them,
 * the head alone (the PC's face_only). One deliberate fix: the PC colours a beard with the hair
 * colour; here it has its facial hair colour, as on the Wii.
 */
object MiiRenderer {
    /**
     * How the Mii is turned and seen, in degrees, as the PC's MiiImageSpecifications turn it:
     * the Mii about its X, Y and Z axes, and the camera orbiting its head.
     */
    class Pose(
        val characterX: Float,
        val characterY: Float,
        val characterZ: Float,
        val cameraX: Float,
        val cameraY: Float,
        val cameraZ: Float,
        /** The camera's distance as a share of the PC's (its CameraZoom): below 1 the Mii fills more. */
        val zoom: Float = 1f,
    ) {
        /** Tells pictures of the same Mii in different poses apart. */
        val key: String get() = "$characterX,$characterY,$characterZ,$cameraX,$cameraY,$cameraZ,$zoom"

        companion object {
            /** Straight ahead, as My Miis and the editor show a Mii. */
            val FRONT = Pose(0f, 0f, 0f, 0f, 0f, 0f)

            /** CurrentUserSideProfile and FriendsSideProfile: the profile page's and sidebar's three-quarter view. */
            val SIDE = Pose(350f, 15f, 355f, 12f, 0f, 0f)
        }
    }

    /**
     * The Mii at [size] x [size], as non-premultiplied ARGB colours, transparent around it: the
     * head, with the upper body below it when [bodies] are given, or with [fullBody] the whole Mii
     * (the PC's all_body) when they are.
     */
    @Throws(IOException::class)
    fun render(
        resource: FflResource,
        mii: Mii,
        size: Int,
        pose: Pose = Pose.FRONT,
        bodies: MiiBodies? = null,
        fullBody: Boolean = false,
        expression: Int = 0,
    ): IntArray = render(resource, FflCharInfo.of(mii), size, expression, pose, bodies, fullBody)

    @Throws(IOException::class)
    internal fun render(
        resource: FflResource,
        info: FflCharInfo,
        size: Int,
        expression: Int = 0,
        pose: Pose = Pose.FRONT,
        bodies: MiiBodies? = null,
        fullBody: Boolean = false,
    ): IntArray {
        require(size in 16..4096 && size % 2 == 0) { "Unsupported picture size $size" }
        val resolution = if (size <= 384) 256 else 512
        val draws = buildDraws(resource, info, resolution, expression)
        if (draws.isEmpty()) throw IOException("The renderer produced no drawable meshes for this Mii.")
        val body = bodies?.let { body(it, info) }

        val target = Target(size, size, withDepth = true)
        target.fill(255, 255, 255, 0)
        // The PC's face view: 15 degrees of field of view on the head, the camera orbiting it
        // (CalculateCameraOrbitPosition) and the head turned about its own origin. With a body
        // the head sits on its shoulders, and the camera rises with it; the body turns about its feet.
        // Its all_body view stands further back, looking at the whole Mii from a fixed place.
        val wholeBody = fullBody && body != null
        val y = if (wholeBody) 90f else 4.805f / 0.14f
        val z = (if (wholeBody) 760f else 57.553f / 0.14f) * pose.zoom
        val camera = radians(pose.cameraX, pose.cameraY, pose.cameraZ)
        val position = floatArrayOf(
            z * -sin(camera[1]) * cos(camera[0]),
            z * sin(camera[0]),
            z * cos(camera[1]) * cos(camera[0]),
        )
        position[1] += y
        val lookAt = floatArrayOf(0f, if (wholeBody) 95f else y, 0f)
        if (body != null && !wholeBody) {
            for (c in 0 until 3) {
                position[c] += body.headTranslation[c]
                lookAt[c] += body.headTranslation[c]
            }
        }
        val up = floatArrayOf(sin(camera[2]), cos(camera[2]), 0f)
        val view = Mat4.lookAt(position, lookAt, up)
        val projection = Mat4.perspective(15f * (Math.PI.toFloat() / 180f), 1f, 10f, 1200f)
        val rotation = Mat4.rotation(radians(pose.characterX, pose.characterY, pose.characterZ))
        val meshes = ArrayList<Prepared>()
        // The body first, so the head is drawn over the collar.
        if (body != null) {
            val bodyModel = rotation * body.scale
            body.draws.mapNotNullTo(meshes) { prepare(it, size, size, bodyModel, view, projection) }
        }
        val headModel = if (body != null) rotation * Mat4.translation(body.headTranslation) else rotation
        draws.mapNotNullTo(meshes) { prepare(it, size, size, headModel, view, projection) }
        drawAll(target, meshes, light = true, Blend.Over)
        return target.argb()
    }

    /** [info]'s body (TryCreateBodyRenderData), or null when [bodies] has none for its gender. */
    private fun body(bodies: MiiBodies, info: FflCharInfo): Body? {
        val meshes = if (Math.floorMod(info.gender, 2) == 1) bodies.female else bodies.male
        if (meshes.isEmpty()) return null
        // CalculateBodyScale: build widens the body, height stretches it.
        val build = info.build.toFloat().coerceIn(0f, 127f)
        val height = info.height.toFloat().coerceIn(0f, 127f)
        val scaleX = (build * (height * 0.003671875f + 0.4f)) / 128.0f + height * 0.001796875f + 0.4f
        val scaleY = height * 0.006015625f + 0.5f
        val headTranslation = floatArrayOf(0f, MiiBodies.HEAD_Y * scaleY * MiiBodies.MODEL_SCALE, 0f)
        val shirt = Colors.favorite(info.favoriteColor)
        val draws = meshes.map { mesh ->
            val count = mesh.positions.size / 3
            val type = if (mesh.pants) TYPE_PANTS else TYPE_BODY
            Draw(
                positions = mesh.positions,
                texcoords = mesh.texcoords,
                normals = mesh.normals,
                tangents = FloatArray(count * 3),
                // The PC's vertex parameters for the body: full specular and rim.
                parameters = FloatArray(count * 4) { if (it % 4 == 2) 0f else 1f },
                indices = mesh.indices,
                cull = CULL_BACK,
                modulate = Modulate(0, type, r = if (mesh.pants) PANTS else shirt),
                material = MATERIALS[type],
            )
        }
        return Body(draws, Mat4.scale(floatArrayOf(scaleX, scaleY, scaleX)), headTranslation)
    }

    /** ConvertDegreesToRadians: each angle brought into -180..180 by an IEEE remainder first. */
    private fun radians(x: Float, y: Float, z: Float): FloatArray =
        floatArrayOf(x, y, z).also { angles ->
            for (i in angles.indices) angles[i] = Math.IEEEremainder(angles[i].toDouble(), 360.0).toFloat() * (Math.PI.toFloat() / 180f)
        }

    /**
     * Draws [meshes] in order. A large picture is split into bands of rows drawn in parallel,
     * each band drawing every mesh in order, so the picture is the same as one pass.
     */
    private fun drawAll(target: Target, meshes: List<Prepared>, light: Boolean, blend: Blend) {
        val height = target.height
        val bands = if (height >= PARALLEL_SIZE) BANDS else 1
        if (bands == 1) {
            for (mesh in meshes) rasterize(target, mesh, light, blend, 0, height)
            return
        }
        val rows = (height + bands - 1) / bands
        val tasks = (0 until bands).map { band ->
            java.util.concurrent.Callable {
                for (mesh in meshes) rasterize(target, mesh, light, blend, band * rows, min(height, (band + 1) * rows))
            }
        }
        for (future in bandWorkers.invokeAll(tasks)) {
            try {
                future.get()
            } catch (e: java.util.concurrent.ExecutionException) {
                throw e.cause ?: e
            }
        }
    }

    private const val PARALLEL_SIZE = 256
    private val BANDS = Runtime.getRuntime().availableProcessors().coerceIn(1, 4)
    private val bandWorkers by lazy {
        java.util.concurrent.Executors.newFixedThreadPool(BANDS) { runnable -> Thread(runnable, "MiiRaster").apply { isDaemon = true } }
    }

    /** The face parts the editor pictures from their textures rather than as a head. */
    enum class Part { Eyebrow, Eye, Nose, Mouth, Glasses, Mustache }

    /**
     * One choice of a face part, drawn alone from its texture in the Mii's colours as the face
     * would show it, [size] x [size] and transparent around it: the pictures on the editor's
     * choice buttons, which the PC draws from icons of its own. Null for a choice that is no part
     * at all, such as no glasses.
     */
    @Throws(IOException::class)
    fun partIcon(resource: FflResource, mii: Mii, part: Part, index: Int, size: Int): IntArray? {
        val info = FflCharInfo.of(
            when (part) {
                Part.Eyebrow -> mii.copy(eyebrowType = index)
                Part.Eye -> mii.copy(eyeType = index)
                Part.Nose -> mii.copy(noseType = index)
                Part.Mouth -> mii.copy(lipType = index)
                Part.Glasses -> mii.copy(glassesType = index)
                Part.Mustache -> mii.copy(mustacheType = index)
            },
        )
        // The texture, how it is coloured, whether its other half is its mirror image, and the
        // part of it worth showing (noses sit small in the middle of theirs).
        val texture: FflTexture
        val modulate: Modulate
        var mirrored = false
        var window = 0f
        when (part) {
            Part.Eyebrow -> {
                texture = resource.texture(FflResource.TEXTURE_EYEBROW, info.eyebrowType) ?: return null
                modulate = Modulate(3, TYPE_MASK, r = Colors.hair(info.eyebrowColor), texture = texture)
            }
            Part.Eye -> {
                texture = resource.texture(FflResource.TEXTURE_EYE, info.eyeType) ?: return null
                modulate = eyeModulate(info, info.eyeType, texture)
            }
            Part.Nose -> {
                texture = resource.texture(FflResource.TEXTURE_NOSELINE, info.noseType) ?: return null
                modulate = Modulate(3, TYPE_NOSELINE, r = BLACK, texture = texture)
                window = 0.25f
            }
            Part.Mouth -> {
                texture = resource.texture(FflResource.TEXTURE_MOUTH, info.mouthType) ?: return null
                modulate = if (info.mouthType > 36) {
                    Modulate(1, TYPE_MASK, texture = texture)
                } else {
                    Modulate(2, TYPE_MASK, r = Colors.mouthR(info.mouthColor), g = Colors.mouthG(info.mouthColor), b = WHITE, texture = texture)
                }
            }
            Part.Glasses -> {
                texture = resource.texture(FflResource.TEXTURE_GLASS, info.glassType) ?: return null
                modulate = Modulate(4, TYPE_GLASS, r = Colors.glass(info.glassColor), texture = texture)
                mirrored = true
            }
            Part.Mustache -> {
                texture = resource.texture(FflResource.TEXTURE_MUSTACHE, info.mustacheType) ?: return null
                modulate = Modulate(3, TYPE_MASK, r = Colors.hair(info.beardColor), texture = texture)
                mirrored = true
            }
        }
        // "None" is an 8x8 blank.
        if (texture.width <= 8 && texture.height <= 8) return null

        val span = 1f - 2f * window
        val sourceWidth = texture.width * span * (if (mirrored) 2 else 1)
        val sourceHeight = texture.height * span
        val scale = size * 0.9f / max(sourceWidth, sourceHeight)
        val width = sourceWidth * scale
        val height = sourceHeight * scale
        val left = (size - width) / 2f
        val top = (size - height) / 2f
        val out = IntArray(size * size)
        val sample = FloatArray(4)
        val at = FloatArray(STRIDE)
        val scratch = Scratch()
        for (y in 0 until size) {
            for (x in 0 until size) {
                // 2x2 samples per pixel, averaged with their alpha.
                var r = 0f
                var g = 0f
                var b = 0f
                var a = 0f
                for (sy in 0 until 2) {
                    for (sx in 0 until 2) {
                        val fx = (x + 0.25f + sx * 0.5f - left) / width
                        val fy = (y + 0.25f + sy * 0.5f - top) / height
                        if (fx < 0f || fx >= 1f || fy < 0f || fy >= 1f) continue
                        at[4] = window + fx * span * (if (mirrored) 2 else 1)
                        at[5] = window + fy * span
                        if (modulate(modulate, at[4], at[5], sample, scratch)) clampColor(sample)
                        r += sample[0] * sample[3]
                        g += sample[1] * sample[3]
                        b += sample[2] * sample[3]
                        a += sample[3]
                    }
                }
                if (a <= 0f) continue
                val alpha = a / 4f
                out[y * size + x] = (toByte(alpha) shl 24) or (toByte(r / a) shl 16) or (toByte(g / a) shl 8) or toByte(b / a)
            }
        }
        return out
    }

    // --- Building the head (BuildManagedDrawParams) ---

    private fun buildDraws(resource: FflResource, info: FflCharInfo, resolution: Int, expression: Int): List<Draw> {
        val draws = ArrayList<Draw>(12)
        val faceline = resource.shape(FflResource.SHAPE_FACELINE, info.faceType)
            ?: throw IOException("Faceline shape ${info.faceType} is missing.")
        val zero = FloatArray(3)
        val hairPos = faceline.translates?.get(0) ?: zero
        val faceCenter = faceline.translates?.get(1) ?: zero
        val beardPos = faceline.translates?.get(2) ?: zero

        val facelineTexture = facelineTexture(resource, info, resolution)
        val maskTexture = maskTexture(resource, info, resolution, expression)
        val skin = Colors.faceline(info.facelineColor)

        draws += draw(
            faceline, 1f, 1f, null, false, CULL_BACK,
            if (facelineTexture == null) Modulate(0, TYPE_FACELINE, r = skin) else Modulate(1, TYPE_FACELINE, texture = facelineTexture),
        )

        val hairColor = Colors.hair(info.hairColor)
        val hairFlip = info.hairDir > 0
        val hairCull = if (hairFlip) CULL_FRONT else CULL_BACK
        resource.shape(FflResource.SHAPE_HAIR, info.hairType)?.let {
            draws += draw(it, 1f, 1f, hairPos, hairFlip, hairCull, Modulate(0, TYPE_HAIR, r = hairColor))
        }
        resource.shape(FflResource.SHAPE_FOREHEAD, info.hairType)?.let {
            draws += draw(it, 1f, 1f, hairPos, hairFlip, hairCull, Modulate(0, TYPE_FOREHEAD, r = skin))
        }
        resource.texture(FflResource.TEXTURE_CAP, info.hairType)?.let { cap ->
            resource.shape(FflResource.SHAPE_HAT, info.hairType)?.let {
                draws += draw(it, 1f, 1f, hairPos, hairFlip, hairCull, Modulate(5, TYPE_CAP, r = Colors.favorite(info.favoriteColor), texture = cap))
            }
        }
        if (info.beardType in 0 until 4) {
            resource.shape(FflResource.SHAPE_BEARD, info.beardType)?.let {
                draws += draw(it, 1f, 1f, beardPos, false, CULL_BACK, Modulate(0, TYPE_BEARD, r = Colors.hair(info.beardColor)))
            }
        }

        if (expression !in NO_NOSE_EXPRESSIONS) {
            val noseScale = info.noseScale * 0.175f + 0.4f
            val nosePos = floatArrayOf(faceCenter[0], faceCenter[1] + (info.nosePositionY - 8) * -1.5f, faceCenter[2])
            resource.shape(FflResource.SHAPE_NOSE, info.noseType)?.let {
                draws += draw(it, noseScale, noseScale, nosePos, false, CULL_BACK, Modulate(0, TYPE_NOSE, r = skin))
            }
            resource.texture(FflResource.TEXTURE_NOSELINE, info.noseType)?.let { line ->
                resource.shape(FflResource.SHAPE_NOSELINE, info.noseType)?.let {
                    draws += draw(it, noseScale, noseScale, nosePos, false, CULL_BACK, Modulate(3, TYPE_NOSELINE, r = BLACK, texture = line))
                }
            }
            if (maskTexture != null) {
                resource.shape(FflResource.SHAPE_MASK, info.faceType)?.let {
                    val cull = if (resource.linearTextures) CULL_NONE else CULL_BACK
                    draws += draw(it, 1f, 1f, null, false, cull, Modulate(1, TYPE_MASK, texture = maskTexture))
                }
            }
        }

        if (info.glassType > 0) {
            resource.texture(FflResource.TEXTURE_GLASS, info.glassType)?.let { glass ->
                val scale = info.glassScale * (if (resource.linearTextures) 0.175f else 0.15f) + 0.4f
                val position = floatArrayOf(
                    faceCenter[0],
                    faceCenter[1] + (info.glassPositionY - 11) * -1.5f + 5.0f,
                    faceCenter[2] + 2.0f,
                )
                resource.shape(FflResource.SHAPE_GLASS, 0)?.let {
                    draws += draw(it, scale, scale, position, false, CULL_NONE, Modulate(4, TYPE_GLASS, r = Colors.glass(info.glassColor), texture = glass))
                }
            }
        }
        return draws
    }

    /** Wrinkles, make-up and textured beards on the skin colour (BuildManagedFacelineTexture). */
    private fun facelineTexture(resource: FflResource, info: FflCharInfo, resolution: Int): FflTexture? {
        if (info.faceLine == 0 && info.faceMakeup == 0 && info.beardType < 4) return null
        val overlays = ArrayList<Draw>(3)
        if (info.faceMakeup > 0) {
            resource.texture(FflResource.TEXTURE_MAKEUP, info.faceMakeup)?.let {
                overlays += fullScreen(Modulate(1, TYPE_FACELINE, texture = it))
            }
        }
        if (info.faceLine > 0) {
            resource.texture(FflResource.TEXTURE_FACELINE, info.faceLine)?.let {
                overlays += fullScreen(Modulate(3, TYPE_FACELINE, r = BLACK, texture = it))
            }
        }
        if (info.beardType >= 4) {
            resource.texture(FflResource.TEXTURE_BEARD, info.beardType - 3)?.let {
                overlays += fullScreen(Modulate(3, TYPE_FACELINE, r = Colors.hair(info.beardColor), texture = it))
            }
        }
        if (overlays.isEmpty()) return null
        return overlayTexture(overlays, max(1, resolution / 2), max(1, resolution), Colors.faceline(info.facelineColor), Blend.Faceline)
    }

    /** Eyes, eyebrows, mouth, mustache and mole, placed on FFL's 64-unit face grid (BuildManagedMaskTexture). */
    private fun maskTexture(resource: FflResource, info: FflCharInfo, resolution: Int, expression: Int): FflTexture? {
        val element = EXPRESSION_ELEMENTS[expression.coerceIn(0, 18)]
        val eyeIndexR = eyeTexture(info, element[0])
        val eyeIndexL = eyeTexture(info, element[1])
        val mouthIndex = mouthTexture(info, element[2])
        val eyebrowIndex = if (element[3] == 0) info.eyebrowType else element[3]
        val parts = MaskParts(info)
        val overlays = ArrayList<Draw>(8)

        if (info.mustacheType != 0) {
            resource.texture(FflResource.TEXTURE_MUSTACHE, info.mustacheType)?.let {
                val modulate = Modulate(3, TYPE_MASK, r = Colors.hair(info.beardColor), texture = it)
                overlays += maskQuad(parts.mustacheR, modulate)
                overlays += maskQuad(parts.mustacheL, modulate)
            }
        }
        resource.texture(FflResource.TEXTURE_MOUTH, mouthIndex)?.let {
            overlays += maskQuad(
                parts.mouth,
                if (mouthIndex > 36) {
                    Modulate(1, TYPE_MASK, texture = it)
                } else {
                    Modulate(2, TYPE_MASK, r = Colors.mouthR(info.mouthColor), g = Colors.mouthG(info.mouthColor), b = WHITE, texture = it)
                },
            )
        }
        if (eyebrowIndex != 23) {
            resource.texture(FflResource.TEXTURE_EYEBROW, eyebrowIndex)?.let {
                val modulate = Modulate(3, TYPE_MASK, r = Colors.hair(info.eyebrowColor), texture = it)
                overlays += maskQuad(parts.eyebrowR, modulate)
                overlays += maskQuad(parts.eyebrowL, modulate)
            }
        }
        val eyeR = resource.texture(FflResource.TEXTURE_EYE, eyeIndexR)
        val eyeL = resource.texture(FflResource.TEXTURE_EYE, eyeIndexL)
        eyeR?.let { overlays += maskQuad(parts.eyeR, eyeModulate(info, eyeIndexR, it)) }
        eyeL?.let { overlays += maskQuad(parts.eyeL, eyeModulate(info, eyeIndexL, it)) }
        if (info.moleType != 0) {
            resource.texture(FflResource.TEXTURE_MOLE, info.moleType)?.let {
                overlays += maskQuad(parts.mole, Modulate(3, TYPE_MASK, r = MOLE, texture = it))
            }
        }
        if (overlays.isEmpty()) return null
        val size = max(1, resolution)
        return overlayTexture(overlays, size, size, floatArrayOf(0f, 0f, 0f, 0f), Blend.Mask)
    }

    private fun eyeModulate(info: FflCharInfo, index: Int, texture: FflTexture) =
        if (index in DIRECT_EYES) {
            Modulate(1, TYPE_MASK, texture = texture)
        } else {
            Modulate(2, TYPE_MASK, r = floatArrayOf(0f, 1f, 1f, 1f), g = WHITE, b = Colors.eyeB(info.eyeColor), texture = texture)
        }

    private fun eyeTexture(info: FflCharInfo, type: Int) = when (type) {
        1 -> 60
        3 -> 61
        4 -> 26
        5 -> 47
        else -> info.eyeType
    }

    private fun mouthTexture(info: FflCharInfo, type: Int) = when (type) {
        1 -> 10
        2 -> 12
        3 -> 36
        5 -> 19
        else -> info.mouthType
    }

    private fun overlayTexture(overlays: List<Draw>, width: Int, height: Int, clear: FloatArray, blend: Blend): FflTexture {
        val target = Target(width, height, withDepth = false)
        target.fill(toByte(clear[0]), toByte(clear[1]), toByte(clear[2]), toByte(clear[3]))
        drawAll(target, overlays.mapNotNull { prepare(it, width, height, Mat4.IDENTITY, Mat4.IDENTITY, Mat4.IDENTITY) }, light = false, blend)
        return FflTexture(width, height, FflTexture.RGBA8, target.pixels)
    }

    private fun fullScreen(modulate: Modulate): Draw {
        val shape = FflShape(
            positions = floatArrayOf(-1f, 1f, 0f, 1f, 1f, 0f, -1f, -1f, 0f, 1f, -1f, 0f),
            texcoords = floatArrayOf(0f, 0f, 1f, 0f, 0f, 1f, 1f, 1f),
            normals = FloatArray(12) { if (it % 3 == 2) 1f else 0f },
            tangents = FloatArray(12),
            parameters = FloatArray(16) { if (it % 4 == 2) 0f else 1f },
            indices = intArrayOf(0, 1, 2, 2, 1, 3),
            translates = null,
        )
        return draw(shape, 1f, 1f, null, false, CULL_NONE, modulate)
    }

    /** A part's quad on the mask (CreateRawMaskOverlayDrawParam), in the mask's clip space. */
    private fun maskQuad(part: MaskPart, modulate: Modulate): Draw {
        val posXAdd = when (part.origin) {
            Origin.Center -> -0.5f
            Origin.Left -> -1f
            Origin.Right -> 0f
        }
        val tex01 = if (part.origin == Origin.Right) 0f else 1f
        val tex23 = if (part.origin == Origin.Right) 1f else 0f
        val baseX = floatArrayOf(1f, 1f, 0f, 0f)
        val baseY = floatArrayOf(-0.5f, 0.5f, 0.5f, -0.5f)
        val uvY = floatArrayOf(0f, 1f, 1f, 0f)
        val radians = part.rotation * (Math.PI.toFloat() / 180f)
        val cos = cos(radians)
        val sin = sin(radians)
        val positions = FloatArray(12)
        val texcoords = FloatArray(8)
        for (i in 0 until 4) {
            val lx = baseX[i] + posXAdd
            val ly = baseY[i]
            val xr = lx * part.scaleX * cos - ly * part.scaleY * sin
            val yr = lx * part.scaleX * sin + ly * part.scaleY * cos
            val xw = 0.88961464f * xr + part.x
            val yw = 0.9276675f * yr + part.y
            positions[i * 3] = xw * (2f / 64f) - 1f
            positions[i * 3 + 1] = 1f - yw * (2f / 64f)
            texcoords[i * 2] = if (i < 2) tex01 else tex23
            texcoords[i * 2 + 1] = uvY[i]
        }
        val shape = FflShape(
            positions, texcoords,
            normals = FloatArray(12) { if (it % 3 == 2) 1f else 0f },
            tangents = FloatArray(12),
            parameters = FloatArray(16) { if (it % 4 == 2) 0f else 1f },
            indices = intArrayOf(2, 1, 3, 1, 3, 0),
            translates = null,
        )
        return draw(shape, 1f, 1f, null, false, CULL_NONE, modulate)
    }

    /**
     * A shape placed on the head (BuildManagedShapeDrawParam). The PC passes normals, tangents and
     * vertex parameters through FFL's packed vertex formats, so they are quantised the same way.
     */
    private fun draw(shape: FflShape, scaleX: Float, scaleY: Float, translate: FloatArray?, flipX: Boolean, cull: Int, modulate: Modulate): Draw {
        val count = shape.vertexCount
        if (count == 0 || shape.indices.size < 3) throw IOException("Shape has no drawable geometry.")
        val scaleZ = (scaleX + scaleY) * 0.5f
        val tx = translate?.get(0) ?: 0f
        val ty = translate?.get(1) ?: 0f
        val tz = translate?.get(2) ?: 0f
        val positions = FloatArray(count * 3)
        val normals = FloatArray(count * 3)
        val tangents = FloatArray(count * 3)
        val parameters = FloatArray(count * 4)
        for (i in 0 until count) {
            val x = if (flipX) -shape.positions[i * 3] else shape.positions[i * 3]
            positions[i * 3] = x * scaleX + tx
            positions[i * 3 + 1] = shape.positions[i * 3 + 1] * scaleY + ty
            positions[i * 3 + 2] = shape.positions[i * 3 + 2] * scaleZ + tz

            val nx = if (flipX) -shape.normals[i * 3] else shape.normals[i * 3]
            val packed = pack10(nx) or (pack10(shape.normals[i * 3 + 1]) shl 10) or (pack10(shape.normals[i * 3 + 2]) shl 20)
            FflResource.decodeInt2101010(packed, normals, i * 3)

            val tangentX = if (flipX) -shape.tangents[i * 3] else shape.tangents[i * 3]
            FflResource.normalizeInto(
                pack8(tangentX) / 127f, pack8(shape.tangents[i * 3 + 1]) / 127f, pack8(shape.tangents[i * 3 + 2]) / 127f,
                tangents, i * 3, 1e-8f, 0f,
            )
            for (c in 0 until 4) parameters[i * 4 + c] = toByte(shape.parameters[i * 4 + c]) / 255f
        }
        return Draw(positions, shape.texcoords, normals, tangents, parameters, shape.indices, cull, modulate, MATERIALS[modulate.type])
    }

    private fun pack10(value: Float): Int = round(value.coerceIn(-1f, 1f) * 511f).toInt().coerceIn(-512, 511) and 0x3FF

    private fun pack8(value: Float): Int = round(value.coerceIn(-1f, 1f) * 127f).toInt().coerceIn(-127, 127)

    // --- Rasterising (PrepareMesh, RasterizeTriangle, EvaluateModulateColor, BlendPixel) ---

    private fun prepare(draw: Draw, width: Int, height: Int, model: Mat4, view: Mat4, projection: Mat4): Prepared? {
        val count = draw.positions.size / 3
        val modelView = model * view
        val out = FloatArray(count * STRIDE)
        val world = FloatArray(3)
        val eye = FloatArray(3)
        val clip = FloatArray(4)
        for (i in 0 until count) {
            model.transformPoint(draw.positions, i * 3, world, 0)
            view.transformPoint(world, 0, eye, 0)
            projection.transform4(eye, clip)
            if (abs(clip[3]) <= 1e-6f) return null
            val invW = 1f / clip[3]
            val ndcX = clip[0] * invW
            val ndcY = clip[1] * invW
            val ndcZ = clip[2] * invW
            val o = i * STRIDE
            out[o] = (ndcX * 0.5f + 0.5f) * width
            out[o + 1] = (1f - (ndcY * 0.5f + 0.5f)) * height
            out[o + 2] = ndcZ * 0.5f + 0.5f
            out[o + 3] = invW
            out[o + 4] = draw.texcoords.getOrElse(i * 2) { 0f }
            out[o + 5] = draw.texcoords.getOrElse(i * 2 + 1) { 0f }
            out[o + 6] = eye[0]
            out[o + 7] = eye[1]
            out[o + 8] = eye[2]
            modelView.transformNormal(draw.normals, i * 3, out, o + 9)
            modelView.transformNormal(draw.tangents, i * 3, out, o + 12)
            for (c in 0 until 4) out[o + 15 + c] = draw.parameters[i * 4 + c]
        }
        return Prepared(draw, out)
    }

    /** Draws [mesh]'s triangles into rows [rowStart] to [rowEnd] of [target]. */
    private fun rasterize(target: Target, mesh: Prepared, light: Boolean, blend: Blend, rowStart: Int, rowEnd: Int) {
        val indices = mesh.draw.indices
        val at = FloatArray(STRIDE)
        val color = FloatArray(4)
        val scratch = Scratch()
        var i = 0
        while (i + 2 < indices.size) {
            triangle(target, mesh, light, blend, indices[i], indices[i + 1], indices[i + 2], at, color, scratch, rowStart, rowEnd)
            i += 3
        }
    }

    private fun triangle(
        target: Target,
        mesh: Prepared,
        light: Boolean,
        blend: Blend,
        ia: Int,
        ib: Int,
        ic: Int,
        at: FloatArray,
        color: FloatArray,
        scratch: Scratch,
        rowStart: Int,
        rowEnd: Int,
    ) {
        val v = mesh.vertices
        val count = v.size / STRIDE
        if (ia !in 0 until count || ib !in 0 until count || ic !in 0 until count) return
        val a = ia * STRIDE
        val b = ib * STRIDE
        val c = ic * STRIDE
        val ax = v[a]
        val ay = v[a + 1]
        val bx = v[b]
        val by = v[b + 1]
        val cx = v[c]
        val cy = v[c + 1]
        val area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if (abs(area) < 1e-6f) return
        val cull = mesh.draw.cull
        if (cull == CULL_BACK && area >= 0f) return
        if (cull == CULL_FRONT && area <= 0f) return

        val width = target.width
        val height = target.height
        val minX = floor(min(ax, min(bx, cx))).toInt().coerceIn(0, width - 1)
        val maxX = ceil(max(ax, max(bx, cx))).toInt().coerceIn(0, width - 1)
        val minY = floor(min(ay, min(by, cy))).toInt().coerceIn(0, height - 1)
        val maxY = ceil(max(ay, max(by, cy))).toInt().coerceIn(0, height - 1)
        val firstRow = max(minY, rowStart)
        val lastRow = min(maxY, rowEnd - 1)
        if (firstRow > lastRow) return

        val invArea = 1f / area
        val sampleX = minX + 0.5f
        val sampleY = minY + 0.5f
        val e0x = by - cy
        val e0y = cx - bx
        val e0c = bx * cy - by * cx
        val e1x = cy - ay
        val e1y = ax - cx
        val e1c = cx * ay - cy * ax
        val e2x = ay - by
        val e2y = bx - ax
        val e2c = ax * by - ay * bx
        var e0Row = e0x * sampleX + e0y * sampleY + e0c
        var e1Row = e1x * sampleX + e1y * sampleY + e1c
        var e2Row = e2x * sampleX + e2y * sampleY + e2c
        // Rows before the band are stepped as the PC steps them, so every row's edges round the same.
        for (skipped in minY until firstRow) {
            e0Row += e0y
            e1Row += e1y
            e2Row += e2y
        }
        val aw = v[a + 3]
        val bw = v[b + 3]
        val cw = v[c + 3]
        val depth = target.depth

        for (y in firstRow..lastRow) {
            var e0 = e0Row
            var e1 = e1Row
            var e2 = e2Row
            var pixel = y * width + minX
            for (x in minX..maxX) {
                val negative = e0 < 0f || e1 < 0f || e2 < 0f
                val positive = e0 > 0f || e1 > 0f || e2 > 0f
                if (!(negative && positive)) {
                    val w0 = e0 * invArea
                    val w1 = e1 * invArea
                    val w2 = e2 * invArea
                    val denominator = w0 * aw + w1 * bw + w2 * cw
                    if (abs(denominator) >= 1e-8f) {
                        val z = (w0 * v[a + 2] * aw + w1 * v[b + 2] * bw + w2 * v[c + 2] * cw) / denominator
                        if (z in 0f..1f && (depth == null || z <= depth[pixel])) {
                            at[4] = (w0 * v[a + 4] * aw + w1 * v[b + 4] * bw + w2 * v[c + 4] * cw) / denominator
                            at[5] = (w0 * v[a + 5] * aw + w1 * v[b + 5] * bw + w2 * v[c + 5] * cw) / denominator
                            // The rest only matters for a texel that shows.
                            if (modulate(mesh.draw.modulate, at[4], at[5], color, scratch)) {
                                if (light) {
                                    for (k in 6 until STRIDE) {
                                        at[k] = (w0 * v[a + k] * aw + w1 * v[b + k] * bw + w2 * v[c + k] * cw) / denominator
                                    }
                                    light(mesh.draw.material, at, color, scratch)
                                } else {
                                    clampColor(color)
                                }
                            }
                            if (color[3] > 0f) {
                                target.blend(pixel * 4, color, blend)
                                if (depth != null) depth[pixel] = z
                            }
                        }
                    }
                }
                e0 += e0x
                e1 += e1x
                e2 += e2x
                pixel++
            }
            e0Row += e0y
            e1Row += e1y
            e2Row += e2y
        }
    }

    /**
     * The texel's colour before lighting (EvaluateModulateColor's modulate modes), not yet clamped.
     * False when it is fully transparent, which draws nothing.
     */
    private fun modulate(modulate: Modulate, u: Float, v: Float, out: FloatArray, scratch: Scratch): Boolean {
        val texel = scratch.texel
        sample(modulate.texture, u, v, texel, scratch.corners)
        val r = modulate.r
        val g = modulate.g
        val b = modulate.b
        when (modulate.mode) {
            0 -> { out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = 1f }
            1 -> { out[0] = texel[0]; out[1] = texel[1]; out[2] = texel[2]; out[3] = texel[3] }
            2 -> {
                out[0] = texel[0] * r[0] + texel[1] * g[0] + texel[2] * b[0]
                out[1] = texel[0] * r[1] + texel[1] * g[1] + texel[2] * b[1]
                out[2] = texel[0] * r[2] + texel[1] * g[2] + texel[2] * b[2]
                out[3] = texel[3]
            }
            3 -> { out[0] = r[0]; out[1] = r[1]; out[2] = r[2]; out[3] = texel[0] }
            4 -> { out[0] = texel[1] * r[0]; out[1] = texel[1] * r[1]; out[2] = texel[1] * r[2]; out[3] = texel[0] }
            5 -> { out[0] = texel[0] * r[0]; out[1] = texel[0] * r[1]; out[2] = texel[0] * r[2]; out[3] = 1f }
            else -> out.fill(1f)
        }
        if (modulate.mode != 0 && out[3] <= 0f) {
            out.fill(0f)
            return false
        }
        return true
    }

    /**
     * Lights the modulated colour in [color] (EvaluateModulateColor's lit path). Specular maths is
     * skipped for materials without specular, where the PC multiplies it by zero.
     */
    private fun light(material: Material, at: FloatArray, color: FloatArray, scratch: Scratch) {
        val baseR = color[0]
        val baseG = color[1]
        val baseB = color[2]
        val n = scratch.normal
        FflResource.normalizeInto(at[9], at[10], at[11], n, 0, 1e-8f, 1f)
        val diffuseDot = max(LIGHT[0] * n[0] + LIGHT[1] * n[1] + LIGHT[2] * n[2], PROFILE_DIFFUSE_FLOOR)
        val diffuseFactor = 1f + (diffuseDot - 1f) * PROFILE_DIRECTIONAL
        var reflection = 0f
        var strength = 0f
        if (material.hasSpecular) {
            val e = scratch.eye
            FflResource.normalizeInto(-at[6], -at[7], -at[8], e, 0, 1e-8f, 1f)
            // Reflect(-light, n) = -light - 2 * dot(-light, n) * n.
            val dotReflect = -LIGHT[0] * n[0] + -LIGHT[1] * n[1] + -LIGHT[2] * n[2]
            val reflectX = -LIGHT[0] - 2f * dotReflect * n[0]
            val reflectY = -LIGHT[1] - 2f * dotReflect * n[1]
            val reflectZ = -LIGHT[2] - 2f * dotReflect * n[2]
            val blinn = power(max(reflectX * e[0] + reflectY * e[1] + reflectZ * e[2], 0f), material.specularPower)
            if (material.anisotropic) {
                val t = scratch.tangent
                val tangentLength = at[12] * at[12] + at[13] * at[13] + at[14] * at[14]
                if (tangentLength < 1e-8f) {
                    t[0] = 1f
                    t[1] = 0f
                    t[2] = 0f
                } else {
                    FflResource.normalizeInto(at[12], at[13], at[14], t, 0, 1e-8f, 0f)
                }
                val dotLt = LIGHT[0] * t[0] + LIGHT[1] * t[1] + LIGHT[2] * t[2]
                val dotVt = e[0] * t[0] + e[1] * t[1] + e[2] * t[2]
                val dotLn = sqrt(max(0f, 1f - dotLt * dotLt))
                val dotVr = dotLn * sqrt(max(0f, 1f - dotVt * dotVt)) - dotLt * dotVt
                val anisotropic = power(max(0f, dotVr), material.specularPower)
                reflection = anisotropic + (blinn - anisotropic) * at[15]
                strength = at[16]
            } else {
                reflection = blinn
                strength = 1f
            }
        }
        val rimFactor = power(max(0f, at[18] * (1f - abs(n[2]))), PROFILE_RIM_POWER)
        val rim = rimFactor * PROFILE_RIM_SCALE
        val diffuseScale = diffuseFactor * PROFILE_DIFFUSE_SCALE
        for (channel in 0 until 3) {
            val ambient = material.ambient[channel] * PROFILE_AMBIENT
            val diffuse = material.diffuse[channel] * diffuseScale
            val specular = material.specular[channel] * reflection * strength * PROFILE_SPECULAR
            val base = when (channel) {
                0 -> baseR
                1 -> baseG
                else -> baseB
            }
            color[channel] = clamp01((ambient + diffuse) * base + specular + material.rim[channel] * rim)
        }
        color[3] = clamp01(color[3])
    }

    /** The unlit colour, clamped as the PC returns it. */
    private fun clampColor(color: FloatArray) {
        for (c in 0 until 4) color[c] = clamp01(color[c])
    }

    /** Float pow as the PC's MathF.Pow gives it; 0 to a positive power is 0 without the call. */
    private fun power(base: Float, exponent: Float): Float = if (base == 0f) 0f else base.pow(exponent)

    /** Bilinear with mirrored repeat, texel centres on the edges (SampleTexture). */
    private fun sample(texture: FflTexture?, u: Float, v: Float, out: FloatArray, corners: FloatArray) {
        if (texture == null) {
            out.fill(1f)
            return
        }
        if (texture.width <= 1 || texture.height <= 1) {
            texel(texture, 0, 0, out, 0)
            return
        }
        val fx = mirror(u) * (texture.width - 1)
        val fy = mirror(v) * (texture.height - 1)
        val x0 = floor(fx).toInt().coerceIn(0, texture.width - 1)
        val y0 = floor(fy).toInt().coerceIn(0, texture.height - 1)
        val x1 = min(x0 + 1, texture.width - 1)
        val y1 = min(y0 + 1, texture.height - 1)
        val tx = fx - x0
        val ty = fy - y0
        texel(texture, x0, y0, corners, 0)
        texel(texture, x1, y0, corners, 4)
        texel(texture, x0, y1, corners, 8)
        texel(texture, x1, y1, corners, 12)
        for (c in 0 until 4) {
            val top = lerp(corners[c], corners[4 + c], tx)
            val bottom = lerp(corners[8 + c], corners[12 + c], tx)
            out[c] = lerp(top, bottom, ty)
        }
    }

    private fun lerp(a: Float, b: Float, t: Float) = a * (1f - t) + b * t

    private fun mirror(value: Float): Float {
        var wrapped = value % 2f
        if (wrapped < 0f) wrapped += 2f
        return if (wrapped <= 1f) wrapped else 2f - wrapped
    }

    private fun texel(texture: FflTexture, x: Int, y: Int, out: FloatArray, at: Int) {
        val index = (y * texture.width + x) * texture.stride
        val p = texture.pixels
        if (index < 0 || index + texture.stride > p.size) {
            for (c in 0 until 4) out[at + c] = 1f
            return
        }
        when (texture.format) {
            FflTexture.R8 -> {
                val value = (p[index].toInt() and 0xFF) / 255f
                out[at] = value
                out[at + 1] = value
                out[at + 2] = value
                out[at + 3] = 1f
            }
            FflTexture.RG8 -> {
                out[at] = (p[index].toInt() and 0xFF) / 255f
                out[at + 1] = (p[index + 1].toInt() and 0xFF) / 255f
                out[at + 2] = 0f
                out[at + 3] = 1f
            }
            else -> for (c in 0 until 4) out[at + c] = (p[index + c].toInt() and 0xFF) / 255f
        }
    }

    private fun clamp01(value: Float) = value.coerceIn(0f, 1f)

    private fun toByte(value: Float): Int = round(clamp01(value) * 255f).toInt().coerceIn(0, 255)

    // --- Types and tables ---

    private enum class Blend { Over, Faceline, Mask }

    /** Per-thread working arrays, so shading a pixel allocates nothing. */
    private class Scratch {
        val texel = FloatArray(4)
        val corners = FloatArray(16)
        val normal = FloatArray(3)
        val eye = FloatArray(3)
        val tangent = FloatArray(3)
    }

    /** An RGBA8 picture being drawn, with a depth buffer for the head. */
    private class Target(val width: Int, val height: Int, withDepth: Boolean) {
        val pixels = ByteArray(width * height * 4)
        val depth: FloatArray? = if (withDepth) FloatArray(width * height) { 1f } else null

        fun fill(r: Int, g: Int, b: Int, a: Int) {
            for (i in 0 until width * height) {
                pixels[i * 4] = r.toByte()
                pixels[i * 4 + 1] = g.toByte()
                pixels[i * 4 + 2] = b.toByte()
                pixels[i * 4 + 3] = a.toByte()
            }
        }

        fun blend(i: Int, src: FloatArray, mode: Blend) {
            val srcA = clamp01(src[3])
            if (srcA <= 0f) return
            if (mode == Blend.Over && srcA >= 0.999f) {
                pixels[i] = toByte(src[0]).toByte()
                pixels[i + 1] = toByte(src[1]).toByte()
                pixels[i + 2] = toByte(src[2]).toByte()
                pixels[i + 3] = 255.toByte()
                return
            }
            val dstR = (pixels[i].toInt() and 0xFF) / 255f
            val dstG = (pixels[i + 1].toInt() and 0xFF) / 255f
            val dstB = (pixels[i + 2].toInt() and 0xFF) / 255f
            val dstA = (pixels[i + 3].toInt() and 0xFF) / 255f
            val outR: Float
            val outG: Float
            val outB: Float
            val outA: Float
            when (mode) {
                Blend.Faceline -> {
                    outR = src[0] * srcA + dstR * (1f - srcA)
                    outG = src[1] * srcA + dstG * (1f - srcA)
                    outB = src[2] * srcA + dstB * (1f - srcA)
                    outA = srcA + dstA
                }
                Blend.Mask -> {
                    outR = src[0] * (1f - dstA) + dstR * dstA
                    outG = src[1] * (1f - dstA) + dstG * dstA
                    outB = src[2] * (1f - dstA) + dstB * dstA
                    outA = srcA * srcA + dstA * dstA
                }
                Blend.Over -> {
                    outA = srcA + dstA * (1f - srcA)
                    if (outA <= 0f) return
                    outR = (src[0] * srcA + dstR * dstA * (1f - srcA)) / outA
                    outG = (src[1] * srcA + dstG * dstA * (1f - srcA)) / outA
                    outB = (src[2] * srcA + dstB * dstA * (1f - srcA)) / outA
                }
            }
            pixels[i] = toByte(outR).toByte()
            pixels[i + 1] = toByte(outG).toByte()
            pixels[i + 2] = toByte(outB).toByte()
            pixels[i + 3] = toByte(outA).toByte()
        }

        fun argb(): IntArray = IntArray(width * height) { i ->
            ((pixels[i * 4 + 3].toInt() and 0xFF) shl 24) or ((pixels[i * 4].toInt() and 0xFF) shl 16) or
                ((pixels[i * 4 + 1].toInt() and 0xFF) shl 8) or (pixels[i * 4 + 2].toInt() and 0xFF)
        }
    }

    private class Modulate(
        val mode: Int,
        val type: Int,
        r: FloatArray? = null,
        g: FloatArray? = null,
        b: FloatArray? = null,
        val texture: FflTexture? = null,
    ) {
        // A colour the PC leaves out reads as white.
        val r = r?.map(::clamp01)?.toFloatArray() ?: WHITE
        val g = g?.map(::clamp01)?.toFloatArray() ?: WHITE
        val b = b?.map(::clamp01)?.toFloatArray() ?: WHITE
    }

    private class Material(
        val ambient: FloatArray,
        val diffuse: FloatArray,
        val specular: FloatArray,
        val specularPower: Float,
        val anisotropic: Boolean,
        val rim: FloatArray,
    ) {
        val hasSpecular = specular.any { it != 0f }
    }

    private class Draw(
        val positions: FloatArray,
        val texcoords: FloatArray,
        val normals: FloatArray,
        val tangents: FloatArray,
        val parameters: FloatArray,
        val indices: IntArray,
        val cull: Int,
        val modulate: Modulate,
        val material: Material,
    )

    /** Per vertex: screen x, y, depth, 1/w, u, v, view position, normal, tangent and the four parameters. */
    private class Prepared(val draw: Draw, val vertices: FloatArray)

    /** A Mii's body (BodyRenderData): its meshes, their scale, and where the head sits on them. */
    private class Body(val draws: List<Draw>, val scale: Mat4, val headTranslation: FloatArray)

    private enum class Origin { Center, Left, Right }

    private class MaskPart(val x: Float, val y: Float, val scaleX: Float, val scaleY: Float, val rotation: Float, val origin: Origin)

    /** Where each face part sits on the mask (BuildRawMaskParts). */
    private class MaskParts(info: FflCharInfo) {
        val eyeR: MaskPart
        val eyeL: MaskPart
        val eyebrowR: MaskPart
        val eyebrowL: MaskPart
        val mouth: MaskPart
        val mustacheR: MaskPart
        val mustacheL: MaskPart
        val mole: MaskPart

        init {
            val posXAdd = 3.5323312f
            val posYAdd = 4.629278f
            val spacingMul = 0.88961464f
            val posXMul = 1.7792293f
            val posYMul = 1.0760943f
            val posYAddEye = posYAdd + 13.822246f
            val posYAddEyebrow = posYAdd + 11.920528f
            val posYAddMouth = posYAdd + 24.629572f
            val posYAddMustache = posYAdd + 27.134275f
            val posXAddMole = posXAdd + 14.233834f
            val posYAddMole = posYAdd + 11.178394f + 2f * posYMul

            val eyeSpacing = info.eyeSpacingX * spacingMul
            val eyeBase = 0.4f * info.eyeScale + 1f
            val eyeBaseY = 0.12f * info.eyeScaleY + 0.64f
            val eyeScaleX = 5.34375f * eyeBase
            val eyeScaleY = 4.5f * eyeBase * eyeBaseY
            val eyeY = info.eyePositionY * posYMul + posYAddEye
            val eyeRotate = ((info.eyeRotate + 32 - EYE_ROTATE[info.eyeType.coerceIn(0, EYE_ROTATE.size - 1)]) % 32) * (360f / 32f)

            val browSpacing = info.eyebrowSpacingX * spacingMul
            val browBase = 0.4f * info.eyebrowScale + 1f
            val browBaseY = 0.12f * info.eyebrowScaleY + 0.64f
            val browScaleX = 5.0625f * browBase
            val browScaleY = 4.5f * browBase * browBaseY
            val browY = info.eyebrowPositionY * posYMul + posYAddEyebrow
            val browRotate =
                ((info.eyebrowRotate + 32 - EYEBROW_ROTATE[info.eyebrowType.coerceIn(0, EYEBROW_ROTATE.size - 1)]) % 32) * (360f / 32f)

            val mouthBase = 0.4f * info.mouthScale + 1f
            val mouthBaseY = 0.12f * info.mouthScaleY + 0.64f
            val mustacheBase = 0.4f * info.mustacheScale + 1f
            val mustacheY = info.mustachePositionY * posYMul + posYAddMustache
            val moleScale = 0.4f * info.moleScale + 1f

            eyeR = MaskPart(32 - eyeSpacing, eyeY, eyeScaleX, eyeScaleY, eyeRotate, Origin.Left)
            eyeL = MaskPart(eyeSpacing + 32, eyeY, eyeScaleX, eyeScaleY, 360f - eyeRotate, Origin.Right)
            eyebrowR = MaskPart(32 - browSpacing, browY, browScaleX, browScaleY, browRotate, Origin.Left)
            eyebrowL = MaskPart(browSpacing + 32, browY, browScaleX, browScaleY, 360f - browRotate, Origin.Right)
            mouth = MaskPart(32f, info.mouthPositionY * posYMul + posYAddMouth, 6.1875f * mouthBase, 4.5f * mouthBase * mouthBaseY, 0f, Origin.Center)
            mustacheR = MaskPart(32f, mustacheY, 4.5f * mustacheBase, 9.0f * mustacheBase, 0f, Origin.Left)
            mustacheL = MaskPart(32f, mustacheY, 4.5f * mustacheBase, 9.0f * mustacheBase, 0f, Origin.Right)
            mole = MaskPart(
                info.molePositionX * posXMul + posXAddMole, info.molePositionY * posYMul + posYAddMole,
                moleScale, moleScale, 0f, Origin.Center,
            )
        }
    }

    private const val STRIDE = 19
    private const val CULL_NONE = 0
    private const val CULL_BACK = 1
    private const val CULL_FRONT = 2
    private const val TYPE_FACELINE = 0
    private const val TYPE_BEARD = 1
    private const val TYPE_NOSE = 2
    private const val TYPE_FOREHEAD = 3
    private const val TYPE_HAIR = 4
    private const val TYPE_CAP = 5
    private const val TYPE_MASK = 6
    private const val TYPE_NOSELINE = 7
    private const val TYPE_GLASS = 8
    private const val TYPE_BODY = 9
    private const val TYPE_PANTS = 10

    // MiiLightingProfiles.Default on the PC.
    private const val PROFILE_AMBIENT = 0.71f
    private const val PROFILE_DIRECTIONAL = 0.32f
    private const val PROFILE_DIFFUSE_SCALE = 1.32f
    private const val PROFILE_DIFFUSE_FLOOR = 0.23f
    private const val PROFILE_SPECULAR = 0.78f
    private const val PROFILE_RIM_SCALE = 0.88f
    private const val PROFILE_RIM_POWER = 2.56f

    private val WHITE = floatArrayOf(1f, 1f, 1f, 1f)
    private val BLACK = floatArrayOf(0f, 0f, 0f, 1f)
    private val MOLE = floatArrayOf(0.071f, 0.059f, 0.059f, 1f)
    /** The PC's trousers: the grey of an ordinary Mii's. */
    private val PANTS = floatArrayOf(0.2509804f, 0.2745099f, 0.30588239f, 1f)
    private val LIGHT = FloatArray(3).also { FflResource.normalizeInto(-0.4531539381f, 0.4226179123f, 0.7848858833f, it, 0, 0f, 1f) }
    private val NO_NOSE_EXPRESSIONS = setOf(49, 50, 51, 52, 61, 62)
    private val DIRECT_EYES = setOf(60, 62, 65, 69, 70, 71, 72, 73, 74, 75, 78, 79)

    /** Right eye, left eye, mouth and eyebrow per expression; 0 keeps the Mii's own part. */
    private val EXPRESSION_ELEMENTS = arrayOf(
        intArrayOf(0, 0, 0, 0), intArrayOf(1, 1, 0, 0), intArrayOf(0, 0, 1, 0), intArrayOf(2, 2, 2, 0),
        intArrayOf(3, 3, 0, 0), intArrayOf(4, 4, 0, 0), intArrayOf(0, 0, 3, 0), intArrayOf(1, 1, 3, 0),
        intArrayOf(0, 0, 3, 0), intArrayOf(2, 2, 3, 0), intArrayOf(3, 3, 3, 0), intArrayOf(4, 4, 3, 0),
        intArrayOf(5, 0, 0, 0), intArrayOf(0, 5, 0, 0), intArrayOf(5, 0, 3, 0), intArrayOf(0, 5, 3, 0),
        intArrayOf(5, 0, 5, 0), intArrayOf(0, 5, 5, 0), intArrayOf(5, 5, 2, 0),
    )

    private val EYE_ROTATE = intArrayOf(
        3, 4, 4, 4, 3, 4, 4, 4, 3, 4, 4, 4, 4, 3, 3, 4, 4, 4, 3, 3, 4, 3, 4, 3, 3, 4, 3, 4, 4, 3, 4, 4, 4, 3, 3, 3, 4, 4, 3, 3,
        3, 4, 4, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 3, 4, 4, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    )
    private val EYEBROW_ROTATE = intArrayOf(6, 6, 5, 7, 6, 7, 6, 7, 4, 7, 6, 8, 5, 5, 6, 6, 7, 7, 6, 6, 5, 6, 7, 5, 6, 6, 6, 6)

    private val MATERIALS = arrayOf(
        material(floatArrayOf(0.85f, 0.75f, 0.75f), 0.75f, 0.30f, 1.2f, false, 0.3f),
        material(floatArrayOf(1.0f, 1.0f, 1.0f), 0.7f, 0.0f, 40.0f, false, 0.3f),
        material(floatArrayOf(0.90f, 0.85f, 0.85f), 0.75f, 0.22f, 1.5f, false, 0.3f),
        material(floatArrayOf(0.85f, 0.75f, 0.75f), 0.75f, 0.30f, 1.2f, false, 0.3f),
        material(floatArrayOf(1.00f, 1.00f, 1.00f), 0.70f, 0.10f, 10.0f, true, 0.3f),
        material(floatArrayOf(0.75f, 0.75f, 0.75f), 0.72f, 0.30f, 1.5f, false, 0.3f),
        material(floatArrayOf(1.0f, 1.0f, 1.0f), 0.7f, 0.0f, 40.0f, true, 0.3f),
        material(floatArrayOf(1.0f, 1.0f, 1.0f), 0.7f, 0.0f, 40.0f, true, 0.3f),
        material(floatArrayOf(1.0f, 1.0f, 1.0f), 0.7f, 0.0f, 40.0f, true, 0.3f),
        // The body and the trousers.
        material(floatArrayOf(0.95622f, 0.95622f, 0.95622f), 0.496733f, 0.2409f, 3.0f, false, 0.4f),
        material(floatArrayOf(0.95622f, 0.95622f, 0.95622f), 1.084967f, 0.2409f, 3.0f, false, 0.4f),
    )

    /** The PC multiplies its light colours into each material per pixel; the products are the same. */
    private fun material(ambient: FloatArray, diffuse: Float, specular: Float, power: Float, anisotropic: Boolean, rim: Float) = Material(
        ambient = FloatArray(3) { 0.73f * ambient[it] },
        diffuse = FloatArray(3) { 0.60f * diffuse },
        specular = FloatArray(3) { 0.70f * specular },
        specularPower = power,
        anisotropic = anisotropic,
        rim = FloatArray(3) { rim },
    )
}

/** A 4x4 matrix in System.Numerics' layout: row vectors, translation in the last row. */
internal class Mat4(val m: FloatArray) {
    operator fun times(other: Mat4): Mat4 {
        val o = other.m
        val result = FloatArray(16)
        for (row in 0 until 4) {
            for (column in 0 until 4) {
                result[row * 4 + column] = m[row * 4] * o[column] + m[row * 4 + 1] * o[4 + column] +
                    m[row * 4 + 2] * o[8 + column] + m[row * 4 + 3] * o[12 + column]
            }
        }
        return Mat4(result)
    }

    fun transformPoint(v: FloatArray, at: Int, out: FloatArray, to: Int) {
        val x = v[at]
        val y = v[at + 1]
        val z = v[at + 2]
        for (c in 0 until 3) out[to + c] = x * m[c] + y * m[4 + c] + z * m[8 + c] + m[12 + c]
    }

    fun transformNormal(v: FloatArray, at: Int, out: FloatArray, to: Int) {
        val x = v[at]
        val y = v[at + 1]
        val z = v[at + 2]
        for (c in 0 until 3) out[to + c] = x * m[c] + y * m[4 + c] + z * m[8 + c]
    }

    /** (x, y, z, 1) times this matrix. */
    fun transform4(v: FloatArray, out: FloatArray) {
        for (c in 0 until 4) out[c] = v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c]
    }

    companion object {
        val IDENTITY = Mat4(floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f))

        /** Matrix4x4.CreateScale. */
        fun scale(scale: FloatArray) = Mat4(floatArrayOf(scale[0], 0f, 0f, 0f, 0f, scale[1], 0f, 0f, 0f, 0f, scale[2], 0f, 0f, 0f, 0f, 1f))

        /** Matrix4x4.CreateTranslation. */
        fun translation(offset: FloatArray) = Mat4(floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f, offset[0], offset[1], offset[2], 1f))

        /** CreateRotationMatrix on the PC: CreateRotationX, then Y, then Z, each by [radians]'s angle. */
        fun rotation(radians: FloatArray): Mat4 {
            val (x, y, z) = radians.map { cos(it) to sin(it) }
            val rotateX = Mat4(floatArrayOf(1f, 0f, 0f, 0f, 0f, x.first, x.second, 0f, 0f, -x.second, x.first, 0f, 0f, 0f, 0f, 1f))
            val rotateY = Mat4(floatArrayOf(y.first, 0f, -y.second, 0f, 0f, 1f, 0f, 0f, y.second, 0f, y.first, 0f, 0f, 0f, 0f, 1f))
            val rotateZ = Mat4(floatArrayOf(z.first, z.second, 0f, 0f, -z.second, z.first, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f))
            return rotateX * rotateY * rotateZ
        }

        /** Matrix4x4.CreateLookAt (right-handed). */
        fun lookAt(position: FloatArray, target: FloatArray, up: FloatArray): Mat4 {
            val axisZ = FloatArray(3)
            FflResource.normalizeInto(position[0] - target[0], position[1] - target[1], position[2] - target[2], axisZ, 0, 0f, 1f)
            val crossX = up[1] * axisZ[2] - up[2] * axisZ[1]
            val crossY = up[2] * axisZ[0] - up[0] * axisZ[2]
            val crossZ = up[0] * axisZ[1] - up[1] * axisZ[0]
            val axisX = FloatArray(3)
            FflResource.normalizeInto(crossX, crossY, crossZ, axisX, 0, 0f, 1f)
            val axisY = floatArrayOf(
                axisZ[1] * axisX[2] - axisZ[2] * axisX[1],
                axisZ[2] * axisX[0] - axisZ[0] * axisX[2],
                axisZ[0] * axisX[1] - axisZ[1] * axisX[0],
            )
            fun dot(axis: FloatArray) = axis[0] * -position[0] + axis[1] * -position[1] + axis[2] * -position[2]
            return Mat4(
                floatArrayOf(
                    axisX[0], axisY[0], axisZ[0], 0f,
                    axisX[1], axisY[1], axisZ[1], 0f,
                    axisX[2], axisY[2], axisZ[2], 0f,
                    dot(axisX), dot(axisY), dot(axisZ), 1f,
                ),
            )
        }

        /** Matrix4x4.CreatePerspectiveFieldOfView (right-handed, depth 0 to 1). */
        fun perspective(fieldOfView: Float, aspect: Float, near: Float, far: Float): Mat4 {
            val height = 1f / tan(fieldOfView * 0.5f)
            val width = height / aspect
            val range = far / (near - far)
            return Mat4(
                floatArrayOf(
                    width, 0f, 0f, 0f,
                    0f, height, 0f, 0f,
                    0f, 0f, range, -1f,
                    0f, 0f, range * near, 0f,
                ),
            )
        }
    }
}

/** FFL's colour tables, as the PC resolves them (sRGB). */
internal object Colors {
    fun faceline(index: Int) = FACELINE[index.coerceIn(0, FACELINE.size - 1)]
    fun favorite(index: Int) = FAVORITE[index.coerceIn(0, FAVORITE.size - 1)]
    fun hair(encoded: Int) = common(encoded, COMMON) ?: palette(encoded, HAIR)
    fun glass(encoded: Int) = common(encoded, COMMON) ?: palette(encoded, GLASS)
    fun eyeB(encoded: Int) = common(encoded, COMMON) ?: palette(encoded, EYE)
    fun mouthR(encoded: Int) = common(encoded, COMMON) ?: palette(encoded, MOUTH_R)
    fun mouthG(encoded: Int) = common(encoded, UPPER_LIP) ?: palette(encoded, MOUTH_G)

    private fun common(encoded: Int, table: Array<FloatArray>): FloatArray? {
        if (encoded and FflCharInfo.COMMON_COLOR == 0) return null
        val index = encoded and 0xFF
        return if (index < table.size) table[index] else table[0]
    }

    private fun palette(encoded: Int, table: Array<FloatArray>): FloatArray {
        val index = if (encoded and FflCharInfo.COMMON_COLOR != 0) encoded and 0xFF else encoded
        return table[index.coerceIn(0, table.size - 1)]
    }

    private fun rgb(r: Float, g: Float, b: Float) = floatArrayOf(r, g, b, 1f)

    private val FACELINE = arrayOf(
        rgb(1.000f, 0.827f, 0.678f), rgb(1.000f, 0.714f, 0.420f), rgb(0.870f, 0.475f, 0.259f),
        rgb(1.000f, 0.667f, 0.549f), rgb(0.678f, 0.318f, 0.161f), rgb(0.388f, 0.173f, 0.094f),
    )
    private val COMMON = arrayOf(
        rgb(0.1764706f, 0.1568628f, 0.1568628f), rgb(0.2509804f, 0.1254902f, 0.0627451f), rgb(0.3607844f, 0.0941177f, 0.0392157f),
        rgb(0.4862746f, 0.2274510f, 0.0784314f), rgb(0.4705883f, 0.4705883f, 0.5019608f), rgb(0.3058824f, 0.2431373f, 0.0627451f),
        rgb(0.5333334f, 0.3450981f, 0.0941177f), rgb(0.8156863f, 0.6274510f, 0.2901961f), rgb(0.0000000f, 0.0000000f, 0.0000000f),
        rgb(0.4235295f, 0.4392157f, 0.4392157f), rgb(0.4000000f, 0.2352942f, 0.1725491f), rgb(0.3764706f, 0.3686275f, 0.1882353f),
        rgb(0.2745099f, 0.3294118f, 0.6588236f), rgb(0.2196079f, 0.4392157f, 0.3450981f), rgb(0.3764706f, 0.2196079f, 0.0627451f),
        rgb(0.6588236f, 0.0627451f, 0.0313726f), rgb(0.1254902f, 0.1882353f, 0.4078432f), rgb(0.6588236f, 0.3764706f, 0.0000000f),
        rgb(0.4705883f, 0.4392157f, 0.4078432f), rgb(0.8470589f, 0.3215687f, 0.0313726f), rgb(0.9411765f, 0.0470589f, 0.0313726f),
        rgb(0.9607844f, 0.2823530f, 0.2823530f), rgb(0.9411765f, 0.6039216f, 0.4549020f), rgb(0.5490197f, 0.3137255f, 0.2509804f),
    )
    private val UPPER_LIP = arrayOf(
        rgb(0.0901961f, 0.0784314f, 0.0784314f), rgb(0.1254902f, 0.0627451f, 0.0313726f), rgb(0.1803922f, 0.0470589f, 0.0196079f),
        rgb(0.2901961f, 0.1372550f, 0.0470589f), rgb(0.3294118f, 0.3294118f, 0.3529412f), rgb(0.1529412f, 0.1215687f, 0.0313726f),
        rgb(0.3215687f, 0.2078432f, 0.0549020f), rgb(0.6941177f, 0.5019608f, 0.1568628f), rgb(0.0000000f, 0.0000000f, 0.0000000f),
        rgb(0.2980393f, 0.3058824f, 0.3058824f), rgb(0.2000000f, 0.1176471f, 0.0862746f), rgb(0.2274510f, 0.2196079f, 0.1137255f),
        rgb(0.1647059f, 0.1960785f, 0.3960785f), rgb(0.1529412f, 0.3058824f, 0.2431373f), rgb(0.1882353f, 0.1098040f, 0.0313726f),
        rgb(0.3960785f, 0.0392157f, 0.0196079f), rgb(0.0627451f, 0.0941177f, 0.2039216f), rgb(0.4627451f, 0.2627451f, 0.0000000f),
        rgb(0.3294118f, 0.3058824f, 0.2862746f), rgb(0.5098040f, 0.1882353f, 0.0941177f), rgb(0.4705883f, 0.0470589f, 0.0470589f),
        rgb(0.5333334f, 0.1254902f, 0.1568628f), rgb(0.8627451f, 0.4705883f, 0.3137255f), rgb(0.2745099f, 0.1176471f, 0.0392157f),
    )
    private val HAIR = arrayOf(
        rgb(0.118f, 0.102f, 0.094f), rgb(0.251f, 0.125f, 0.063f), rgb(0.361f, 0.094f, 0.039f), rgb(0.486f, 0.227f, 0.078f),
        rgb(0.471f, 0.471f, 0.502f), rgb(0.306f, 0.243f, 0.063f), rgb(0.533f, 0.345f, 0.094f), rgb(0.816f, 0.627f, 0.290f),
    )
    private val GLASS = arrayOf(
        rgb(0.094f, 0.094f, 0.094f), rgb(0.376f, 0.219f, 0.062f), rgb(0.658f, 0.062f, 0.031f),
        rgb(0.125f, 0.188f, 0.407f), rgb(0.658f, 0.376f, 0.000f), rgb(0.470f, 0.439f, 0.407f),
    )
    private val EYE = arrayOf(
        rgb(0.000f, 0.000f, 0.000f), rgb(0.424f, 0.439f, 0.439f), rgb(0.400f, 0.235f, 0.173f),
        rgb(0.376f, 0.369f, 0.188f), rgb(0.275f, 0.329f, 0.659f), rgb(0.220f, 0.439f, 0.345f),
    )
    private val MOUTH_R = arrayOf(
        rgb(0.847f, 0.322f, 0.031f), rgb(0.941f, 0.047f, 0.031f), rgb(0.961f, 0.282f, 0.282f),
        rgb(0.941f, 0.604f, 0.455f), rgb(0.549f, 0.314f, 0.251f),
    )
    private val MOUTH_G = arrayOf(
        rgb(0.510f, 0.188f, 0.094f), rgb(0.471f, 0.047f, 0.047f), rgb(0.533f, 0.125f, 0.157f),
        rgb(0.863f, 0.471f, 0.314f), rgb(0.275f, 0.118f, 0.039f),
    )
    private val FAVORITE = arrayOf(
        rgb(0.824f, 0.118f, 0.078f), rgb(1.000f, 0.431f, 0.098f), rgb(1.000f, 0.847f, 0.125f), rgb(0.471f, 0.824f, 0.125f),
        rgb(0.000f, 0.471f, 0.188f), rgb(0.039f, 0.282f, 0.706f), rgb(0.235f, 0.667f, 0.871f), rgb(0.961f, 0.353f, 0.490f),
        rgb(0.451f, 0.157f, 0.678f), rgb(0.282f, 0.220f, 0.094f), rgb(0.878f, 0.878f, 0.878f), rgb(0.094f, 0.094f, 0.078f),
    )
}

/**
 * FFL's description of a Mii (FFLiCharInfo's parts), made the way the PC makes it: the Wii Mii is
 * turned into Mii Studio's 46 values (MiiStudioDataSerializer.GenerateStudioDataArray), which are
 * then read as FFL parts (NativeMiiRenderer.MapStudioDataToCharInfo).
 */
internal class FflCharInfo(studio: IntArray) {
    val beardColor = studio[0] or COMMON_COLOR
    val beardType = studio[1]
    val build = studio[2]
    val eyeScaleY = studio[3]
    val eyeColor = studio[4] or COMMON_COLOR
    val eyeRotate = studio[5]
    val eyeScale = studio[6]
    val eyeType = studio[7]
    val eyeSpacingX = studio[8]
    val eyePositionY = studio[9]
    val eyebrowScaleY = studio[10]
    val eyebrowColor = studio[11] or COMMON_COLOR
    val eyebrowRotate = studio[12]
    val eyebrowScale = studio[13]
    val eyebrowType = studio[14]
    val eyebrowSpacingX = studio[15]
    val eyebrowPositionY = studio[16]
    val facelineColor = studio[17]
    val faceMakeup = studio[18]
    val faceType = studio[19]
    val faceLine = studio[20]
    val favoriteColor = studio[21]
    val gender = studio[22]
    val glassColor = studio[23] or COMMON_COLOR
    val glassScale = studio[24]
    val glassType = studio[25]
    val glassPositionY = studio[26]
    val hairColor = studio[27] or COMMON_COLOR
    val hairDir = studio[28]
    val hairType = studio[29]
    val height = studio[30]
    val moleScale = studio[31]
    val moleType = studio[32]
    val molePositionX = studio[33]
    val molePositionY = studio[34]
    val mouthScaleY = studio[35]
    val mouthColor = studio[36] or COMMON_COLOR
    val mouthScale = studio[37]
    val mouthType = studio[38]
    val mouthPositionY = studio[39]
    val mustacheScale = studio[40]
    val mustacheType = studio[41]
    val mustachePositionY = studio[42]
    val noseScale = studio[43]
    val noseType = studio[44]
    val nosePositionY = studio[45]

    companion object {
        const val COMMON_COLOR = 0x80000000.toInt()

        private val MAKEUP = intArrayOf(0, 1, 6, 9, 0, 0, 0, 0, 0, 10, 0, 0)
        private val WRINKLES = intArrayOf(0, 0, 0, 0, 5, 2, 3, 7, 8, 0, 9, 11)

        fun of(mii: Mii) = FflCharInfo(studio(mii))

        /** GenerateStudioDataArray: the Wii Mii's look as Mii Studio's values. */
        fun studio(mii: Mii): IntArray {
            val s = IntArray(46)
            s[0x16] = if (mii.girl) 1 else 0
            s[0x15] = mii.favoriteColor
            s[0x1E] = mii.height
            s[2] = mii.weight
            s[0x13] = mii.faceShape
            s[0x11] = mii.skinColor
            s[0x14] = WRINKLES.getOrElse(mii.facialFeature) { 0 }
            s[0x12] = MAKEUP.getOrElse(mii.facialFeature) { 0 }
            s[0x1D] = mii.hairType
            s[0x1B] = if (mii.hairColor == 0) 8 else mii.hairColor
            s[0x1C] = if (mii.hairFlipped) 1 else 0
            s[0xE] = mii.eyebrowType
            s[0xC] = mii.eyebrowRotation
            s[0xB] = if (mii.eyebrowColor == 0) 8 else mii.eyebrowColor
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
            s[0x24] = if (mii.lipColor < 4) mii.lipColor + 19 else 0
            s[0x25] = mii.lipSize
            s[0x23] = 3
            s[0x27] = mii.lipVertical
            s[0x29] = mii.mustacheType
            s[1] = mii.beardType
            s[0] = if (mii.facialHairColor == 0) 8 else mii.facialHairColor
            s[0x28] = mii.mustacheSize
            s[0x2A] = mii.mustacheVertical
            s[0x19] = mii.glassesType
            s[0x17] = when {
                mii.glassesColor == 0 -> 8
                mii.glassesColor < 6 -> mii.glassesColor + 13
                else -> 0
            }
            s[0x18] = mii.glassesSize
            s[0x1A] = mii.glassesVertical
            s[0x20] = if (mii.moleEnabled) 1 else 0
            s[0x1F] = mii.moleSize
            s[0x22] = mii.moleVertical
            s[0x21] = mii.moleHorizontal
            return s
        }
    }
}

package org.wiicompiled.quest.launcher

import kotlin.random.Random

/**
 * A Wii Mii, as the 74 bytes the Mii database stores (RFL's RFLCharData), field for field like the
 * PC launcher's Mii model (WheelWizard's WiiManagement/MiiManagement). The ranges are the Wii's,
 * which the editor keeps to and [MiiData.parse] checks.
 */
data class Mii(
    var invalid: Boolean = false,
    var girl: Boolean = false,
    /** Kept as stored, 0 when the Mii has no birthday; the editor does not show it. */
    var birthMonth: Int = 1,
    var birthDay: Int = 1,
    var favoriteColor: Int = 11,
    var favorite: Boolean = false,
    var name: String = "no name",
    var height: Int = 1,
    var weight: Int = 1,
    /** Also called the avatar ID; licences refer to their Mii by it. */
    var miiId: Long = 0,
    /** Also called the client ID: derived from the MAC address of the console that made it. */
    var systemId: Long = 0,
    var faceShape: Int = 5,
    var skinColor: Int = 0,
    var facialFeature: Int = 0,
    var mingleOff: Boolean = false,
    var downloaded: Boolean = false,
    var hairType: Int = 1,
    var hairColor: Int = 0,
    var hairFlipped: Boolean = false,
    var eyebrowType: Int = 1,
    var eyebrowRotation: Int = 0,
    var eyebrowColor: Int = 0,
    var eyebrowSize: Int = 4,
    var eyebrowVertical: Int = 10,
    var eyebrowSpacing: Int = 1,
    var eyeType: Int = 1,
    var eyeRotation: Int = 6,
    var eyeVertical: Int = 7,
    var eyeColor: Int = 0,
    var eyeSize: Int = 3,
    var eyeSpacing: Int = 6,
    var noseType: Int = 0,
    var noseSize: Int = 6,
    var noseVertical: Int = 4,
    var lipType: Int = 1,
    var lipColor: Int = 0,
    var lipSize: Int = 4,
    var lipVertical: Int = 9,
    var glassesType: Int = 0,
    var glassesColor: Int = 0,
    var glassesSize: Int = 4,
    var glassesVertical: Int = 1,
    var mustacheType: Int = 0,
    var beardType: Int = 0,
    var facialHairColor: Int = 0,
    var mustacheSize: Int = 1,
    var mustacheVertical: Int = 1,
    var moleEnabled: Boolean = false,
    var moleSize: Int = 0,
    var moleVertical: Int = 0,
    var moleHorizontal: Int = 0,
    var creatorName: String = "no name",
) {
    /** Every field that shows in a picture, which is what a render is cached by. */
    fun lookKey(): String = listOf(
        girl, favoriteColor, height, weight, faceShape, skinColor, facialFeature, hairType, hairColor, hairFlipped,
        eyebrowType, eyebrowRotation, eyebrowColor, eyebrowSize, eyebrowVertical, eyebrowSpacing,
        eyeType, eyeRotation, eyeVertical, eyeColor, eyeSize, eyeSpacing, noseType, noseSize, noseVertical,
        lipType, lipColor, lipSize, lipVertical, glassesType, glassesColor, glassesSize, glassesVertical,
        mustacheType, beardType, facialHairColor, mustacheSize, mustacheVertical,
        moleEnabled, moleSize, moleVertical, moleHorizontal,
    ).joinToString(",")
}

/** The Wii's value ranges, from the PC's Mii part classes and editor pages. */
object MiiRanges {
    const val NAME_LENGTH = 10
    const val SCALE_MAX = 127
    const val FAVORITE_COLORS = 12
    const val FACE_SHAPES = 8
    const val SKIN_COLORS = 6
    const val FACIAL_FEATURES = 12
    const val HAIR_TYPES = 72
    const val HAIR_COLORS = 8
    const val EYEBROW_TYPES = 24
    val EYEBROW_ROTATION = 0..11
    val EYEBROW_SIZE = 0..8
    val EYEBROW_VERTICAL = 3..18
    val EYEBROW_SPACING = 0..12
    const val EYE_TYPES = 48
    const val EYE_COLORS = 6
    val EYE_ROTATION = 0..7
    val EYE_VERTICAL = 0..18
    val EYE_SIZE = 0..7
    val EYE_SPACING = 0..12
    const val NOSE_TYPES = 12
    val NOSE_SIZE = 0..8
    val NOSE_VERTICAL = 0..18
    const val LIP_TYPES = 24
    const val LIP_COLORS = 3
    val LIP_SIZE = 0..8
    val LIP_VERTICAL = 0..18
    const val GLASSES_TYPES = 9
    const val GLASSES_COLORS = 6
    val GLASSES_SIZE = 0..7
    val GLASSES_VERTICAL = 0..20
    const val MUSTACHE_TYPES = 4
    const val BEARD_TYPES = 4
    val FACIAL_HAIR_SIZE = 0..8
    val FACIAL_HAIR_VERTICAL = 0..16
    val MOLE_SIZE = 0..8
    val MOLE_VERTICAL = 0..30
    val MOLE_HORIZONTAL = 0..16
}

/** The 74-byte Mii block: MiiSerializer on the PC. */
object MiiData {
    const val SIZE = 74

    /** Reads a block, refusing empty slots and values the Wii never writes. */
    fun parse(data: ByteArray): Mii {
        require(data.size == SIZE) { "Invalid Mii data length." }
        require(!data.all { it == 0.toByte() } && !data.all { it == 0xFF.toByte() }) { "Mii data is empty." }
        val mii = Mii()
        val header = u16(data, 0)
        mii.invalid = header and 0x8000 != 0
        mii.girl = header and 0x4000 != 0
        mii.birthMonth = (header shr 10) and 0x0F
        mii.birthDay = (header shr 5) and 0x1F
        mii.favoriteColor = check((header shr 1) and 0x0F, MiiRanges.FAVORITE_COLORS, "MiiFavoriteColor")
        mii.favorite = header and 0x01 != 0

        mii.name = name(data, 0x02)
        require(mii.name.isNotEmpty()) { "Invalid MiiName" }
        mii.height = check(data[0x16].toInt() and 0xFF, MiiRanges.SCALE_MAX + 1, "Height")
        mii.weight = check(data[0x17].toInt() and 0xFF, MiiRanges.SCALE_MAX + 1, "Weight")
        mii.miiId = u32(data, 0x18)
        mii.systemId = u32(data, 0x1C)

        val face = u16(data, 0x20)
        mii.faceShape = (face shr 13) and 0x07
        mii.skinColor = check((face shr 10) and 0x07, MiiRanges.SKIN_COLORS, "SkinColor")
        mii.facialFeature = check((face shr 6) and 0x0F, MiiRanges.FACIAL_FEATURES, "FacialFeature")
        mii.mingleOff = (face shr 2) and 0x01 != 0
        mii.downloaded = face and 0x01 != 0

        val hair = u16(data, 0x22)
        mii.hairType = check((hair shr 9) and 0x7F, MiiRanges.HAIR_TYPES, "HairType")
        mii.hairColor = (hair shr 6) and 0x07
        mii.hairFlipped = (hair shr 5) and 0x01 != 0

        val brow = u32(data, 0x24)
        mii.eyebrowType = check(((brow shr 27) and 0x1F).toInt(), MiiRanges.EYEBROW_TYPES, "Eyebrow type")
        mii.eyebrowRotation = check(((brow shr 22) and 0x0F).toInt(), MiiRanges.EYEBROW_ROTATION, "Eyebrow rotation")
        mii.eyebrowColor = ((brow shr 13) and 0x07).toInt()
        mii.eyebrowSize = check(((brow shr 9) and 0x0F).toInt(), MiiRanges.EYEBROW_SIZE, "Eyebrow size")
        mii.eyebrowVertical = check(((brow shr 4) and 0x1F).toInt(), MiiRanges.EYEBROW_VERTICAL, "Eyebrow vertical position")
        mii.eyebrowSpacing = check((brow and 0x0F).toInt(), MiiRanges.EYEBROW_SPACING, "Eyebrow spacing")

        val eye = u32(data, 0x28)
        mii.eyeType = check(((eye shr 26) and 0x3F).toInt(), MiiRanges.EYE_TYPES, "Eye type")
        mii.eyeRotation = ((eye shr 21) and 0x07).toInt()
        mii.eyeVertical = check(((eye shr 16) and 0x1F).toInt(), MiiRanges.EYE_VERTICAL, "Eye vertical position")
        mii.eyeColor = check(((eye shr 13) and 0x07).toInt(), MiiRanges.EYE_COLORS, "EyeColor")
        mii.eyeSize = ((eye shr 9) and 0x07).toInt()
        mii.eyeSpacing = check(((eye shr 5) and 0x0F).toInt(), MiiRanges.EYE_SPACING, "Eye spacing")

        val nose = u16(data, 0x2C)
        mii.noseType = check((nose shr 12) and 0x0F, MiiRanges.NOSE_TYPES, "NoseType")
        mii.noseSize = check((nose shr 8) and 0x0F, MiiRanges.NOSE_SIZE, "Nose size")
        mii.noseVertical = check((nose shr 3) and 0x1F, MiiRanges.NOSE_VERTICAL, "Nose vertical position")

        val lip = u16(data, 0x2E)
        mii.lipType = check((lip shr 11) and 0x1F, MiiRanges.LIP_TYPES, "Lip type")
        mii.lipColor = check((lip shr 9) and 0x03, MiiRanges.LIP_COLORS, "LipColor")
        mii.lipSize = check((lip shr 5) and 0x0F, MiiRanges.LIP_SIZE, "Lip size")
        mii.lipVertical = check(lip and 0x1F, MiiRanges.LIP_VERTICAL, "Lip vertical position")

        val glasses = u16(data, 0x30)
        mii.glassesType = check((glasses shr 12) and 0x0F, MiiRanges.GLASSES_TYPES, "GlassesType")
        mii.glassesColor = check((glasses shr 9) and 0x07, MiiRanges.GLASSES_COLORS, "GlassesColor")
        mii.glassesSize = (glasses shr 5) and 0x07
        mii.glassesVertical = check(glasses and 0x1F, MiiRanges.GLASSES_VERTICAL, "Glasses vertical position")

        val facial = u16(data, 0x32)
        mii.mustacheType = (facial shr 14) and 0x03
        mii.beardType = (facial shr 12) and 0x03
        mii.facialHairColor = (facial shr 9) and 0x07
        mii.mustacheSize = check((facial shr 5) and 0x0F, MiiRanges.FACIAL_HAIR_SIZE, "Facial hair size")
        mii.mustacheVertical = check(facial and 0x1F, MiiRanges.FACIAL_HAIR_VERTICAL, "Facial hair vertical position")

        val mole = u16(data, 0x34)
        mii.moleEnabled = (mole shr 15) and 0x01 != 0
        mii.moleSize = check((mole shr 11) and 0x0F, MiiRanges.MOLE_SIZE, "Mole size")
        mii.moleVertical = check((mole shr 6) and 0x1F, MiiRanges.MOLE_VERTICAL, "Mole vertical position")
        mii.moleHorizontal = check((mole shr 1) and 0x1F, MiiRanges.MOLE_HORIZONTAL, "Mole horizontal position")

        mii.creatorName = name(data, 0x36)
        return mii
    }

    fun serialize(mii: Mii): ByteArray {
        require(mii.miiId != 0L) { "Mii ID cannot be 0." }
        require(mii.name.length <= MiiRanges.NAME_LENGTH && mii.creatorName.length <= MiiRanges.NAME_LENGTH) {
            "Mii name too long, maximum is 10 characters"
        }
        val data = ByteArray(SIZE)
        var header = 0
        if (mii.invalid) header = header or 0x8000
        if (mii.girl) header = header or 0x4000
        header = header or ((mii.birthMonth and 0x0F) shl 10) or ((mii.birthDay and 0x1F) shl 5)
        header = header or ((mii.favoriteColor and 0x0F) shl 1)
        if (mii.favorite) header = header or 0x01
        put16(data, 0, header)
        putName(data, 0x02, mii.name)
        data[0x16] = mii.height.toByte()
        data[0x17] = mii.weight.toByte()
        put32(data, 0x18, mii.miiId)
        put32(data, 0x1C, mii.systemId)
        put16(
            data, 0x20,
            ((mii.faceShape and 0x07) shl 13) or ((mii.skinColor and 0x07) shl 10) or
                ((mii.facialFeature and 0x0F) shl 6) or (bit(mii.mingleOff) shl 2) or bit(mii.downloaded),
        )
        put16(data, 0x22, ((mii.hairType and 0x7F) shl 9) or ((mii.hairColor and 0x07) shl 6) or (bit(mii.hairFlipped) shl 5))
        put32(
            data, 0x24,
            ((mii.eyebrowType.toLong() and 0x1F) shl 27) or ((mii.eyebrowRotation.toLong() and 0x0F) shl 22) or
                ((mii.eyebrowColor.toLong() and 0x07) shl 13) or ((mii.eyebrowSize.toLong() and 0x0F) shl 9) or
                ((mii.eyebrowVertical.toLong() and 0x1F) shl 4) or (mii.eyebrowSpacing.toLong() and 0x0F),
        )
        put32(
            data, 0x28,
            ((mii.eyeType.toLong() and 0x3F) shl 26) or ((mii.eyeRotation.toLong() and 0x07) shl 21) or
                ((mii.eyeVertical.toLong() and 0x1F) shl 16) or ((mii.eyeColor.toLong() and 0x07) shl 13) or
                ((mii.eyeSize.toLong() and 0x07) shl 9) or ((mii.eyeSpacing.toLong() and 0x0F) shl 5),
        )
        put16(data, 0x2C, ((mii.noseType and 0x0F) shl 12) or ((mii.noseSize and 0x0F) shl 8) or ((mii.noseVertical and 0x1F) shl 3))
        put16(
            data, 0x2E,
            ((mii.lipType and 0x1F) shl 11) or ((mii.lipColor and 0x03) shl 9) or ((mii.lipSize and 0x0F) shl 5) or
                (mii.lipVertical and 0x1F),
        )
        put16(
            data, 0x30,
            ((mii.glassesType and 0x0F) shl 12) or ((mii.glassesColor and 0x07) shl 9) or
                ((mii.glassesSize and 0x07) shl 5) or (mii.glassesVertical and 0x1F),
        )
        put16(
            data, 0x32,
            ((mii.mustacheType and 0x03) shl 14) or ((mii.beardType and 0x03) shl 12) or
                ((mii.facialHairColor and 0x07) shl 9) or ((mii.mustacheSize and 0x0F) shl 5) or (mii.mustacheVertical and 0x1F),
        )
        put16(
            data, 0x34,
            (bit(mii.moleEnabled) shl 15) or ((mii.moleSize and 0x0F) shl 11) or ((mii.moleVertical and 0x1F) shl 6) or
                ((mii.moleHorizontal and 0x1F) shl 1),
        )
        putName(data, 0x36, mii.creatorName)
        return data
    }

    /** The Mii ID's lower 29 bits count 4-second ticks from 2006, so it also dates the Mii. */
    fun creationTimeMillis(miiId: Long): Long? =
        if (miiId == 0L) null else EPOCH_2006_MILLIS + (miiId and 0x1FFFFFFF) * 4_000L

    const val EPOCH_2006_MILLIS = 1_136_073_600_000L

    private fun check(value: Int, count: Int, what: String): Int = check(value, 0 until count, what)

    private fun check(value: Int, range: IntRange, what: String): Int {
        require(value in range) { "Invalid $what" }
        return value
    }

    private fun bit(value: Boolean) = if (value) 1 else 0

    private fun u16(data: ByteArray, offset: Int) = ((data[offset].toInt() and 0xFF) shl 8) or (data[offset + 1].toInt() and 0xFF)

    private fun u32(data: ByteArray, offset: Int): Long = (u16(data, offset).toLong() shl 16) or u16(data, offset + 2).toLong()

    private fun put16(data: ByteArray, offset: Int, value: Int) {
        data[offset] = (value shr 8).toByte()
        data[offset + 1] = value.toByte()
    }

    private fun put32(data: ByteArray, offset: Int, value: Long) {
        put16(data, offset, (value shr 16).toInt())
        put16(data, offset + 2, value.toInt())
    }

    private fun name(data: ByteArray, offset: Int): String =
        String(data, offset, 20, Charsets.UTF_16BE).trimEnd('\u0000')

    private fun putName(data: ByteArray, offset: Int, name: String) {
        name.padEnd(MiiRanges.NAME_LENGTH, '\u0000').toByteArray(Charsets.UTF_16BE).copyInto(data, offset)
    }
}

/** New Miis, as the PC's MiiFactory makes them. */
object MiiFactory {
    private fun base(name: String) = Mii(
        name = name,
        creatorName = "",
        favorite = false,
        favoriteColor = 0,
        faceShape = 0,
        skinColor = 0,
        facialFeature = 0,
        hairType = 30,
        hairColor = 1,
        hairFlipped = false,
        eyebrowType = 0, eyebrowRotation = 6, eyebrowColor = 1, eyebrowSize = 4, eyebrowVertical = 10, eyebrowSpacing = 2,
        glassesType = 0, glassesColor = 0, glassesSize = 4, glassesVertical = 10,
        eyeType = 2, eyeRotation = 4, eyeVertical = 12, eyeColor = 0, eyeSize = 4, eyeSpacing = 2,
        noseType = 1, noseSize = 4, noseVertical = 9,
        lipType = 23, lipColor = 0, lipSize = 4, lipVertical = 13,
        mustacheType = 0, beardType = 0, facialHairColor = 0, mustacheSize = 4, mustacheVertical = 10,
        moleEnabled = false, moleSize = 4, moleVertical = 20, moleHorizontal = 2,
        height = 63,
        weight = 63,
        miiId = 1,
    )

    fun female(name: String): Mii = base(name).apply {
        girl = true
        hairType = 12
        eyeType = 4
        eyeRotation = 3
    }

    fun male(name: String): Mii = base(name).apply {
        girl = false
        hairType = 33
        eyebrowType = 6
    }

    fun random(name: String, random: Random = Random.Default): Mii = base(name).apply {
        val hair = random.nextInt(MiiRanges.HAIR_COLORS)
        girl = random.nextInt(2) == 0
        hairType = random.nextInt(71)
        hairColor = hair
        hairFlipped = random.nextInt(3) == 0
        eyebrowType = random.nextInt(23)
        eyebrowColor = hair
        eyeType = random.nextInt(47)
        eyeColor = random.nextInt(MiiRanges.EYE_COLORS)
        favoriteColor = random.nextInt(MiiRanges.FAVORITE_COLORS)
        faceShape = random.nextInt(MiiRanges.FACE_SHAPES)
        skinColor = random.nextInt(MiiRanges.SKIN_COLORS)
        facialFeature = random.nextInt(MiiRanges.FACIAL_FEATURES)
        noseType = random.nextInt(MiiRanges.NOSE_TYPES)
        lipType = random.nextInt(23)
        lipColor = random.nextInt(MiiRanges.LIP_COLORS)
        moleEnabled = random.nextInt(4) == 0
        if (random.nextInt(4) == 0) {
            glassesType = random.nextInt(MiiRanges.GLASSES_TYPES)
            glassesColor = random.nextInt(MiiRanges.GLASSES_COLORS)
        }
        if (random.nextInt(4) == 0) {
            mustacheType = random.nextInt(MiiRanges.MUSTACHE_TYPES)
            beardType = random.nextInt(MiiRanges.BEARD_TYPES)
            facialHairColor = hair
        }
    }

    /**
     * The editor's Randomize: a new look that keeps who the Mii is. The PC also resets the birthday
     * and the mingle and downloaded flags, which the editor does not show; those are kept here.
     */
    fun randomLook(of: Mii, random: Random = Random.Default): Mii = random(of.name, random).also {
        it.favorite = of.favorite
        it.miiId = of.miiId
        it.systemId = of.systemId
        it.creatorName = of.creatorName
        it.birthMonth = of.birthMonth
        it.birthDay = of.birthDay
        it.mingleOff = of.mingleOff
        it.downloaded = of.downloaded
    }
}

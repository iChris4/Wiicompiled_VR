// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// A Wii Mii, as the 74 bytes the Mii database stores (RFL's RFLCharData), field for field like the
/// Quest launcher's (android/.../launcher/Mii.kt) and the PC launcher's Mii model (WheelWizard's
/// WiiManagement/MiiManagement). The ranges are the Wii's, which the editor keeps to and
/// `MiiData.parse` checks.
struct Mii: Hashable {
    var invalid = false
    var girl = false
    /// Kept as stored, 0 when the Mii has no birthday; the editor does not show it.
    var birthMonth = 1
    var birthDay = 1
    var favoriteColor = 11
    var favorite = false
    var name = "no name"
    var height = 1
    var weight = 1
    /// Also called the avatar ID; licences refer to their Mii by it.
    var miiId: UInt32 = 0
    /// Also called the client ID: derived from the MAC address of the console that made it.
    var systemId: UInt32 = 0
    var faceShape = 5
    var skinColor = 0
    var facialFeature = 0
    var mingleOff = false
    var downloaded = false
    var hairType = 1
    var hairColor = 0
    var hairFlipped = false
    var eyebrowType = 1
    var eyebrowRotation = 0
    var eyebrowColor = 0
    var eyebrowSize = 4
    var eyebrowVertical = 10
    var eyebrowSpacing = 1
    var eyeType = 1
    var eyeRotation = 6
    var eyeVertical = 7
    var eyeColor = 0
    var eyeSize = 3
    var eyeSpacing = 6
    var noseType = 0
    var noseSize = 6
    var noseVertical = 4
    var lipType = 1
    var lipColor = 0
    var lipSize = 4
    var lipVertical = 9
    var glassesType = 0
    var glassesColor = 0
    var glassesSize = 4
    var glassesVertical = 1
    var mustacheType = 0
    var beardType = 0
    var facialHairColor = 0
    var mustacheSize = 1
    var mustacheVertical = 1
    var moleEnabled = false
    var moleSize = 0
    var moleVertical = 0
    var moleHorizontal = 0
    var creatorName = "no name"

    /// Every field that shows in a picture, which is what a render is cached by.
    var lookKey: String {
        let look: [Any] = [
            girl, favoriteColor, height, weight, faceShape, skinColor, facialFeature, hairType, hairColor, hairFlipped,
            eyebrowType, eyebrowRotation, eyebrowColor, eyebrowSize, eyebrowVertical, eyebrowSpacing,
            eyeType, eyeRotation, eyeVertical, eyeColor, eyeSize, eyeSpacing, noseType, noseSize, noseVertical,
            lipType, lipColor, lipSize, lipVertical, glassesType, glassesColor, glassesSize, glassesVertical,
            mustacheType, beardType, facialHairColor, mustacheSize, mustacheVertical,
            moleEnabled, moleSize, moleVertical, moleHorizontal,
        ]
        return look.map { "\($0)" }.joined(separator: ",")
    }
}

/// The Wii's value ranges, from the PC's Mii part classes and editor pages.
enum MiiRanges {
    /// In UTF-16 code units, as the database stores names.
    static let nameLength = 10
    static let scaleMax = 127
    static let favoriteColors = 12
    static let faceShapes = 8
    static let skinColors = 6
    static let facialFeatures = 12
    static let hairTypes = 72
    static let hairColors = 8
    static let eyebrowTypes = 24
    static let eyebrowRotation = 0...11
    static let eyebrowSize = 0...8
    static let eyebrowVertical = 3...18
    static let eyebrowSpacing = 0...12
    static let eyeTypes = 48
    static let eyeColors = 6
    static let eyeRotation = 0...7
    static let eyeVertical = 0...18
    static let eyeSize = 0...7
    static let eyeSpacing = 0...12
    static let noseTypes = 12
    static let noseSize = 0...8
    static let noseVertical = 0...18
    static let lipTypes = 24
    static let lipColors = 3
    static let lipSize = 0...8
    static let lipVertical = 0...18
    static let glassesTypes = 9
    static let glassesColors = 6
    static let glassesSize = 0...7
    static let glassesVertical = 0...20
    static let mustacheTypes = 4
    static let beardTypes = 4
    static let facialHairSize = 0...8
    static let facialHairVertical = 0...16
    static let moleSize = 0...8
    static let moleVertical = 0...30
    static let moleHorizontal = 0...16
}

/// Why a Mii block or file was refused; the message is shown to the player as it is.
struct MiiError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}

/// The 74-byte Mii block: MiiSerializer on the PC, MiiData in the Quest launcher.
enum MiiData {
    static let size = 74

    /// Reads a block, refusing empty slots and values the Wii never writes.
    static func parse(_ data: Data) throws -> Mii {
        let bytes = [UInt8](data)
        guard bytes.count == size else { throw MiiError("Invalid Mii data length.") }
        guard !bytes.allSatisfy({ $0 == 0 }), !bytes.allSatisfy({ $0 == 0xFF }) else { throw MiiError("Mii data is empty.") }
        var mii = Mii()
        let header = u16(bytes, 0)
        mii.invalid = header & 0x8000 != 0
        mii.girl = header & 0x4000 != 0
        mii.birthMonth = (header >> 10) & 0x0F
        mii.birthDay = (header >> 5) & 0x1F
        mii.favoriteColor = try check((header >> 1) & 0x0F, MiiRanges.favoriteColors, "MiiFavoriteColor")
        mii.favorite = header & 0x01 != 0

        mii.name = name(bytes, 0x02)
        guard !mii.name.isEmpty else { throw MiiError("Invalid MiiName") }
        mii.height = try check(Int(bytes[0x16]), MiiRanges.scaleMax + 1, "Height")
        mii.weight = try check(Int(bytes[0x17]), MiiRanges.scaleMax + 1, "Weight")
        mii.miiId = u32(bytes, 0x18)
        mii.systemId = u32(bytes, 0x1C)

        let face = u16(bytes, 0x20)
        mii.faceShape = (face >> 13) & 0x07
        mii.skinColor = try check((face >> 10) & 0x07, MiiRanges.skinColors, "SkinColor")
        mii.facialFeature = try check((face >> 6) & 0x0F, MiiRanges.facialFeatures, "FacialFeature")
        mii.mingleOff = (face >> 2) & 0x01 != 0
        mii.downloaded = face & 0x01 != 0

        let hair = u16(bytes, 0x22)
        mii.hairType = try check((hair >> 9) & 0x7F, MiiRanges.hairTypes, "HairType")
        mii.hairColor = (hair >> 6) & 0x07
        mii.hairFlipped = (hair >> 5) & 0x01 != 0

        let brow = Int(u32(bytes, 0x24))
        mii.eyebrowType = try check((brow >> 27) & 0x1F, MiiRanges.eyebrowTypes, "Eyebrow type")
        mii.eyebrowRotation = try check((brow >> 22) & 0x0F, MiiRanges.eyebrowRotation, "Eyebrow rotation")
        mii.eyebrowColor = (brow >> 13) & 0x07
        mii.eyebrowSize = try check((brow >> 9) & 0x0F, MiiRanges.eyebrowSize, "Eyebrow size")
        mii.eyebrowVertical = try check((brow >> 4) & 0x1F, MiiRanges.eyebrowVertical, "Eyebrow vertical position")
        mii.eyebrowSpacing = try check(brow & 0x0F, MiiRanges.eyebrowSpacing, "Eyebrow spacing")

        let eye = Int(u32(bytes, 0x28))
        mii.eyeType = try check((eye >> 26) & 0x3F, MiiRanges.eyeTypes, "Eye type")
        mii.eyeRotation = (eye >> 21) & 0x07
        mii.eyeVertical = try check((eye >> 16) & 0x1F, MiiRanges.eyeVertical, "Eye vertical position")
        mii.eyeColor = try check((eye >> 13) & 0x07, MiiRanges.eyeColors, "EyeColor")
        mii.eyeSize = (eye >> 9) & 0x07
        mii.eyeSpacing = try check((eye >> 5) & 0x0F, MiiRanges.eyeSpacing, "Eye spacing")

        let nose = u16(bytes, 0x2C)
        mii.noseType = try check((nose >> 12) & 0x0F, MiiRanges.noseTypes, "NoseType")
        mii.noseSize = try check((nose >> 8) & 0x0F, MiiRanges.noseSize, "Nose size")
        mii.noseVertical = try check((nose >> 3) & 0x1F, MiiRanges.noseVertical, "Nose vertical position")

        let lip = u16(bytes, 0x2E)
        mii.lipType = try check((lip >> 11) & 0x1F, MiiRanges.lipTypes, "Lip type")
        mii.lipColor = try check((lip >> 9) & 0x03, MiiRanges.lipColors, "LipColor")
        mii.lipSize = try check((lip >> 5) & 0x0F, MiiRanges.lipSize, "Lip size")
        mii.lipVertical = try check(lip & 0x1F, MiiRanges.lipVertical, "Lip vertical position")

        let glasses = u16(bytes, 0x30)
        mii.glassesType = try check((glasses >> 12) & 0x0F, MiiRanges.glassesTypes, "GlassesType")
        mii.glassesColor = try check((glasses >> 9) & 0x07, MiiRanges.glassesColors, "GlassesColor")
        mii.glassesSize = (glasses >> 5) & 0x07
        mii.glassesVertical = try check(glasses & 0x1F, MiiRanges.glassesVertical, "Glasses vertical position")

        let facial = u16(bytes, 0x32)
        mii.mustacheType = (facial >> 14) & 0x03
        mii.beardType = (facial >> 12) & 0x03
        mii.facialHairColor = (facial >> 9) & 0x07
        mii.mustacheSize = try check((facial >> 5) & 0x0F, MiiRanges.facialHairSize, "Facial hair size")
        mii.mustacheVertical = try check(facial & 0x1F, MiiRanges.facialHairVertical, "Facial hair vertical position")

        let mole = u16(bytes, 0x34)
        mii.moleEnabled = (mole >> 15) & 0x01 != 0
        mii.moleSize = try check((mole >> 11) & 0x0F, MiiRanges.moleSize, "Mole size")
        mii.moleVertical = try check((mole >> 6) & 0x1F, MiiRanges.moleVertical, "Mole vertical position")
        mii.moleHorizontal = try check((mole >> 1) & 0x1F, MiiRanges.moleHorizontal, "Mole horizontal position")

        mii.creatorName = name(bytes, 0x36)
        return mii
    }

    static func serialize(_ mii: Mii) throws -> Data {
        guard mii.miiId != 0 else { throw MiiError("Mii ID cannot be 0.") }
        guard mii.name.utf16.count <= MiiRanges.nameLength, mii.creatorName.utf16.count <= MiiRanges.nameLength else {
            throw MiiError("Mii name too long, maximum is 10 characters")
        }
        var data = [UInt8](repeating: 0, count: size)
        var header = 0
        if mii.invalid { header |= 0x8000 }
        if mii.girl { header |= 0x4000 }
        header |= ((mii.birthMonth & 0x0F) << 10) | ((mii.birthDay & 0x1F) << 5)
        header |= (mii.favoriteColor & 0x0F) << 1
        if mii.favorite { header |= 0x01 }
        put16(&data, 0, header)
        putName(&data, 0x02, mii.name)
        data[0x16] = UInt8(truncatingIfNeeded: mii.height)
        data[0x17] = UInt8(truncatingIfNeeded: mii.weight)
        put32(&data, 0x18, mii.miiId)
        put32(&data, 0x1C, mii.systemId)
        put16(&data, 0x20,
              ((mii.faceShape & 0x07) << 13) | ((mii.skinColor & 0x07) << 10) |
                  ((mii.facialFeature & 0x0F) << 6) | (bit(mii.mingleOff) << 2) | bit(mii.downloaded))
        put16(&data, 0x22, ((mii.hairType & 0x7F) << 9) | ((mii.hairColor & 0x07) << 6) | (bit(mii.hairFlipped) << 5))
        put32(&data, 0x24, UInt32(truncatingIfNeeded:
            ((mii.eyebrowType & 0x1F) << 27) | ((mii.eyebrowRotation & 0x0F) << 22) |
                ((mii.eyebrowColor & 0x07) << 13) | ((mii.eyebrowSize & 0x0F) << 9) |
                ((mii.eyebrowVertical & 0x1F) << 4) | (mii.eyebrowSpacing & 0x0F)))
        put32(&data, 0x28, UInt32(truncatingIfNeeded:
            ((mii.eyeType & 0x3F) << 26) | ((mii.eyeRotation & 0x07) << 21) |
                ((mii.eyeVertical & 0x1F) << 16) | ((mii.eyeColor & 0x07) << 13) |
                ((mii.eyeSize & 0x07) << 9) | ((mii.eyeSpacing & 0x0F) << 5)))
        put16(&data, 0x2C, ((mii.noseType & 0x0F) << 12) | ((mii.noseSize & 0x0F) << 8) | ((mii.noseVertical & 0x1F) << 3))
        put16(&data, 0x2E,
              ((mii.lipType & 0x1F) << 11) | ((mii.lipColor & 0x03) << 9) | ((mii.lipSize & 0x0F) << 5) |
                  (mii.lipVertical & 0x1F))
        put16(&data, 0x30,
              ((mii.glassesType & 0x0F) << 12) | ((mii.glassesColor & 0x07) << 9) |
                  ((mii.glassesSize & 0x07) << 5) | (mii.glassesVertical & 0x1F))
        put16(&data, 0x32,
              ((mii.mustacheType & 0x03) << 14) | ((mii.beardType & 0x03) << 12) |
                  ((mii.facialHairColor & 0x07) << 9) | ((mii.mustacheSize & 0x0F) << 5) | (mii.mustacheVertical & 0x1F))
        put16(&data, 0x34,
              (bit(mii.moleEnabled) << 15) | ((mii.moleSize & 0x0F) << 11) | ((mii.moleVertical & 0x1F) << 6) |
                  ((mii.moleHorizontal & 0x1F) << 1))
        putName(&data, 0x36, mii.creatorName)
        return Data(data)
    }

    /// The Mii ID's lower 29 bits count 4-second ticks from 2006, so it also dates the Mii.
    static func creationDate(miiId: UInt32) -> Date? {
        guard miiId != 0 else { return nil }
        return epoch2006.addingTimeInterval(TimeInterval(miiId & 0x1FFF_FFFF) * 4)
    }

    /// 2006-01-01 00:00 UTC, which Mii IDs count from.
    static let epoch2006 = Date(timeIntervalSince1970: 1_136_073_600)

    private static func check(_ value: Int, _ count: Int, _ what: String) throws -> Int {
        try check(value, 0...(count - 1), what)
    }

    private static func check(_ value: Int, _ range: ClosedRange<Int>, _ what: String) throws -> Int {
        guard range.contains(value) else { throw MiiError("Invalid \(what)") }
        return value
    }

    private static func bit(_ value: Bool) -> Int { value ? 1 : 0 }

    private static func u16(_ data: [UInt8], _ offset: Int) -> Int { Int(data[offset]) << 8 | Int(data[offset + 1]) }

    private static func u32(_ data: [UInt8], _ offset: Int) -> UInt32 {
        UInt32(u16(data, offset)) << 16 | UInt32(u16(data, offset + 2))
    }

    private static func put16(_ data: inout [UInt8], _ offset: Int, _ value: Int) {
        data[offset] = UInt8(truncatingIfNeeded: value >> 8)
        data[offset + 1] = UInt8(truncatingIfNeeded: value)
    }

    private static func put32(_ data: inout [UInt8], _ offset: Int, _ value: UInt32) {
        put16(&data, offset, Int(value >> 16))
        put16(&data, offset + 2, Int(value & 0xFFFF))
    }

    /// Ten UTF-16BE code units, the trailing NULs trimmed.
    private static func name(_ data: [UInt8], _ offset: Int) -> String {
        var units = (0..<MiiRanges.nameLength).map { UInt16(data[offset + $0 * 2]) << 8 | UInt16(data[offset + $0 * 2 + 1]) }
        while units.last == 0 { units.removeLast() }
        return String(decoding: units, as: UTF16.self)
    }

    private static func putName(_ data: inout [UInt8], _ offset: Int, _ name: String) {
        for (index, unit) in name.utf16.prefix(MiiRanges.nameLength).enumerated() {
            data[offset + index * 2] = UInt8(unit >> 8)
            data[offset + index * 2 + 1] = UInt8(unit & 0xFF)
        }
    }
}

/// New Miis, as the PC's MiiFactory makes them.
enum MiiFactory {
    private static func base(_ name: String) -> Mii {
        var mii = Mii()
        mii.name = name
        mii.creatorName = ""
        mii.favorite = false
        mii.favoriteColor = 0
        mii.faceShape = 0
        mii.skinColor = 0
        mii.facialFeature = 0
        mii.hairType = 30
        mii.hairColor = 1
        mii.hairFlipped = false
        mii.eyebrowType = 0; mii.eyebrowRotation = 6; mii.eyebrowColor = 1; mii.eyebrowSize = 4; mii.eyebrowVertical = 10; mii.eyebrowSpacing = 2
        mii.glassesType = 0; mii.glassesColor = 0; mii.glassesSize = 4; mii.glassesVertical = 10
        mii.eyeType = 2; mii.eyeRotation = 4; mii.eyeVertical = 12; mii.eyeColor = 0; mii.eyeSize = 4; mii.eyeSpacing = 2
        mii.noseType = 1; mii.noseSize = 4; mii.noseVertical = 9
        mii.lipType = 23; mii.lipColor = 0; mii.lipSize = 4; mii.lipVertical = 13
        mii.mustacheType = 0; mii.beardType = 0; mii.facialHairColor = 0; mii.mustacheSize = 4; mii.mustacheVertical = 10
        mii.moleEnabled = false; mii.moleSize = 4; mii.moleVertical = 20; mii.moleHorizontal = 2
        mii.height = 63
        mii.weight = 63
        mii.miiId = 1
        return mii
    }

    static func female(_ name: String) -> Mii {
        var mii = base(name)
        mii.girl = true
        mii.hairType = 12
        mii.eyeType = 4
        mii.eyeRotation = 3
        return mii
    }

    static func male(_ name: String) -> Mii {
        var mii = base(name)
        mii.girl = false
        mii.hairType = 33
        mii.eyebrowType = 6
        return mii
    }

    static func random(_ name: String) -> Mii {
        var generator = SystemRandomNumberGenerator()
        return random(name, using: &generator)
    }

    static func random<G: RandomNumberGenerator>(_ name: String, using generator: inout G) -> Mii {
        func next(_ count: Int) -> Int { Int.random(in: 0..<count, using: &generator) }
        var mii = base(name)
        let hair = next(MiiRanges.hairColors)
        mii.girl = next(2) == 0
        mii.hairType = next(71)
        mii.hairColor = hair
        mii.hairFlipped = next(3) == 0
        mii.eyebrowType = next(23)
        mii.eyebrowColor = hair
        mii.eyeType = next(47)
        mii.eyeColor = next(MiiRanges.eyeColors)
        mii.favoriteColor = next(MiiRanges.favoriteColors)
        mii.faceShape = next(MiiRanges.faceShapes)
        mii.skinColor = next(MiiRanges.skinColors)
        mii.facialFeature = next(MiiRanges.facialFeatures)
        mii.noseType = next(MiiRanges.noseTypes)
        mii.lipType = next(23)
        mii.lipColor = next(MiiRanges.lipColors)
        mii.moleEnabled = next(4) == 0
        if next(4) == 0 {
            mii.glassesType = next(MiiRanges.glassesTypes)
            mii.glassesColor = next(MiiRanges.glassesColors)
        }
        if next(4) == 0 {
            mii.mustacheType = next(MiiRanges.mustacheTypes)
            mii.beardType = next(MiiRanges.beardTypes)
            mii.facialHairColor = hair
        }
        return mii
    }

    /// The editor's Randomize: a new look that keeps who the Mii is. The PC also resets the birthday
    /// and the mingle and downloaded flags, which the editor does not show; those are kept here.
    static func randomLook(of mii: Mii) -> Mii {
        var look = random(mii.name)
        look.favorite = mii.favorite
        look.miiId = mii.miiId
        look.systemId = mii.systemId
        look.creatorName = mii.creatorName
        look.birthMonth = mii.birthMonth
        look.birthDay = mii.birthDay
        look.mingleOff = mii.mingleOff
        look.downloaded = mii.downloaded
        return look
    }
}

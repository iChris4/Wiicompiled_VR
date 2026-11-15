// SPDX-License-Identifier: GPL-3.0-or-later

import CryptoKit
import Foundation

/// The licences of a Mario Kart Wii save, read as WheelWizard's GameLicenseService reads rksys.dat
/// (after kazuki-4ys' FaceThief and https://wiki.tockdom.com/wiki/Rksys.dat), by way of the Quest
/// launcher's RksysProfiles.kt: after the RKSD0006 magic come four RKPD blocks, one per licence, each
/// holding its Mii's name and ID, the profile ID its friend code derives from, its VR and BR, and race
/// counts. Retro Rewind keeps the ratings it plays with in Pulsar's RRRating.pul, which outranks the
/// save's own when it knows the profile. Nothing here writes the save.
enum RksysProfiles {
    static let slots = 4

    private static let magic = Array("RKSD0006".utf8)
    private static let licenseMagic = Array("RKPD".utf8)
    private static let licenseSize = 0x8CC0
    /// Where the licences end: the magic, then the four blocks. The rest of the save is not read.
    static let licensesEnd = 8 + slots * licenseSize

    private static let nameOffset = 0x14
    private static let nameChars = 10
    /// The Mii's ID in the Mii database, which the PC calls its avatar ID.
    private static let miiIdOffset = 0x28
    private static let profileIdOffset = 0x5C
    private static let vrOffset = 0xB0
    private static let brOffset = 0xB2
    private static let racesOffset = 0xB4
    private static let winsOffset = 0xDC

    private static let ratingMagic: UInt32 = 0x5252_5254 // "RRRT"
    private static let ratingVersion = 1
    private static let ratingEntries = 100
    private static let ratingEntrySize = 16
    private static let ratingHasData: UInt32 = 1

    /// One licence as the Profiles tab shows it. `friendCode` is empty for a licence never taken
    /// online, which has no profile ID yet.
    struct License: Equatable {
        let slot: Int
        let name: String
        let profileId: UInt32
        let friendCode: String
        let vr: Int
        let br: Int
        let races: UInt32
        let wins: UInt32
        /// The licence's Mii in the Mii database; a guest Mii's (0x80000001 and on) is in none.
        let miiId: UInt32
    }

    /// A rating from RRRating.pul, already as the game shows it (60.50 is 6050).
    struct Rating: Equatable {
        let vr: Int
        let br: Int
    }

    /// The four licence slots of `save`, an empty one nil, or nil when `save` is not a Mario Kart
    /// Wii save. `ratings` are RRRating.pul's, by profile ID.
    static func parse(_ save: [UInt8], ratings: [UInt32: Rating] = [:]) -> [License?]? {
        guard save.count >= licensesEnd, Array(save[0..<magic.count]) == magic else { return nil }
        return (0..<slots).map { slot in
            let base = magic.count + slot * licenseSize
            guard Array(save[base..<(base + licenseMagic.count)]) == licenseMagic else { return nil }
            let profileId = u32(save, base + profileIdOffset)
            let rating = profileId != 0 ? ratings[profileId] : nil
            return License(
                slot: slot,
                name: name(save, base + nameOffset),
                profileId: profileId,
                friendCode: friendCode(profileId),
                vr: rating?.vr ?? u16(save, base + vrOffset),
                br: rating?.br ?? u16(save, base + brOffset),
                races: u32(save, base + racesOffset),
                wins: u32(save, base + winsOffset),
                miiId: u32(save, base + miiIdOffset))
        }
    }

    /// FriendCodeGenerator.GetFriendCode: the profile ID with, above it, the top seven bits of the
    /// MD5 of the ID (little-endian) followed by "JCMR", written as three groups of four digits.
    /// Empty for ID 0.
    static func friendCode(_ profileId: UInt32) -> String {
        guard profileId != 0 else { return "" }
        let input: [UInt8] = [UInt8(profileId & 0xFF), UInt8((profileId >> 8) & 0xFF), UInt8((profileId >> 16) & 0xFF),
                              UInt8(profileId >> 24), 0x4A, 0x43, 0x4D, 0x52]
        let check = UInt64(Array(Insecure.MD5.hash(data: input))[0] >> 1)
        let digits = String(format: "%012llu", check << 32 | UInt64(profileId))
        let characters = Array(digits)
        return "\(String(characters[0..<4]))-\(String(characters[4..<8]))-\(String(characters[8...]))"
    }

    /// RRratingReader: RRRating.pul is "RRRT", version 1 and 100 entries of profile ID, VR and BR
    /// as floats, and flags. The PC multiplies by 100 and rounds half to even, as .NET's Math.Round.
    static func parseRatings(_ pul: [UInt8]) -> [UInt32: Rating] {
        guard pul.count >= 8 + ratingEntries * ratingEntrySize,
              u32(pul, 0) == ratingMagic, u16(pul, 4) == ratingVersion, u16(pul, 6) == ratingEntries else { return [:] }
        var ratings: [UInt32: Rating] = [:]
        for index in 0..<ratingEntries {
            let entry = 8 + index * ratingEntrySize
            let profileId = u32(pul, entry)
            guard u32(pul, entry + 12) & ratingHasData != 0, profileId != 0 else { continue }
            ratings[profileId] = Rating(vr: rating(pul, entry + 4), br: rating(pul, entry + 8))
        }
        return ratings
    }

    /// The float times 100 in single precision, as the Kotlin multiplies it, then rounded half to even.
    private static func rating(_ data: [UInt8], _ offset: Int) -> Int {
        let scaled = Float(bitPattern: u32(data, offset)) * 100
        let rounded = Double(scaled).rounded(.toNearestOrEven)
        return rounded.isFinite ? Int(max(min(rounded, Double(Int32.max)), Double(Int32.min))) : 0
    }

    /// The part of `file` the licences occupy, or nil when it is not there or too short to be a save.
    static func readLicenses(_ file: URL) -> [UInt8]? {
        guard let handle = try? FileHandle(forReadingFrom: file) else { return nil }
        defer { try? handle.close() }
        guard let data = try? handle.read(upToCount: licensesEnd), data.count == licensesEnd else { return nil }
        return [UInt8](data)
    }

    /// Ten UTF-16BE code units, cut at the first NUL and trimmed.
    private static func name(_ data: [UInt8], _ offset: Int) -> String {
        let units = (0..<nameChars).map { UInt16(data[offset + $0 * 2]) << 8 | UInt16(data[offset + $0 * 2 + 1]) }
        let text = String(decoding: units.prefix { $0 != 0 }, as: UTF16.self)
        return text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    private static func u16(_ data: [UInt8], _ offset: Int) -> Int { Int(data[offset]) << 8 | Int(data[offset + 1]) }

    private static func u32(_ data: [UInt8], _ offset: Int) -> UInt32 {
        UInt32(data[offset]) << 24 | UInt32(data[offset + 1]) << 16 | UInt32(data[offset + 2]) << 8 | UInt32(data[offset + 3])
    }
}

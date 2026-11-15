// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// Retro WFC, the server Retro Rewind plays online on, asked what WheelWizard asks it
/// (Features/RrRooms/IRwfcApi), by way of the Quest launcher's RetroWfc.kt: the rooms open now, a
/// player's profile, which carries the Mii they last played with, and their VR history. Also
/// WheelWizard's own badges (badges.json), by friend code.
///
/// Values are read with type checks as org.json reads them on the Quest: a JSON boolean is not a
/// number and a number is not a boolean, although JSONSerialization hands both over as NSNumber.
enum RetroWfc {
    private static let api = "https://rwfc.net/api"
    private static let roomStatusURL = "\(api)/roomstatus"
    private static let badgesURL = "https://raw.githubusercontent.com/TeamWheelWizard/WheelWizard-Data/main/badges.json"
    private static let timeout: TimeInterval = 20

    struct Failure: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    /// One VR change, at `time`, leaving the player at `total`.
    struct HistoryEntry: Equatable {
        let time: Date
        let change: Int
        let total: Int
    }

    /// RwfcPlayerVrHistoryResponse: the entries of a period, oldest first, and its totals.
    struct History: Equatable {
        let from: Date
        let to: Date
        let starting: Int
        let ending: Int
        let totalChange: Int
        let entries: [HistoryEntry]
    }

    /// WheelWizard's BadgeVariant, with the tip its Badge component shows.
    enum Badge: String, CaseIterable {
        case whWzDev = "WhWzDev"
        case rrDev = "RrDev"
        case translator = "Translator"
        case translatorLead = "TranslatorLead"
        case heart = "Heart"
        case firestarterGoldWinner = "Firestarter_GoldWinner"
        case firestarterSilverWinner = "Firestarter_SilverWinner"
        case firestarterBronzeWinner = "Firestarter_BronzeWinner"
        case summitShowdownGoldWinner = "SummitShowdown_GoldWinner"
        case summitShowdownSilverWinner = "SummitShowdown_SilverWinner"
        case summitShowdownBronzeWinner = "SummitShowdown_BronzeWinner"
        case leafstruckGoldWinner = "Leafstruck_GoldWinner"
        case leafstruckSilverWinner = "Leafstruck_SilverWinner"
        case leafstruckBronzeWinner = "Leafstruck_BronzeWinner"

        var tip: String {
            switch self {
            case .whWzDev: return "Wheel Wizard Developer (hiii!)"
            case .rrDev: return "Retro Rewind Developer"
            case .translator: return "Translator"
            case .translatorLead: return "Translator Lead"
            case .heart: return "Heart of the Community"
            case .firestarterGoldWinner: return "Firestarter Tournament Winner"
            case .firestarterSilverWinner, .firestarterBronzeWinner: return "Firestarter Tournament Runner-Up"
            case .summitShowdownGoldWinner: return "Summit Showdown Tournament Winner"
            case .summitShowdownSilverWinner, .summitShowdownBronzeWinner: return "Summit Showdown Tournament Runner-Up"
            case .leafstruckGoldWinner: return "Leafstruck Tournament Winner"
            case .leafstruckSilverWinner, .leafstruckBronzeWinner: return "Leafstruck Tournament Runner-Up"
            }
        }
    }

    /// A player in an open room (RwfcRoomStatusPlayer); `mii` is the 74 bytes of their Mii.
    struct RoomPlayer: Equatable {
        let pid: String
        let name: String
        let friendCode: String
        let vr: Int?
        let br: Int?
        let isOpenHost: Bool
        let isSuspended: Bool
        let mii: Data?
        let connectionMap: [String]
    }

    /// An open room (RwfcRoomStatusRoom).
    struct Room: Equatable {
        let id: String
        let type: String
        let created: Date
        let rk: String?
        let suspended: Bool
        let players: [RoomPlayer]
    }

    /// What Retro WFC last saw of a profile's Mii: its 74 bytes, and its own picture of it.
    struct PlayerMii: Equatable {
        let data: Data?
        let image: Data?
    }

    static func profileURL(_ friendCode: String) -> String {
        let encoded = friendCode.addingPercentEncoding(withAllowedCharacters: .alphanumerics.union(CharacterSet(charactersIn: "-._"))) ?? friendCode
        return "\(api)/leaderboard/player/\(encoded)"
    }

    static func historyURL(_ friendCode: String, days: Int) -> String { "\(profileURL(friendCode))/history?days=\(days)" }

    // MARK: Parsing

    /// The 74 bytes of the Mii a profile last played with (the Wii's RFLCharData, as the game sent
    /// it), or nil without one.
    static func parseMiiData(_ json: Data) throws -> Data? {
        miiBytes(text(try object(json), "miiData"))
    }

    /// The PNG of the player's Mii a profile carries (64 by 64 pixels), or nil without one.
    static func parseMiiImage(_ json: Data) throws -> Data? {
        guard let encoded = text(try object(json), "miiImageBase64"),
              let bytes = Data(base64Encoded: encoded, options: .ignoreUnknownCharacters), !bytes.isEmpty else { return nil }
        return bytes
    }

    static func parseHistory(_ json: Data) throws -> History {
        let root = try object(json)
        let entries = objects(root["history"]).compactMap { entry -> HistoryEntry? in
            guard let time = time(text(entry, "date")) else { return nil }
            return HistoryEntry(time: time, change: number(entry, "vrChange"), total: number(entry, "totalVR"))
        }.sorted { $0.time < $1.time }
        return History(
            from: time(text(root, "fromDate")) ?? entries.first?.time ?? Date(timeIntervalSince1970: 0),
            to: time(text(root, "toDate")) ?? entries.last?.time ?? Date(timeIntervalSince1970: 0),
            starting: number(root, "startingVR"),
            ending: number(root, "endingVR"),
            totalChange: number(root, "totalVRChange"),
            entries: entries)
    }

    /// The rooms open now (RwfcRoomStatusResponse). A room or player without its ID, or a room
    /// without its type or creation time, which the PC could not read either, is left out.
    static func parseRoomStatus(_ json: Data) throws -> [Room] {
        objects(try object(json)["rooms"]).compactMap { room -> Room? in
            guard let roomId = text(room, "id"), let type = text(room, "type"), let created = time(text(room, "created")) else { return nil }
            return Room(
                id: roomId,
                type: type,
                created: created,
                rk: text(room, "rk"),
                // The PC's "suspend"; Retro WFC now calls it "isSuspended".
                suspended: bool(room["isSuspended"] ?? room["suspend"]) ?? false,
                players: objects(room["players"]).compactMap { player -> RoomPlayer? in
                    guard let pid = id(player["pid"]) else { return nil }
                    // The Mii is one object, or was a list of them.
                    let mii = player["mii"] as? [String: Any] ?? objects(player["mii"]).first
                    return RoomPlayer(
                        pid: pid,
                        name: player["name"] as? String ?? "",
                        friendCode: player["friendCode"] as? String ?? "",
                        vr: integer(player["vr"]),
                        br: integer(player["br"]),
                        isOpenHost: bool(player["isOpenHost"]) ?? false,
                        isSuspended: bool(player["isSuspended"]) ?? false,
                        mii: miiBytes(mii.flatMap { text($0, "data") }),
                        connectionMap: (player["connectionMap"] as? [Any])?.map { value in
                            if value is NSNull { return "" }
                            if let string = value as? String { return string }
                            return integer(value).map(String.init) ?? "\(value)"
                        } ?? [])
                })
        }
    }

    /// WhWzDataSingletonService.LoadBadgesAsync: friend code to badges, unknown badge names left out.
    static func parseBadges(_ json: Data) throws -> [String: [Badge]] {
        try object(json).mapValues { value in
            (value as? [Any] ?? []).compactMap { ($0 as? String).flatMap(Badge.init(rawValue:)) }
        }
    }

    /// The 74 bytes of a Mii Retro WFC sent in Base64, or nil for none or anything shorter.
    private static func miiBytes(_ encoded: String?) -> Data? {
        guard let encoded, let bytes = Data(base64Encoded: encoded, options: .ignoreUnknownCharacters),
              bytes.count >= MiiData.size else { return nil }
        return bytes.prefix(MiiData.size)
    }

    private static func object(_ json: Data) throws -> [String: Any] {
        guard let root = try? JSONSerialization.jsonObject(with: json, options: [.fragmentsAllowed]) as? [String: Any] else {
            throw Failure(message: "Retro WFC sent something this launcher cannot read.")
        }
        return root
    }

    private static func objects(_ value: Any?) -> [[String: Any]] {
        (value as? [Any])?.compactMap { $0 as? [String: Any] } ?? []
    }

    private static func isBoolean(_ value: Any?) -> Bool {
        guard let number = value as? NSNumber else { return false }
        return CFGetTypeID(number) == CFBooleanGetTypeID()
    }

    private static func bool(_ value: Any?) -> Bool? {
        isBoolean(value) ? (value as? NSNumber)?.boolValue : nil
    }

    /// A JSON number as Kotlin's Number.toInt() takes it: truncated, a boolean being no number.
    private static func integer(_ value: Any?) -> Int? {
        guard let number = value as? NSNumber, !isBoolean(value) else { return nil }
        let double = number.doubleValue
        guard double.isFinite else { return 0 }
        if double == double.rounded(), abs(double) < 9e15 { return Int(Int32(truncatingIfNeeded: number.int64Value)) }
        return Int(Int32(max(min(double.rounded(.towardZero), Double(Int32.max)), Double(Int32.min))))
    }

    private static func number(_ object: [String: Any], _ key: String) -> Int { integer(object[key]) ?? 0 }

    private static func text(_ object: [String: Any], _ key: String) -> String? {
        guard let text = object[key] as? String, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return nil }
        return text
    }

    /// An ID Retro WFC sends as text, or might send as a number.
    private static func id(_ value: Any?) -> String? {
        if let text = value as? String { return text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? nil : text }
        guard let number = value as? NSNumber, !isBoolean(value) else { return nil }
        return String(number.int64Value)
    }

    /// Instant.parse: an ISO-8601 instant, its fraction of a second (up to nine digits) cut to the
    /// millisecond as toEpochMilli cuts it, with a Z or an hours and minutes offset.
    static func time(_ text: String?) -> Date? {
        guard let text else { return nil }
        let scalars = Array(text.utf8)
        func digits(_ from: Int, _ count: Int) -> Int? {
            guard from + count <= scalars.count else { return nil }
            var value = 0
            for byte in scalars[from..<(from + count)] {
                guard (0x30...0x39).contains(byte) else { return nil }
                value = value * 10 + Int(byte - 0x30)
            }
            return value
        }
        func character(_ at: Int, _ expected: Character) -> Bool {
            at < scalars.count && scalars[at] == expected.asciiValue
        }
        guard let year = digits(0, 4), character(4, "-"), let month = digits(5, 2), character(7, "-"),
              let day = digits(8, 2), character(10, "T") || character(10, "t"),
              let hour = digits(11, 2), character(13, ":"), let minute = digits(14, 2), character(16, ":"),
              let second = digits(17, 2),
              (1...12).contains(month), (1...31).contains(day), hour < 24, minute < 60, second < 60 else { return nil }
        var at = 19
        var millis = 0
        if character(at, ".") {
            at += 1
            let start = at
            while at < scalars.count, (0x30...0x39).contains(scalars[at]) { at += 1 }
            let count = at - start
            guard (1...9).contains(count) else { return nil }
            let fraction = Array(scalars[start..<at]) + Array(repeating: UInt8(0x30), count: max(0, 3 - count))
            millis = fraction.prefix(3).reduce(0) { $0 * 10 + Int($1 - 0x30) }
        }
        var offset = 0
        if character(at, "Z") || character(at, "z") {
            at += 1
        } else if character(at, "+") || character(at, "-") {
            let sign = character(at, "-") ? -1 : 1
            guard let hours = digits(at + 1, 2), character(at + 3, ":"), let minutes = digits(at + 4, 2) else { return nil }
            offset = sign * (hours * 3600 + minutes * 60)
            at += 6
        } else {
            return nil
        }
        guard at == scalars.count else { return nil }
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        guard let date = calendar.date(from: DateComponents(year: year, month: month, day: day, hour: hour, minute: minute, second: second)),
              calendar.component(.day, from: date) == day else { return nil }
        return date.addingTimeInterval(TimeInterval(millis) / 1000 - TimeInterval(offset))
    }

    // MARK: Network

    /// The Mii of `friendCode`'s profile, or nil when Retro WFC has never seen it.
    static func playerMii(_ friendCode: String) async throws -> PlayerMii? {
        guard let json = try await get(profileURL(friendCode), missingIsNil: true) else { return nil }
        return PlayerMii(data: try parseMiiData(json), image: try parseMiiImage(json))
    }

    static func history(_ friendCode: String, days: Int) async throws -> History {
        guard let json = try await get(historyURL(friendCode, days: days), missingIsNil: true) else {
            throw Failure(message: "Retro WFC has no VR history for \(friendCode).")
        }
        return try parseHistory(json)
    }

    static func roomStatus() async throws -> [Room] {
        try parseRoomStatus(try await get(roomStatusURL) ?? Data("{}".utf8))
    }

    static func badges() async throws -> [String: [Badge]] {
        try parseBadges(try await get(badgesURL) ?? Data("{}".utf8))
    }

    private static func get(_ url: String, missingIsNil: Bool = false) async throws -> Data? {
        guard let address = URL(string: url) else { throw Failure(message: "Invalid address \(url)") }
        var request = URLRequest(url: address, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: timeout)
        let version = Bundle.main.infoDictionary?["CFBundleShortVersionString"] as? String ?? "0"
        request.setValue("WiiCompiledVR-VisionPro/\(version)", forHTTPHeaderField: "User-Agent")
        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await URLSession.shared.data(for: request)
        } catch is CancellationError {
            throw CancellationError()
        } catch let error as URLError where error.code == .cancelled {
            throw CancellationError()
        } catch {
            throw Failure(message: "Retro WFC could not be reached. Check the headset's internet connection.")
        }
        let code = (response as? HTTPURLResponse)?.statusCode ?? 200
        if code == 404, missingIsNil { return nil }
        guard (200...299).contains(code) else { throw Failure(message: "\(address.host ?? url) answered \(code).") }
        return data
    }
}

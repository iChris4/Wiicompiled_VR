// SPDX-License-Identifier: GPL-3.0-or-later

import Foundation

/// The runtime's Config.toml held as lines, read and edited one flat `key = value` at a time so
/// every comment, unknown key and path the runtime, the in-headset settings panel or the player
/// put there survives. A port of the Quest launcher's TomlConfig.kt: edits follow
/// RuntimeConfigFile::WriteSetting (runtime/include/runtime_config.h) line for line, and the
/// typed readers accept only what the runtime's toml11 lookups accept, so a value the game would
/// ignore reads as absent here too and shows its default.
struct TomlConfig {
    private(set) var lines: [String]

    init(text: String) {
        var lines = text.components(separatedBy: "\n").map { $0.hasSuffix("\r") ? String($0.dropLast()) : $0 }
        if lines.last == "" { lines.removeLast() }
        self.lines = lines
    }

    var text: String { lines.isEmpty ? "" : lines.joined(separator: "\n") + "\n" }

    // MARK: Reading

    /// The value of section.key as written, without a trailing comment.
    func literal(_ section: String, _ key: String) -> String? {
        guard let found = findSection(section), let index = findKey(found, key) else { return nil }
        let code = Self.removeComment(lines[index])
        guard let equals = code.firstIndex(of: "=") else { return nil }
        let value = code[code.index(after: equals)...].trimmingCharacters(in: .whitespaces)
        return value.isEmpty ? nil : value
    }

    func bool(_ section: String, _ key: String) -> Bool? {
        switch literal(section, key) {
        case "true": return true
        case "false": return false
        default: return nil
        }
    }

    /// An integer literal; the runtime's integer keys reject floats.
    func integer(_ section: String, _ key: String) -> Int64? {
        guard let literal = literal(section, key), literal.wholeMatch(of: Self.integerPattern) != nil else { return nil }
        return Int64(literal.replacingOccurrences(of: "_", with: ""))
    }

    /// An integer or float literal, as FindConfigFloat accepts either.
    func number(_ section: String, _ key: String) -> Double? {
        guard let literal = literal(section, key),
              literal.wholeMatch(of: Self.integerPattern) != nil || literal.wholeMatch(of: Self.floatPattern) != nil,
              let value = Double(literal.replacingOccurrences(of: "_", with: "")), value.isFinite else { return nil }
        return value
    }

    func string(_ section: String, _ key: String) -> String? {
        literal(section, key).flatMap(Self.unquote)
    }

    // MARK: Writing

    mutating func set(_ section: String, _ key: String, literal: String) {
        let replacement = "\(key) = \(literal)"
        guard let found = findSection(section) else {
            if let last = lines.last, !last.isEmpty { lines.append("") }
            lines.append("[\(section)]")
            lines.append(replacement)
            return
        }
        if let index = findKey(found, key) {
            lines[index] = replacement
            return
        }
        // Above the blank line that separates this section from the next header, where a
        // reader would take the key for the next section's.
        var insertAt = found.end
        while insertAt > found.header + 1, lines[insertAt - 1].trimmingCharacters(in: .whitespaces).isEmpty {
            insertAt -= 1
        }
        lines.insert(replacement, at: insertAt)
    }

    mutating func setBool(_ section: String, _ key: String, _ value: Bool) { set(section, key, literal: value ? "true" : "false") }
    mutating func setInteger(_ section: String, _ key: String, _ value: Int64) { set(section, key, literal: String(value)) }
    mutating func setFloat(_ section: String, _ key: String, _ value: Double) { set(section, key, literal: Self.formatFloat(value)) }
    mutating func setString(_ section: String, _ key: String, _ value: String) { set(section, key, literal: Self.quote(value)) }

    // MARK: Structure

    private struct Section { let header: Int; let end: Int }

    private func findSection(_ section: String) -> Section? {
        var header = -1
        for (i, line) in lines.enumerated() {
            guard let name = Self.headerName(line) else { continue }
            if header >= 0 { return Section(header: header, end: i) }
            if name == section { header = i }
        }
        return header >= 0 ? Section(header: header, end: lines.count) : nil
    }

    private func findKey(_ section: Section, _ key: String) -> Int? {
        guard section.header + 1 < section.end else { return nil }
        for i in (section.header + 1)..<section.end {
            let code = Self.removeComment(lines[i]).trimmingCharacters(in: .whitespaces)
            if let equals = code.firstIndex(of: "="),
               code[..<equals].trimmingCharacters(in: .whitespaces) == key {
                return i
            }
        }
        return nil
    }

    // MARK: Lexing

    nonisolated(unsafe) private static let integerPattern = try! Regex(#"[+-]?\d(_?\d)*"#)
    nonisolated(unsafe) private static let floatPattern = try! Regex(#"[+-]?\d(_?\d)*(\.\d(_?\d)*)?([eE][+-]?\d(_?\d)*)?"#)

    /// The line up to a `#` that is not inside a quoted string.
    static func removeComment(_ line: String) -> String {
        var inSingle = false
        var inDouble = false
        var escaped = false
        for index in line.indices {
            let ch = line[index]
            if inDouble && ch == "\\" && !escaped {
                escaped = true
                continue
            }
            if ch == "'" && !inDouble {
                inSingle.toggle()
            } else if ch == "\"" && !inSingle && !escaped {
                inDouble.toggle()
            } else if ch == "#" && !inSingle && !inDouble {
                return String(line[..<index])
            }
            escaped = false
        }
        return line
    }

    private static func headerName(_ line: String) -> String? {
        let code = removeComment(line).trimmingCharacters(in: .whitespaces)
        guard code.count >= 2, code.first == "[", code.last == "]" else { return nil }
        return code.dropFirst().dropLast().trimmingCharacters(in: .whitespaces)
    }

    /// Three decimals at most, and always a decimal point so TOML reads a float.
    static func formatFloat(_ value: Double) -> String {
        var text = String(format: "%.3f", locale: Locale(identifier: "en_US_POSIX"), value)
        while text.hasSuffix("0") { text.removeLast() }
        if text.hasSuffix(".") { text += "0" }
        return text
    }

    static func quote(_ value: String) -> String {
        var result = "\""
        for scalar in value.unicodeScalars {
            switch scalar {
            case "\"": result += "\\\""
            case "\\": result += "\\\\"
            case "\n": result += "\\n"
            case "\r": result += "\\r"
            case "\t": result += "\\t"
            default:
                if scalar.value < 0x20 || scalar.value == 0x7f {
                    result += String(format: "\\u%04x", scalar.value)
                } else {
                    result.unicodeScalars.append(scalar)
                }
            }
        }
        return result + "\""
    }

    /// A TOML basic or literal string's content, or nil for anything else.
    static func unquote(_ literal: String) -> String? {
        if literal.count >= 2, literal.first == "'", literal.last == "'" {
            let content = String(literal.dropFirst().dropLast())
            return content.contains("'") ? nil : content
        }
        guard literal.count >= 2, literal.first == "\"", literal.last == "\"" else { return nil }
        let scalars = Array(literal.unicodeScalars.dropFirst().dropLast())
        var result = String.UnicodeScalarView()
        var i = 0
        while i < scalars.count {
            let ch = scalars[i]
            if ch == "\"" { return nil }
            if ch != "\\" {
                result.append(ch)
                i += 1
                continue
            }
            guard i + 1 < scalars.count else { return nil }
            let escape = scalars[i + 1]
            switch escape {
            case "b": result.append("\u{08}")
            case "t": result.append("\t")
            case "n": result.append("\n")
            case "f": result.append("\u{0C}")
            case "r": result.append("\r")
            case "\"": result.append("\"")
            case "\\": result.append("\\")
            case "u", "U":
                let digits = escape == "u" ? 4 : 8
                guard i + 2 + digits <= scalars.count else { return nil }
                var hex = String.UnicodeScalarView()
                hex.append(contentsOf: scalars[(i + 2)..<(i + 2 + digits)])
                guard let code = UInt32(String(hex), radix: 16), let scalar = Unicode.Scalar(code) else { return nil }
                result.append(scalar)
                i += digits
            default:
                return nil
            }
            i += 2
        }
        return String(result)
    }
}

package org.wiicompiled.quest.launcher

import java.math.BigDecimal
import java.math.RoundingMode
import java.util.Locale

/**
 * The runtime's Config.toml held as lines, read and edited one flat
 * `key = value` at a time so every comment, unknown key and path the runtime,
 * the in-headset settings panel or the player put there survives.
 *
 * Edits keep to the line rules RuntimeConfigFile::WriteSetting used before it
 * moved to re-emitting the whole document through toml11 (UpdateSettingText in
 * runtime/include/runtime_config.h, upstream v0.2.34): a present key is replaced
 * where it stands, a missing key goes after its section's last non-blank line,
 * and a missing section is appended. The runtime's writer keeps keys, order and
 * the comments attached to them but may lay the file out differently; either
 * side reads what the other wrote.
 * The typed readers accept only what the runtime's toml11 lookups accept, so a
 * value the game would ignore reads as absent here too and shows its default.
 */
class TomlConfig private constructor(private val lines: MutableList<String>) {

    private class Section(val header: Int, val end: Int)

    fun text(): String = if (lines.isEmpty()) "" else lines.joinToString("\n", postfix = "\n")

    /** The value of [section].[key] as written, without a trailing comment. */
    fun literal(section: String, key: String): String? {
        val found = findSection(section) ?: return null
        val index = findKey(found, key) ?: return null
        val code = removeComment(lines[index])
        return code.substring(code.indexOf('=') + 1).trim().ifEmpty { null }
    }

    fun bool(section: String, key: String): Boolean? = when (literal(section, key)) {
        "true" -> true
        "false" -> false
        else -> null
    }

    /** An integer literal; the runtime's integer keys reject floats. */
    fun integer(section: String, key: String): Long? {
        val literal = literal(section, key) ?: return null
        return if (INTEGER.matches(literal)) literal.replace("_", "").toLongOrNull() else null
    }

    /** An integer or float literal, as FindConfigFloat accepts either. */
    fun number(section: String, key: String): Double? {
        val literal = literal(section, key) ?: return null
        if (!INTEGER.matches(literal) && !FLOAT.matches(literal)) {
            return null
        }
        return literal.replace("_", "").toDoubleOrNull()?.takeIf { it.isFinite() }
    }

    fun string(section: String, key: String): String? = literal(section, key)?.let(::unquote)

    fun set(section: String, key: String, literal: String) {
        val replacement = "$key = $literal"
        val found = findSection(section)
        if (found == null) {
            if (lines.isNotEmpty() && lines.last().isNotEmpty()) {
                lines.add("")
            }
            lines.add("[$section]")
            lines.add(replacement)
            return
        }
        val index = findKey(found, key)
        if (index != null) {
            lines[index] = replacement
            return
        }
        // Keep the key above the blank line that separates this section from
        // the next header, where a reader would take it for the next section's.
        var insertAt = found.end
        while (insertAt > found.header + 1 && lines[insertAt - 1].trim().isEmpty()) {
            insertAt--
        }
        lines.add(insertAt, replacement)
    }

    fun setBool(section: String, key: String, value: Boolean) = set(section, key, value.toString())

    fun setInteger(section: String, key: String, value: Long) = set(section, key, value.toString())

    fun setFloat(section: String, key: String, value: Double) = set(section, key, formatFloat(value))

    fun setString(section: String, key: String, value: String) = set(section, key, quote(value))

    /**
     * Removes [section].[key] and the comment lines directly above it, which toml11
     * attaches to the key and RemoveSettingsText removes with it. Absent, nothing changes.
     */
    fun remove(section: String, key: String) {
        val found = findSection(section) ?: return
        val index = findKey(found, key) ?: return
        var first = index
        while (first > found.header + 1 && lines[first - 1].trim().startsWith("#")) {
            first--
        }
        for (i in index downTo first) {
            lines.removeAt(i)
        }
    }

    private fun findSection(section: String): Section? {
        var header = -1
        for (i in lines.indices) {
            val name = headerName(lines[i]) ?: continue
            if (header >= 0) {
                return Section(header, i)
            }
            if (name == section) {
                header = i
            }
        }
        return if (header >= 0) Section(header, lines.size) else null
    }

    private fun findKey(section: Section, key: String): Int? {
        for (i in section.header + 1 until section.end) {
            val code = removeComment(lines[i]).trim()
            val equals = code.indexOf('=')
            if (equals >= 0 && code.substring(0, equals).trim() == key) {
                return i
            }
        }
        return null
    }

    companion object {
        private val INTEGER = Regex("[+-]?\\d(_?\\d)*")
        private val FLOAT = Regex("[+-]?\\d(_?\\d)*(\\.\\d(_?\\d)*)?([eE][+-]?\\d(_?\\d)*)?")

        fun parse(text: String): TomlConfig {
            val lines = text.split('\n').map { it.removeSuffix("\r") }.toMutableList()
            if (lines.last().isEmpty()) {
                lines.removeAt(lines.size - 1)
            }
            return TomlConfig(lines)
        }

        /** The line up to a `#` that is not inside a quoted string. */
        fun removeComment(line: String): String {
            var inSingle = false
            var inDouble = false
            var escaped = false
            for (i in line.indices) {
                val ch = line[i]
                if (inDouble && ch == '\\' && !escaped) {
                    escaped = true
                    continue
                }
                when {
                    ch == '\'' && !inDouble -> inSingle = !inSingle
                    ch == '"' && !inSingle && !escaped -> inDouble = !inDouble
                    ch == '#' && !inSingle && !inDouble -> return line.substring(0, i)
                }
                escaped = false
            }
            return line
        }

        private fun headerName(line: String): String? {
            val code = removeComment(line).trim()
            if (code.length < 2 || code.first() != '[' || code.last() != ']') {
                return null
            }
            return code.substring(1, code.length - 1).trim()
        }

        /** Three decimals at most, and always a decimal point so TOML reads a float. */
        fun formatFloat(value: Double): String {
            val text = BigDecimal.valueOf(value).setScale(3, RoundingMode.HALF_UP).stripTrailingZeros().toPlainString()
            return if (text.contains('.')) text else "$text.0"
        }

        fun quote(value: String): String = buildString {
            append('"')
            for (ch in value) {
                when (ch) {
                    '"' -> append("\\\"")
                    '\\' -> append("\\\\")
                    '\n' -> append("\\n")
                    '\r' -> append("\\r")
                    '\t' -> append("\\t")
                    else -> if (ch < ' ' || ch == '') append(String.format(Locale.ROOT, "\\u%04x", ch.code)) else append(ch)
                }
            }
            append('"')
        }

        /** A TOML basic or literal string's content, or null for anything else. */
        fun unquote(literal: String): String? {
            if (literal.length >= 2 && literal.first() == '\'' && literal.last() == '\'') {
                val content = literal.substring(1, literal.length - 1)
                return if (content.contains('\'')) null else content
            }
            if (literal.length < 2 || literal.first() != '"' || literal.last() != '"') {
                return null
            }
            val result = StringBuilder()
            var i = 1
            val end = literal.length - 1
            while (i < end) {
                val ch = literal[i]
                if (ch == '"') {
                    return null
                }
                if (ch != '\\') {
                    result.append(ch)
                    i++
                    continue
                }
                if (i + 1 >= end) {
                    return null
                }
                when (val escape = literal[i + 1]) {
                    'b' -> result.append('\b')
                    't' -> result.append('\t')
                    'n' -> result.append('\n')
                    'f' -> result.append('')
                    'r' -> result.append('\r')
                    '"' -> result.append('"')
                    '\\' -> result.append('\\')
                    'u', 'U' -> {
                        val digits = if (escape == 'u') 4 else 8
                        if (i + 2 + digits > end) {
                            return null
                        }
                        val code = literal.substring(i + 2, i + 2 + digits).toIntOrNull(16) ?: return null
                        if (!Character.isValidCodePoint(code)) {
                            return null
                        }
                        result.appendCodePoint(code)
                        i += digits
                    }
                    else -> return null
                }
                i += 2
            }
            return result.toString()
        }
    }
}

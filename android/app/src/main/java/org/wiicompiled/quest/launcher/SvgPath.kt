package org.wiicompiled.quest.launcher

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.drawable.Drawable
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.tan

/**
 * SVG path data, as Wheel Wizard's status can carry an icon of its own (Avalonia's
 * Geometry.Parse on the PC): every command of the path mini-language, relative or absolute, with
 * arcs turned into cubic curves. [parse] is plain Kotlin, so it is tested apart from Android.
 */
object SvgPath {

    sealed class Segment {
        data class Move(val x: Float, val y: Float) : Segment()
        data class Line(val x: Float, val y: Float) : Segment()
        data class Cubic(val x1: Float, val y1: Float, val x2: Float, val y2: Float, val x: Float, val y: Float) : Segment()
        data class Quad(val x1: Float, val y1: Float, val x: Float, val y: Float) : Segment()
        object Close : Segment()
    }

    /** The path's segments in absolute coordinates; throws IllegalArgumentException on bad data. */
    fun parse(data: String): List<Segment> {
        val reader = Reader(data.trim().removePrefix("F0").removePrefix("F1"))
        val out = ArrayList<Segment>()
        var x = 0f
        var y = 0f
        var startX = 0f
        var startY = 0f
        // The last control point, for S and T, and which kind of curve set it.
        var controlX = 0f
        var controlY = 0f
        var last = ' '
        var command = ' '
        while (true) {
            reader.skipSeparators()
            if (reader.done) break
            val next = reader.peek()
            if (next.isLetter()) {
                command = next
                reader.advance()
            } else {
                require(command != ' ' && command.lowercaseChar() != 'z') { "Path data must start with a command" }
                // Numbers after a move are lines.
                if (command == 'M') command = 'L' else if (command == 'm') command = 'l'
            }
            val relative = command.isLowerCase()
            val baseX = if (relative) x else 0f
            val baseY = if (relative) y else 0f
            when (command.uppercaseChar()) {
                'M' -> {
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    startX = x
                    startY = y
                    out += Segment.Move(x, y)
                }
                'L' -> {
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    out += Segment.Line(x, y)
                }
                'H' -> {
                    x = baseX + reader.number()
                    out += Segment.Line(x, y)
                }
                'V' -> {
                    y = baseY + reader.number()
                    out += Segment.Line(x, y)
                }
                'C' -> {
                    val x1 = baseX + reader.number()
                    val y1 = baseY + reader.number()
                    controlX = baseX + reader.number()
                    controlY = baseY + reader.number()
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    out += Segment.Cubic(x1, y1, controlX, controlY, x, y)
                }
                'S' -> {
                    val x1 = if (last == 'C' || last == 'S') 2 * x - controlX else x
                    val y1 = if (last == 'C' || last == 'S') 2 * y - controlY else y
                    controlX = baseX + reader.number()
                    controlY = baseY + reader.number()
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    out += Segment.Cubic(x1, y1, controlX, controlY, x, y)
                }
                'Q' -> {
                    controlX = baseX + reader.number()
                    controlY = baseY + reader.number()
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    out += Segment.Quad(controlX, controlY, x, y)
                }
                'T' -> {
                    controlX = if (last == 'Q' || last == 'T') 2 * x - controlX else x
                    controlY = if (last == 'Q' || last == 'T') 2 * y - controlY else y
                    x = baseX + reader.number()
                    y = baseY + reader.number()
                    out += Segment.Quad(controlX, controlY, x, y)
                }
                'A' -> {
                    val rx = reader.number()
                    val ry = reader.number()
                    val rotation = reader.number()
                    val large = reader.flag()
                    val sweep = reader.flag()
                    val endX = baseX + reader.number()
                    val endY = baseY + reader.number()
                    arc(out, x, y, rx, ry, rotation, large, sweep, endX, endY)
                    x = endX
                    y = endY
                }
                'Z' -> {
                    out += Segment.Close
                    x = startX
                    y = startY
                }
                else -> throw IllegalArgumentException("Unknown path command $command")
            }
            last = command.uppercaseChar()
        }
        return out
    }

    /** The SVG spec's endpoint arc (appendix F.6), as cubic curves of at most a quarter turn each. */
    private fun arc(
        out: MutableList<Segment>,
        x0: Float, y0: Float, rxIn: Float, ryIn: Float, rotationDegrees: Float,
        large: Boolean, sweep: Boolean, x: Float, y: Float,
    ) {
        if (x0 == x && y0 == y) return
        var rx = abs(rxIn.toDouble())
        var ry = abs(ryIn.toDouble())
        if (rx == 0.0 || ry == 0.0) {
            out += Segment.Line(x, y)
            return
        }
        val phi = rotationDegrees * PI / 180
        val cosPhi = cos(phi)
        val sinPhi = sin(phi)
        val dx = (x0 - x) / 2.0
        val dy = (y0 - y) / 2.0
        val x1p = cosPhi * dx + sinPhi * dy
        val y1p = -sinPhi * dx + cosPhi * dy
        // Radii too small to reach the end grow until they do.
        val lambda = x1p * x1p / (rx * rx) + y1p * y1p / (ry * ry)
        if (lambda > 1) {
            rx *= sqrt(lambda)
            ry *= sqrt(lambda)
        }
        val numerator = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
        val denominator = rx * rx * y1p * y1p + ry * ry * x1p * x1p
        var factor = sqrt(maxOf(0.0, numerator / denominator))
        if (large == sweep) factor = -factor
        val cxp = factor * rx * y1p / ry
        val cyp = -factor * ry * x1p / rx
        val cx = cosPhi * cxp - sinPhi * cyp + (x0 + x) / 2.0
        val cy = sinPhi * cxp + cosPhi * cyp + (y0 + y) / 2.0
        val theta1 = atan2((y1p - cyp) / ry, (x1p - cxp) / rx)
        var delta = atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx) - theta1
        if (sweep && delta < 0) delta += 2 * PI else if (!sweep && delta > 0) delta -= 2 * PI

        val pieces = ceil(abs(delta) / (PI / 2)).toInt().coerceAtLeast(1)
        val step = delta / pieces
        val k = 4.0 / 3.0 * tan(step / 4)
        var angle = theta1
        repeat(pieces) {
            val cos1 = cos(angle)
            val sin1 = sin(angle)
            val cos2 = cos(angle + step)
            val sin2 = sin(angle + step)
            fun px(u: Double, v: Double) = (cx + rx * u * cosPhi - ry * v * sinPhi).toFloat()
            fun py(u: Double, v: Double) = (cy + rx * u * sinPhi + ry * v * cosPhi).toFloat()
            out += Segment.Cubic(
                px(cos1 - k * sin1, sin1 + k * cos1), py(cos1 - k * sin1, sin1 + k * cos1),
                px(cos2 + k * sin2, sin2 - k * cos2), py(cos2 + k * sin2, sin2 - k * cos2),
                px(cos2, sin2), py(cos2, sin2),
            )
            angle += step
        }
    }

    fun toPath(segments: List<Segment>): Path = Path().apply {
        segments.forEach { segment ->
            when (segment) {
                is Segment.Move -> moveTo(segment.x, segment.y)
                is Segment.Line -> lineTo(segment.x, segment.y)
                is Segment.Cubic -> cubicTo(segment.x1, segment.y1, segment.x2, segment.y2, segment.x, segment.y)
                is Segment.Quad -> quadTo(segment.x1, segment.y1, segment.x, segment.y)
                Segment.Close -> close()
            }
        }
    }

    /** A PathIcon: the path filled in [color], scaled to fit its bounds and centred, as Avalonia stretches it. */
    class IconDrawable(private val source: Path, color: Int) : Drawable() {
        private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply { this.color = color }
        private val fitted = Path()

        override fun onBoundsChange(bounds: android.graphics.Rect) {
            val extent = RectF().also { source.computeBounds(it, true) }
            fitted.reset()
            if (extent.width() <= 0f || extent.height() <= 0f || bounds.isEmpty) return
            val scale = minOf(bounds.width() / extent.width(), bounds.height() / extent.height())
            val matrix = Matrix().apply {
                setTranslate(-extent.centerX(), -extent.centerY())
                postScale(scale, scale)
                postTranslate(bounds.exactCenterX(), bounds.exactCenterY())
            }
            source.transform(matrix, fitted)
        }

        override fun draw(canvas: Canvas) = canvas.drawPath(fitted, paint)
        override fun setAlpha(alpha: Int) {
            paint.alpha = alpha
        }
        override fun setColorFilter(colorFilter: ColorFilter?) {
            paint.colorFilter = colorFilter
        }
        @Deprecated("Deprecated in Java")
        override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
    }

    private class Reader(private val text: String) {
        private var index = 0
        val done: Boolean get() = index >= text.length

        fun peek(): Char = text[index]
        fun advance() {
            index++
        }

        fun skipSeparators() {
            while (index < text.length && (text[index].isWhitespace() || text[index] == ',')) index++
        }

        fun number(): Float {
            skipSeparators()
            val start = index
            if (index < text.length && (text[index] == '+' || text[index] == '-')) index++
            var dot = false
            while (index < text.length) {
                val c = text[index]
                when {
                    c.isDigit() -> index++
                    c == '.' && !dot -> {
                        dot = true
                        index++
                    }
                    (c == 'e' || c == 'E') && index > start -> {
                        index++
                        if (index < text.length && (text[index] == '+' || text[index] == '-')) index++
                    }
                    else -> break
                }
            }
            require(index > start) { "Expected a number at $start" }
            return text.substring(start, index).toFloat()
        }

        /** Arc flags are one digit each, and may run together ("a5 5 0 015 5"). */
        fun flag(): Boolean {
            skipSeparators()
            require(index < text.length && (text[index] == '0' || text[index] == '1')) { "Expected an arc flag at $index" }
            return text[index++] == '1'
        }
    }
}

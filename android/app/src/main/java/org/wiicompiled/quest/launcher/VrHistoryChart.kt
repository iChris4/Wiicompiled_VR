package org.wiicompiled.quest.launcher

import android.content.Context
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PointF
import android.graphics.RectF
import android.graphics.Shader
import android.util.AttributeSet
import android.view.View
import org.wiicompiled.quest.R

/**
 * The plot of WheelWizard's VrHistoryGraph: a dark panel with three lines across and three down,
 * the area under the VR line in a fading primary gradient, and the line itself. Points are given
 * from 0 to 1 on both axes, 0 being the left and the lowest VR.
 */
class VrHistoryChart(context: Context, attrs: AttributeSet? = null) : View(context, attrs) {

    private val density = resources.displayMetrics.density
    private val panel = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = context.getColor(R.color.neutral_950) }
    private val panelEdge = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = density
        color = context.getColor(R.color.neutral_900)
    }
    private val grid = Paint().apply {
        strokeWidth = density
        color = context.getColor(R.color.neutral_800)
    }
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeWidth = 2 * density
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
        color = context.getColor(R.color.primary_400)
    }
    private val area = Paint(Paint.ANTI_ALIAS_FLAG).apply { alpha = (0.30f * 255).toInt() }

    private var points: List<PointF> = emptyList()

    fun setPoints(normalized: List<PointF>) {
        points = normalized
        invalidate()
    }

    override fun onDraw(canvas: Canvas) {
        val bounds = RectF(0f, 0f, width.toFloat(), height.toFloat())
        val corner = 6 * density
        canvas.drawRoundRect(bounds, corner, corner, panel)
        canvas.drawRoundRect(bounds.apply { inset(density / 2, density / 2) }, corner, corner, panelEdge)

        val inner = RectF(8 * density, 8 * density, width - 8 * density, height - 8 * density)
        if (inner.width() <= 0 || inner.height() <= 0) return
        for (fraction in listOf(0f, 0.5f, 1f)) {
            val y = inner.top + inner.height() * fraction
            canvas.drawLine(inner.left, y, inner.right, y, grid)
            val x = inner.left + inner.width() * fraction
            canvas.drawLine(x, inner.top, x, inner.bottom, grid)
        }
        if (points.isEmpty()) return

        fun at(point: PointF) = PointF(inner.left + point.x * inner.width(), inner.bottom - point.y * inner.height())
        val stroke = Path()
        val fill = Path().apply { moveTo(inner.left, inner.bottom) }
        points.map(::at).forEachIndexed { index, point ->
            if (index == 0) stroke.moveTo(point.x, point.y) else stroke.lineTo(point.x, point.y)
            fill.lineTo(point.x, point.y)
        }
        fill.lineTo(inner.right, inner.bottom)
        fill.close()
        area.shader = LinearGradient(
            0f, inner.top, 0f, inner.bottom,
            context.getColor(R.color.primary_300), context.getColor(R.color.primary_700), Shader.TileMode.CLAMP,
        )
        canvas.drawPath(fill, area)
        canvas.drawPath(stroke, line)
    }
}

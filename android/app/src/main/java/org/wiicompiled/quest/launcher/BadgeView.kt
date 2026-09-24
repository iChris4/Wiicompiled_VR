package org.wiicompiled.quest.launcher

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.View
import org.wiicompiled.quest.R

/**
 * One of WheelWizard's badges (Views/Components/Badge.axaml), drawn in proportion to its size. A
 * role badge is two discs with an icon; a tournament medal is a small disc behind the Award ribbon,
 * turned 21 degrees, in gold, silver or bronze. The tip shows when the pointer rests on it.
 */
class BadgeView(context: Context, private val badge: RetroWfc.Badge) : View(context) {

    private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

    init {
        tooltipText = badge.tip
        contentDescription = badge.tip
    }

    override fun onDraw(canvas: Canvas) {
        val size = minOf(width, height).toFloat()
        if (size <= 0f) return
        canvas.save()
        canvas.translate((width - size) / 2f, (height - size) / 2f)
        when (badge) {
            RetroWfc.Badge.WhWzDev -> role(canvas, size, res(R.color.primary_100), res(R.color.primary_400), R.drawable.ic_code, res(R.color.primary_900))
            RetroWfc.Badge.RrDev -> role(canvas, size, res(R.color.warning_100), res(R.color.warning_500), R.drawable.ic_code, res(R.color.warning_800))
            RetroWfc.Badge.Translator -> role(canvas, size, 0xFFE9C7FF.toInt(), 0xFF8A01CB.toInt(), R.drawable.ic_translation, 0xFF400C55.toInt())
            RetroWfc.Badge.TranslatorLead -> {
                discs(canvas, size, 0xFFFFC7FB.toInt(), 0xFFCB01AE.toInt())
                // The translation icon sits low and right, with a small star above it on the left.
                icon(canvas, R.drawable.ic_translation, 0xFF550C48.toInt(), RectF(size * 3 / 14, size * 2 / 14, size * 12 / 14, size * 11 / 14))
                icon(canvas, R.drawable.ic_star, 0xFF550C48.toInt(), RectF(size * 1.5f / 14, size * 5.5f / 14, size * 5.5f / 14, size * 8.5f / 14))
            }
            RetroWfc.Badge.Heart -> {
                discs(canvas, size, 0xFFA7FCFF.toInt(), 0xFFF0F0EC.toInt())
                icon(canvas, R.drawable.ic_heart, 0xFF72F2FF.toInt(), RectF(size / 4, size * 0.3f, size * 3 / 4, size * 0.8f))
            }
            RetroWfc.Badge.Firestarter_GoldWinner,
            RetroWfc.Badge.SummitShowdown_GoldWinner,
            RetroWfc.Badge.Leafstruck_GoldWinner -> medal(canvas, size, res(R.color.warning_100), res(R.color.warning_600))
            RetroWfc.Badge.Firestarter_SilverWinner,
            RetroWfc.Badge.SummitShowdown_SilverWinner,
            RetroWfc.Badge.Leafstruck_SilverWinner -> medal(canvas, size, res(R.color.neutral_50), res(R.color.neutral_400))
            RetroWfc.Badge.Firestarter_BronzeWinner,
            RetroWfc.Badge.SummitShowdown_BronzeWinner,
            RetroWfc.Badge.Leafstruck_BronzeWinner -> medal(canvas, size, 0xFFFFD19D.toInt(), 0xFFEC5616.toInt())
        }
        canvas.restore()
    }

    /** A 14-unit grid: the outer disc fills it, the inner one leaves a unit, the icon two. */
    private fun role(canvas: Canvas, size: Float, outer: Int, inner: Int, icon: Int, fill: Int) {
        discs(canvas, size, outer, inner)
        icon(canvas, icon, fill, RectF(size * 2 / 14, size * 2 / 14, size * 12 / 14, size * 12 / 14))
    }

    private fun discs(canvas: Canvas, size: Float, outer: Int, inner: Int) {
        paint.color = outer
        canvas.drawCircle(size / 2, size / 2, size / 2, paint)
        paint.color = inner
        canvas.drawCircle(size / 2, size / 2, size * 6 / 14, paint)
    }

    /** Rows of 1, 2 and 2 fifths and columns of 1, 2 and 1 quarters: the disc takes the middle cell. */
    private fun medal(canvas: Canvas, size: Float, disc: Int, ribbon: Int) {
        paint.color = disc
        val cell = RectF(size / 4, size / 5, size * 3 / 4, size * 3 / 5)
        val radius = minOf(cell.width(), cell.height()) / 2
        canvas.drawRoundRect(cell, radius, radius, paint)
        canvas.save()
        canvas.rotate(21f, size / 2, size / 2)
        icon(canvas, R.drawable.ic_award, ribbon, RectF(0f, 0f, size, size))
        canvas.restore()
    }

    private fun icon(canvas: Canvas, drawable: Int, tint: Int, box: RectF) {
        val icon = context.getDrawable(drawable)?.mutate() ?: return
        icon.setTint(tint)
        icon.setBounds(box.left.toInt(), box.top.toInt(), box.right.toInt(), box.bottom.toInt())
        icon.draw(canvas)
    }

    private fun res(color: Int): Int = context.getColor(color)
}

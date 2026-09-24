package org.wiicompiled.quest.launcher

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.LinearLayout

/**
 * A list row that takes every tap itself. A view with a tooltip, or a badge strip that scrolls,
 * would otherwise swallow a tap on it; the pointer's hover still reaches them, so their tips show.
 * Only a button inside the row, a clickable view such as the leaderboard's View Room, keeps the
 * taps that begin on it.
 */
class TapRow(context: Context, attrs: AttributeSet?) : LinearLayout(context, attrs) {
    private val taps = Taps(this)

    override fun onInterceptTouchEvent(event: MotionEvent): Boolean = taps.intercept(event)
}

/** A [TapRow] whose children are layered, such as the Friends page's cards. */
class TapFrame(context: Context, attrs: AttributeSet?) : FrameLayout(context, attrs) {
    private val taps = Taps(this)

    override fun onInterceptTouchEvent(event: MotionEvent): Boolean = taps.intercept(event)
}

private class Taps(private val row: ViewGroup) {
    /** The gesture began on a button inside the row, which then has all of it. */
    private var onButton = false

    fun intercept(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) onButton = buttonAt(row, event.x, event.y)
        return !onButton
    }

    private fun buttonAt(group: ViewGroup, x: Float, y: Float): Boolean {
        for (index in group.childCount - 1 downTo 0) {
            val child = group.getChildAt(index)
            if (child.visibility != View.VISIBLE) continue
            val childX = x + group.scrollX - child.left
            val childY = y + group.scrollY - child.top
            if (childX < 0 || childY < 0 || childX >= child.width || childY >= child.height) continue
            if (child.isClickable && child.isEnabled) return true
            if (child is ViewGroup && buttonAt(child, childX, childY)) return true
        }
        return false
    }
}

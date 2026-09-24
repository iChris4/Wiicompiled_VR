package org.wiicompiled.quest.launcher

import android.content.Context
import android.util.AttributeSet
import android.view.MotionEvent
import android.widget.LinearLayout

/**
 * A list row that takes every tap itself. A view with a tooltip, or a badge strip that scrolls,
 * would otherwise swallow a tap on it; the pointer's hover still reaches them, so their tips show.
 */
class TapRow(context: Context, attrs: AttributeSet?) : LinearLayout(context, attrs) {
    override fun onInterceptTouchEvent(event: MotionEvent): Boolean = true
}

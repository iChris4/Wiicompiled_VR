package org.wiicompiled.quest.launcher

import android.content.Context
import android.util.AttributeSet
import android.view.View
import android.view.ViewGroup
import kotlin.math.max

/**
 * Children in rows of equal cells (WheelWizard's UniformGrid): a fixed number of [columns], or
 * with [columns] at 0 as many cells of [cellWidth] as fit, the rows then centred. A child is as
 * tall as its layout height, or its content, and each row as its tallest child.
 */
class TileGrid @JvmOverloads constructor(context: Context, attrs: AttributeSet? = null) : ViewGroup(context, attrs) {
    var columns = 0
    var cellWidth = 0
    var spacing = 0

    private var measuredColumns = 1
    private var measuredCell = 0

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val width = MeasureSpec.getSize(widthMeasureSpec) - paddingLeft - paddingRight
        measuredColumns = if (columns > 0) columns else max(1, (width + spacing) / (cellWidth + spacing))
        measuredCell = if (columns > 0) max(0, (width - spacing * (measuredColumns - 1)) / measuredColumns) else cellWidth
        val cellSpec = MeasureSpec.makeMeasureSpec(measuredCell, MeasureSpec.EXACTLY)
        var total = 0
        var rowHeight = 0
        var visible = 0
        for (i in 0 until childCount) {
            val child = getChildAt(i)
            if (child.visibility == View.GONE) continue
            val height = child.layoutParams?.height ?: LayoutParams.WRAP_CONTENT
            val heightSpec = if (height >= 0) {
                MeasureSpec.makeMeasureSpec(height, MeasureSpec.EXACTLY)
            } else {
                MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED)
            }
            child.measure(cellSpec, heightSpec)
            rowHeight = max(rowHeight, child.measuredHeight)
            visible++
            if (visible % measuredColumns == 0) {
                total += rowHeight + spacing
                rowHeight = 0
            }
        }
        if (visible % measuredColumns != 0) total += rowHeight else if (visible > 0) total -= spacing
        setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), total + paddingTop + paddingBottom)
    }

    override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
        val used = measuredColumns * measuredCell + (measuredColumns - 1) * spacing
        val left = paddingLeft + if (columns > 0) 0 else max(0, (r - l - paddingLeft - paddingRight - used) / 2)
        var top = paddingTop
        var column = 0
        var rowHeight = 0
        for (i in 0 until childCount) {
            val child = getChildAt(i)
            if (child.visibility == View.GONE) continue
            val x = left + column * (measuredCell + spacing)
            child.layout(x, top, x + child.measuredWidth, top + child.measuredHeight)
            rowHeight = max(rowHeight, child.measuredHeight)
            column++
            if (column == measuredColumns) {
                column = 0
                top += rowHeight + spacing
                rowHeight = 0
            }
        }
    }
}

package org.wiicompiled.quest.launcher

import android.annotation.SuppressLint
import android.app.Activity
import android.app.AlertDialog
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.ImageView
import android.widget.TextView
import org.wiicompiled.quest.R

/**
 * WheelWizard's MiiCarouselWindow with its Mii3DRender: the whole Mii, which dragging turns (left
 * and right about the Mii, up and down about the camera) and the thumbstick brings nearer or
 * further. While it moves it is drawn small, then sharp once it rests, as the PC does.
 */
class MiiViewDialog private constructor(private val activity: Activity, private val mii: Mii) {

    private val view = activity.layoutInflater.inflate(R.layout.dialog_mii_view, null)
    private val image: ImageView = view.findViewById(R.id.mii_view_image)
    private val loading: View = view.findViewById(R.id.mii_view_loading)
    private val message: TextView = view.findViewById(R.id.mii_view_message)
    private val density = activity.resources.displayMetrics.density
    private val size = (PICTURE_DP * density).toInt() and 1.inv()
    private val previewSize = (size / 4) and 1.inv()
    private lateinit var dialog: AlertDialog

    private var yaw = 0f
    private var pitch = 0f
    private var zoom = 1f
    private var lastX = 0f
    private var lastY = 0f
    private val settle = Runnable { draw(size) }

    @SuppressLint("ClickableViewAccessibility")
    private fun open() {
        dialog = AlertDialog.Builder(activity)
            .setTitle(mii.name)
            .setView(view)
            .setPositiveButton(R.string.dialog_close, null)
            .create()
        dialog.setOnDismissListener { image.removeCallbacks(settle) }
        dialog.show()
        dialog.window?.setLayout(PatchesWidgets.dp(activity, 520), ViewGroup.LayoutParams.WRAP_CONTENT)
        if (!MiiRenderResource.installed(activity)) {
            loading.visibility = View.GONE
            message.visibility = View.VISIBLE
            message.setText(R.string.mii_view_no_parts)
            return
        }
        image.setOnTouchListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    lastX = event.x
                    lastY = event.y
                }
                MotionEvent.ACTION_MOVE -> {
                    // Mii3DRender turns 0.8 degrees per pixel dragged, in the PC's device-independent pixels.
                    yaw += (event.x - lastX) / density * DRAG_DEGREES
                    pitch += (event.y - lastY) / density * DRAG_DEGREES
                    lastX = event.x
                    lastY = event.y
                    draw(previewSize)
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> draw(size)
            }
            true
        }
        image.setOnGenericMotionListener { _, event ->
            if (event.actionMasked != MotionEvent.ACTION_SCROLL) return@setOnGenericMotionListener false
            val steps = event.getAxisValue(MotionEvent.AXIS_VSCROLL)
            if (steps == 0f) return@setOnGenericMotionListener false
            zoom = (zoom - steps * ZOOM_STEP).coerceIn(MIN_ZOOM, MAX_ZOOM)
            draw(previewSize)
            image.removeCallbacks(settle)
            image.postDelayed(settle, SETTLE_MS)
            true
        }
        draw(size)
    }

    private fun draw(pixels: Int) {
        if (pixels == size) image.removeCallbacks(settle)
        MiiImages.view(activity, mii, pixels, MiiRenderer.Pose(0f, yaw, 0f, pitch, 0f, 0f, zoom)) { bitmap ->
            if (!dialog.isShowing) return@view
            loading.visibility = View.GONE
            if (bitmap != null) {
                image.setImageBitmap(bitmap)
            } else if (image.drawable == null) {
                message.visibility = View.VISIBLE
                message.setText(R.string.mii_view_no_parts)
            }
        }
    }

    companion object {
        /** The picture's height in dialog_mii_view.xml. */
        private const val PICTURE_DP = 300
        private const val DRAG_DEGREES = 0.8f
        private const val ZOOM_STEP = 0.1f
        private const val MIN_ZOOM = 0.35f
        private const val MAX_ZOOM = 1.5f
        /** Mii3DRender's HighQualitySettleDelayMs. */
        private const val SETTLE_MS = 90L

        fun show(activity: Activity, mii: Mii) = MiiViewDialog(activity, mii).open()
    }
}

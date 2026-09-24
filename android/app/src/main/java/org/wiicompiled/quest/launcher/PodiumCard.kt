package org.wiicompiled.quest.launcher

import android.animation.ValueAnimator
import android.app.Activity
import android.graphics.drawable.GradientDrawable
import android.view.MotionEvent
import android.view.View
import android.view.animation.LinearInterpolator
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import org.wiicompiled.quest.R

/**
 * A step of the leaderboard's podium, WheelWizard's LeaderboardPodiumCard (view_podium_card.xml):
 * the player on a card in their medal's colours. As on the PC it rises in when it is filled, then
 * floats, glows, glints and sparkles while the page is shown, and grows a little under the pointer.
 */
class PodiumCard(
    private val activity: Activity,
    val root: View,
    medal: Medal,
    avatarDp: Int,
    /** The PC's EntryFirst, EntrySecond and EntryThird start 80 ms apart. */
    private val entryDelayMs: Long,
    /** FloatA, FloatB or FloatC: how long the card takes to rise and settle, and from where to where. */
    private val drift: Drift,
) {
    /** A medal's border and the two ends of its card's diagonal gradient (#AARRGGBB, as the PC's). */
    enum class Medal(val border: Int, val from: Int, val to: Int) {
        Gold(0xFFEEC21E.toInt(), 0x2E341B07, 0x2D3A2710),
        Silver(0x90A9B8D7.toInt(), 0x27363A42, 0x29313847),
        Bronze(0xA8D77E47.toInt(), 0x2C3C2418, 0x2B4C2A1D),
    }

    /** An animation of [durationMs] from [from] to [to] and back, in dp. */
    class Drift(val durationMs: Long, val from: Float, val to: Float)

    private val surface: View = root.findViewById(R.id.podium_surface)
    private val aura: View = root.findViewById(R.id.podium_aura)
    private val shimmer: View = root.findViewById(R.id.podium_shimmer)
    private val particles: List<View> = listOf(
        root.findViewById(R.id.podium_particle_a),
        root.findViewById(R.id.podium_particle_b),
        root.findViewById(R.id.podium_particle_c),
    )
    private val picture: ImageView = root.findViewById(R.id.podium_mii)
    private val placeholder: View = root.findViewById(R.id.podium_placeholder)
    private val badgeBox: FrameLayout = root.findViewById(R.id.podium_badge)
    private val miiSize = (2 * avatarDp * activity.resources.displayMetrics.density).toInt() and 1.inv()

    private val loops = ArrayList<ValueAnimator>()
    private var entry: ValueAnimator? = null

    init {
        val density = activity.resources.displayMetrics.density
        surface.background = GradientDrawable(GradientDrawable.Orientation.TL_BR, intArrayOf(medal.from, medal.to)).apply {
            cornerRadius = 20 * density
            setStroke(density.toInt().coerceAtLeast(1), medal.border)
        }
        // The PC blurs a circle of the border's colour by 70 pixels; a radial fade looks the same.
        aura.background = GradientDrawable().apply {
            shape = GradientDrawable.OVAL
            gradientType = GradientDrawable.RADIAL_GRADIENT
            gradientRadius = 100 * density
            colors = intArrayOf(medal.border, withAlpha(medal.border, 0.4f), withAlpha(medal.border, 0f))
        }
        root.findViewById<View>(R.id.podium_avatar).layoutParams.apply {
            width = (avatarDp * density).toInt()
            height = width
        }
        // Hovered: scale(1.03) over 0.22 s, CircularEaseOut.
        root.setOnHoverListener { _, event ->
            when (event.actionMasked) {
                MotionEvent.ACTION_HOVER_ENTER -> scaleSurface(1.03f)
                MotionEvent.ACTION_HOVER_EXIT -> scaleSurface(1f)
            }
            false
        }
    }

    /** Shows [row], or nothing in its place (the PC's HasPodiumFirst and so on). */
    fun fill(row: Leaderboard.Row?, name: String, place: String, vrText: String, badge: RetroWfc.Badge?) {
        if (row == null) {
            root.visibility = View.INVISIBLE
            return
        }
        root.visibility = View.VISIBLE
        root.findViewById<TextView>(R.id.podium_rank).text = activity.getString(R.string.leaderboard_rank, row.rank)
        root.findViewById<TextView>(R.id.podium_place).text = place
        root.findViewById<TextView>(R.id.podium_name).text = name
        root.findViewById<TextView>(R.id.podium_vr).text = vrText
        root.findViewById<View>(R.id.podium_suspicious).visibility = if (row.isSuspicious) View.VISIBLE else View.GONE
        showBadge(badge)
        val mii = row.mii
        val drawn = mii != null && MiiRenderResource.installed(activity)
        placeholder.visibility = if (drawn) View.GONE else View.VISIBLE
        if (drawn) {
            MiiImages.head(activity, picture, mii!!, miiSize)
        } else {
            picture.setTag(R.id.mii_image_key, null)
            picture.setImageDrawable(null)
        }
    }

    /** The first of the player's badges, in a circle at the foot of the Mii. */
    fun showBadge(badge: RetroWfc.Badge?) {
        badgeBox.removeAllViews()
        badgeBox.visibility = if (badge == null) View.GONE else View.VISIBLE
        if (badge != null) badgeBox.addView(BadgeView(activity, badge), FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    }

    /** Rises in (0.45 s, CircularEaseOut, 30 px up while fading in), then keeps moving. */
    fun enter() {
        stop()
        root.alpha = 0f
        root.translationY = dp(30f)
        entry = ValueAnimator.ofFloat(0f, 1f).apply {
            duration = ENTRY_MS
            startDelay = entryDelayMs
            interpolator = LinearInterpolator()
            addUpdateListener {
                val progress = circularOut(it.animatedFraction)
                root.alpha = progress
                root.translationY = dp(30f * (1 - progress))
            }
            start()
        }
        start()
    }

    /** The endless animations, while the page is shown. */
    fun start() {
        if (loops.isNotEmpty()) return
        // FloatA/B/C: the card's surface rises and settles.
        loop(drift.durationMs, ::sineInOut) { p -> surface.translationY = dp(between(drift.from, drift.to, there(p))) }
        // The aura's opacity breathes between 0.2 and 0.34; the PC also turns it, which a round
        // glow of one colour does not show.
        loop(4_400, ::sineInOut) { p -> aura.alpha = between(0.2f, 0.34f, there(p)) }
        // The glint shows between 20% and 80% of the way across. The PC's band, 220 px long, sweeps
        // from 90 px left to 160 px right of its 146-px cards and spills over them; the Quest's
        // cards are wider and taller and clip it, so the band is stretched along its length to
        // reach the card's top and bottom, and crosses from beyond one side to beyond the other.
        loop(3_800, ::circularInOut) { p ->
            if (surface.width == 0) return@loop
            val length = surface.height / cos(TILT) + dp(40f)
            shimmer.scaleY = length / shimmer.height.coerceAtLeast(1)
            val reach = (length * sin(TILT) + shimmer.width * cos(TILT)) / 2
            shimmer.translationX = -reach - shimmer.width / 2f + (surface.width + 2 * reach) * p
            shimmer.alpha = 0.27f * when {
                p < 0.2f -> p / 0.2f
                p > 0.8f -> (1 - p) / 0.2f
                else -> 1f
            }
        }
        // Three sparkles, each rising and brightening at its own pace.
        sparkle(particles[0], 3_200, 0.35f, 0.95f, 8f)
        sparkle(particles[1], 2_900, 0.2f, 0.8f, 7f)
        sparkle(particles[2], 3_700, 0.25f, 0.85f, 6f)
    }

    fun stop() {
        entry?.cancel()
        entry = null
        loops.forEach { it.cancel() }
        loops.clear()
        root.alpha = 1f
        root.translationY = 0f
    }

    private fun sparkle(view: View, durationMs: Long, dim: Float, bright: Float, rise: Float) =
        loop(durationMs, ::sineInOut) { p ->
            view.alpha = between(dim, bright, there(p))
            view.translationY = dp(-rise * there(p))
        }

    /** Avalonia eases a whole iteration, then goes between its key frames in a straight line. */
    private fun loop(durationMs: Long, easing: (Float) -> Float, apply: (Float) -> Unit) {
        loops += ValueAnimator.ofFloat(0f, 1f).apply {
            duration = durationMs
            repeatCount = ValueAnimator.INFINITE
            interpolator = LinearInterpolator()
            addUpdateListener { apply(easing(it.animatedFraction)) }
            start()
        }
    }

    private fun scaleSurface(scale: Float) {
        surface.animate().scaleX(scale).scaleY(scale).setDuration(HOVER_MS).setInterpolator { circularOut(it) }.start()
    }

    private fun dp(value: Float): Float = value * activity.resources.displayMetrics.density

    private companion object {
        const val ENTRY_MS = 450L
        const val HOVER_MS = 220L
        /** The glint's slant, rotate(25deg) in the layout. */
        val TILT = Math.toRadians(25.0).toFloat()

        /** Key frames at 0%, 50% and 100% going there and back: 0 to 1 to 0. */
        fun there(p: Float): Float = if (p < 0.5f) p * 2 else (1 - p) * 2

        fun between(from: Float, to: Float, amount: Float): Float = from + (to - from) * amount

        fun sineInOut(t: Float): Float = (-(cos(PI * t) - 1) / 2).toFloat()

        fun circularOut(t: Float): Float = sqrt(1 - (t - 1) * (t - 1))

        fun circularInOut(t: Float): Float =
            if (t < 0.5f) (1 - sqrt(1 - (2 * t) * (2 * t))) / 2 else (sqrt(1 - (2 - 2 * t) * (2 - 2 * t)) + 1) / 2

        fun withAlpha(color: Int, alpha: Float): Int = (((color ushr 24) * alpha).toInt() shl 24) or (color and 0xFFFFFF)
    }
}

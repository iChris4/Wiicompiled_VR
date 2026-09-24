package org.wiicompiled.quest.launcher

import android.app.Activity
import android.graphics.drawable.GradientDrawable
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.Spinner
import android.widget.TextView
import android.widget.Toast
import org.wiicompiled.quest.R

/**
 * The launcher's Friends page, WheelWizard's FriendsPage: the friend list of the licence the
 * sidebar shows ([FriendList]), sorted as chosen, each friend a FriendsListItem card whose View
 * Room opens their room on the Rooms page ([viewRoom]) while they play. A tap on a card opens the
 * player's actions, Remove Friend among them; Add Friend asks for a friend code ([FriendActions]).
 * The save is read again each time the page is shown, and who is online follows [LiveRooms].
 */
class FriendsPage(
    private val activity: Activity,
    root: View,
    private val actions: FriendActions,
    private val viewRoom: (LiveRooms.Room) -> Unit,
) {
    private val content: View = root.findViewById(R.id.friends_content)
    private val empty: View = root.findViewById(R.id.friends_empty)
    private val addTop: View = root.findViewById(R.id.friends_add_top)
    private val count: TextView = root.findViewById(R.id.friends_count)
    private val list: ListView = root.findViewById(R.id.friends_list)
    private val adapter = CardAdapter()

    private var visible = false
    private var badges: Map<String, List<RetroWfc.Badge>> = emptyMap()
    private val changed: () -> Unit = { render() }

    private val density = activity.resources.displayMetrics.density
    /** The Mii picture in item_friend.xml, drawn at twice its pixels for smooth edges. */
    private val miiSize = (2 * MII_DP * density).toInt() and 1.inv()

    init {
        list.adapter = adapter
        addTop.setOnClickListener { actions.addByCode() }
        root.findViewById<View>(R.id.friends_add_empty).setOnClickListener { actions.addByCode() }
        root.findViewById<Spinner>(R.id.friends_sort).apply {
            background = activity.getDrawable(R.drawable.bg_dropdown)
            setPopupBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
            dropDownVerticalOffset = PatchesWidgets.dp(activity, 4)
            adapter = ArrayAdapter(activity, R.layout.item_dropdown, FriendList.Order.entries.map { activity.getString(label(it)) }).apply {
                setDropDownViewResource(R.layout.item_dropdown_popup)
            }
            setSelection(order.ordinal, false)
            onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                    if (order.ordinal == position) return
                    order = FriendList.Order.entries[position]
                    render()
                }

                override fun onNothingSelected(parent: AdapterView<*>?) = Unit
            }
        }
    }

    /** While the page is on screen it follows the save and who is online. */
    fun setVisible(shown: Boolean) {
        if (shown == visible) return
        visible = shown
        if (!shown) {
            FriendList.removeListener(changed)
            LiveRooms.removeListener(changed)
            return
        }
        FriendList.addListener(changed)
        LiveRooms.addListener(changed)
        ProfileStore.badges { loaded ->
            badges = loaded
            if (!activity.isDestroyed) render()
        }
        render()
        // The game may have changed the list since it was read.
        FriendList.load(activity)
    }

    /** UpdateFriendList and HandleVisibility. */
    private fun render() {
        if (activity.isDestroyed) return
        val friends = FriendList.sorted(FriendList.friends, order, FriendList::isOnline)
        val any = friends.isNotEmpty()
        content.visibility = if (any) View.VISIBLE else View.GONE
        empty.visibility = if (any) View.GONE else View.VISIBLE
        addTop.visibility = if (any) View.VISIBLE else View.GONE
        count.text = friends.size.toString()
        adapter.friends = friends
        adapter.notifyDataSetChanged()
    }

    private fun label(order: FriendList.Order): Int = when (order) {
        FriendList.Order.Online -> R.string.friends_sort_online
        FriendList.Order.Vr -> R.string.room_vr_full
        FriendList.Order.Br -> R.string.friends_br_full
        FriendList.Order.Name -> R.string.friends_sort_name
        FriendList.Order.Wins -> R.string.friends_wins
        FriendList.Order.Races -> R.string.friends_sort_races
    }

    /** ViewRoom_OnClick. */
    private fun openRoomOf(friend: RksysFriends.Friend) {
        val room = Leaderboard.roomOf(LiveRooms.rooms, friend.friendCode)
        if (room == null) {
            Toast.makeText(activity, R.string.friends_no_room, Toast.LENGTH_LONG).show()
            return
        }
        viewRoom(room)
    }

    /** FriendsListItem cards. */
    private inner class CardAdapter : BaseAdapter() {
        var friends: List<RksysFriends.Friend> = emptyList()

        override fun getCount(): Int = friends.size
        override fun getItem(position: Int): Any = friends[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val card = convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_friend, parent, false).also(::sizeIcons)
            val friend = friends[position]
            val online = FriendList.isOnline(friend)
            val pending = friend.isPending
            // Pending shows over online, as the PC's styles order them.
            val accent = when {
                pending -> R.color.warning_500
                online -> R.color.primary_400
                else -> null
            }

            card.foreground = accent?.let { outline(activity.getColor(it), 6f, 1) }
            card.findViewById<View>(R.id.friend_glow).apply {
                visibility = if (pending || online) View.VISIBLE else View.GONE
                if (pending || online) background = glow(activity.getColor(if (pending) R.color.warning_200 else R.color.primary_200))
            }
            card.findViewById<TextView>(R.id.friend_status).apply {
                setText(if (pending) R.string.friends_pending else if (online) R.string.friends_online else R.string.friends_offline)
                setTextColor(activity.getColor(if (pending) R.color.warning_400 else R.color.neutral_200))
            }
            card.findViewById<TextView>(R.id.friend_name).text = friend.name
            card.findViewById<TextView>(R.id.friend_code).text = friend.friendCode
            card.findViewById<TextView>(R.id.friend_vr).text = rating(friend.vr)
            card.findViewById<TextView>(R.id.friend_br).text = rating(friend.br)
            card.findViewById<TextView>(R.id.friend_wins).text = friend.wins.toString()
            card.findViewById<TextView>(R.id.friend_losses).text = friend.losses.toString()

            val badgeList = badges[friend.friendCode].orEmpty()
            card.findViewById<View>(R.id.friend_badges_box).apply {
                visibility = if (badgeList.isEmpty()) View.GONE else View.VISIBLE
                background = outline(activity.getColor(accent ?: R.color.neutral_600), 6f, 1, fill = activity.getColor(R.color.neutral_950))
            }
            card.findViewById<LinearLayout>(R.id.friend_badges).apply {
                removeAllViews()
                badgeList.forEachIndexed { index, badge ->
                    val size = PatchesWidgets.dp(activity, 30)
                    addView(BadgeView(activity, badge), LinearLayout.LayoutParams(size, size).apply {
                        if (index > 0) marginStart = PatchesWidgets.dp(activity, 3)
                    })
                }
            }

            card.findViewById<TextView>(R.id.friend_view_room).apply {
                isEnabled = online
                // The Warning variant for a pending friend; only its icon beside badges.
                setBackgroundResource(if (pending) R.drawable.bg_button_warning else R.drawable.bg_play_button)
                val content = activity.getColorStateList(if (pending) R.color.warning_button_content else R.color.play_button_content)
                setTextColor(content)
                compoundDrawableTintList = content
                text = if (badgeList.isEmpty()) activity.getString(R.string.leaderboard_view_room) else ""
                compoundDrawablePadding = if (badgeList.isEmpty()) PatchesWidgets.dp(activity, 10) else 0
                val side = PatchesWidgets.dp(activity, if (badgeList.isEmpty()) 16 else 12)
                setPaddingRelative(side, 0, side, 0)
                tooltipText = activity.getString(R.string.leaderboard_view_room)
                setOnClickListener { openRoomOf(friend) }
            }

            // FriendsSideProfile, or FriendsSideProfilePending's angry face.
            val picture = card.findViewById<ImageView>(R.id.friend_mii)
            val placeholder = card.findViewById<View>(R.id.friend_placeholder)
            val expression = if (pending) EXPRESSION_ANGER else 0
            val key = "${friend.profileId}:$expression:${friend.mii.lookKey()}"
            if (picture.tag != key) {
                picture.tag = key
                picture.setImageDrawable(null)
                placeholder.visibility = View.VISIBLE
                if (MiiRenderResource.installed(activity)) {
                    MiiImages.picture(activity, friend.mii, miiSize, MiiRenderer.Pose.SIDE, expression) { bitmap ->
                        if (picture.tag != key || bitmap == null) return@picture
                        picture.setImageBitmap(bitmap)
                        placeholder.visibility = View.GONE
                    }
                }
            }

            card.setOnClickListener {
                PlayerActions.show(activity, card, friend.friendCode, friend.mii, removeFriend = { actions.remove(friend) })
            }
            return card
        }
    }

    /** The PC shows the most a friend's rating can hold, 9999, as 9999+. */
    private fun rating(value: Int): String = if (value == 9999) "9999+" else value.toString()

    /** The labels' tips and View Room's road at the PC's sizes. */
    private fun sizeIcons(card: View) {
        listOf(R.id.friend_vr_label to 12, R.id.friend_br_label to 12, R.id.friend_view_room to 16).forEach { (id, dp) ->
            card.findViewById<TextView>(id).apply {
                val size = PatchesWidgets.dp(activity, dp)
                val drawables = compoundDrawablesRelative.map { drawable -> drawable?.mutate()?.apply { setBounds(0, 0, size, size) } }
                setCompoundDrawablesRelative(drawables[0], drawables[1], drawables[2], drawables[3])
            }
        }
    }

    private fun outline(color: Int, radiusDp: Float, widthDp: Int, fill: Int? = null) = GradientDrawable().apply {
        cornerRadius = radiusDp * density
        setStroke((widthDp * density).toInt().coerceAtLeast(1), color)
        fill?.let(::setColor)
    }

    /**
     * The PC's glow: a 100 x 175 blob at half opacity, blurred by 250 pixels, which leaves a soft
     * wash of colour at the card's end; a radial fade gives the same.
     */
    private fun glow(color: Int) = GradientDrawable().apply {
        shape = GradientDrawable.OVAL
        gradientType = GradientDrawable.RADIAL_GRADIENT
        gradientRadius = 150 * density
        colors = intArrayOf(withAlpha(color, 0.3f), withAlpha(color, 0.12f), withAlpha(color, 0f))
    }

    private fun withAlpha(color: Int, alpha: Float): Int = ((255 * alpha).toInt() shl 24) or (color and 0xFFFFFF)

    private companion object {
        const val MII_DP = 138
        /** FFL's anger, the PC's FriendsSideProfilePending. */
        const val EXPRESSION_ANGER = 2
        /** The PC keeps the order for the session, in a static, not as a setting. */
        var order = FriendList.Order.Online
    }
}

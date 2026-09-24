package org.wiicompiled.quest.launcher

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ListView
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.text.NumberFormat
import java.util.Locale
import java.util.concurrent.Executors
import org.wiicompiled.quest.R

/**
 * The launcher's Leaderboard page, WheelWizard's LeaderboardPage: Retro WFC's top 50
 * ([Leaderboard]), the first three on a podium and the rest in a list. A player opens their actions
 * ([PlayerActions], Add Friend through [friends]); one in a room now also has View Room, which
 * opens it on the Rooms page ([viewRoom]). Like the PC, which makes the page anew each time it is
 * opened, it asks again every time it is shown, through the 90-second cache it shares with the
 * rooms.
 */
class LeaderboardPage(
    private val activity: Activity,
    root: View,
    private val friends: FriendActions,
    private val viewRoom: (LiveRooms.Room) -> Unit,
) {
    private val loading: View = root.findViewById(R.id.leaderboard_loading)
    private val error: View = root.findViewById(R.id.leaderboard_error)
    private val errorText: TextView = root.findViewById(R.id.leaderboard_error_text)
    private val empty: View = root.findViewById(R.id.leaderboard_empty)
    private val list: ListView = root.findViewById(R.id.leaderboard_list)
    private val header: View = LayoutInflater.from(activity).inflate(R.layout.header_leaderboard, list, false)

    /** First, second and third, with the PC's avatar sizes, entry delays and floats. */
    private val podium = listOf(
        PodiumCard(activity, header.findViewById(R.id.podium_first), PodiumCard.Medal.Gold, 122, 0, PodiumCard.Drift(5_200, 0f, -5f)),
        PodiumCard(activity, header.findViewById(R.id.podium_second), PodiumCard.Medal.Silver, 104, 80, PodiumCard.Drift(4_700, -1f, -6f)),
        PodiumCard(activity, header.findViewById(R.id.podium_third), PodiumCard.Medal.Bronze, 104, 160, PodiumCard.Drift(5_700, -2f, -7f)),
    )
    private val adapter = RowAdapter()

    private var visible = false
    /** Changes with every load and when the page is hidden, so an answer that comes late is dropped. */
    private var generation = 0
    private var rows: List<Leaderboard.Row> = emptyList()
    private var badges: Map<String, List<RetroWfc.Badge>> = emptyMap()
    /** A player can join or leave a room while the list is shown; their View Room follows. */
    private val roomsChanged: () -> Unit = { adapter.notifyDataSetChanged() }

    init {
        list.addHeaderView(header, null, false)
        list.adapter = adapter
        root.findViewById<View>(R.id.leaderboard_retry).setOnClickListener { load() }
        root.findViewById<View>(R.id.leaderboard_refresh).setOnClickListener { load() }
        podium.forEachIndexed { index, card ->
            card.root.setOnClickListener {
                rows.getOrNull(index)?.let { row -> showActions(row, card.root, offsetDp = 20) }
            }
        }
    }

    fun setVisible(shown: Boolean) {
        if (shown == visible) return
        visible = shown
        if (!shown) {
            generation++
            LiveRooms.removeListener(roomsChanged)
            podium.forEach { it.stop() }
            return
        }
        LiveRooms.addListener(roomsChanged)
        ProfileStore.badges { loaded ->
            badges = loaded
            if (activity.isDestroyed) return@badges
            adapter.notifyDataSetChanged()
            podium.forEachIndexed { index, card -> card.showBadge(rows.getOrNull(index)?.let(::firstBadge)) }
        }
        load()
    }

    /** ReloadLeaderboardAsync. */
    private fun load() {
        val asked = ++generation
        podium.forEach { it.stop() }
        show(State.Loading)
        worker.execute {
            val result = try {
                Result.success(Leaderboard.rows(Leaderboard.top()))
            } catch (e: IOException) {
                Result.failure(e)
            }
            main.post {
                if (asked != generation || activity.isDestroyed) return@post
                result.fold(onSuccess = ::fill, onFailure = { e ->
                    errorText.text = e.message?.takeIf { it.isNotBlank() } ?: activity.getString(R.string.leaderboard_error_fallback)
                    show(State.Error)
                })
            }
        }
    }

    private fun fill(loaded: List<Leaderboard.Row>) {
        rows = loaded
        if (loaded.isEmpty()) {
            show(State.Empty)
            return
        }
        podium.forEachIndexed { index, card ->
            val row = loaded.getOrNull(index)
            card.fill(row, row?.let(::name).orEmpty(), row?.let { place(it.rank) }.orEmpty(), row?.let(::vrText).orEmpty(), row?.let(::firstBadge))
        }
        adapter.rows = loaded.drop(PODIUM)
        adapter.notifyDataSetChanged()
        list.setSelection(0)
        show(State.Data)
        podium.forEachIndexed { index, card -> if (index < loaded.size) card.enter() }
    }

    private enum class State { Loading, Error, Empty, Data }

    private fun show(state: State) {
        loading.visibility = if (state == State.Loading) View.VISIBLE else View.GONE
        error.visibility = if (state == State.Error) View.VISIBLE else View.GONE
        empty.visibility = if (state == State.Empty) View.VISIBLE else View.GONE
        list.visibility = if (state == State.Data) View.VISIBLE else View.GONE
    }

    // What a row shows (CreateLeaderboardPlayer).

    private fun name(row: Leaderboard.Row): String = row.name.ifBlank { activity.getString(R.string.leaderboard_unknown_player) }

    private fun vrText(row: Leaderboard.Row): String = row.vr?.let { NumberFormat.getIntegerInstance(Locale.getDefault()).format(it) } ?: "--"

    private fun firstBadge(row: Leaderboard.Row): RetroWfc.Badge? = badges[row.friendCode]?.firstOrNull()

    /** GetPlacementLabel. */
    private fun place(rank: Int): String = when (rank) {
        1 -> activity.getString(R.string.leaderboard_champion)
        2 -> activity.getString(R.string.leaderboard_second)
        3 -> activity.getString(R.string.leaderboard_third)
        else -> activity.getString(R.string.leaderboard_rank, rank)
    }

    /** The player's context menu, Add Friend with the Mii and VR the leaderboard gives. */
    private fun showActions(row: Leaderboard.Row, anchor: View, offsetDp: Int = 60) {
        PlayerActions.show(activity, anchor, row.friendCode, row.mii, offsetDp, addFriend = {
            friends.addPlayer(name(row), row.friendCode, row.mii, row.vr)
        })
    }

    /** JoinRoom_OnClick: the room the player is in now, on the Rooms page. */
    private fun joinRoom(row: Leaderboard.Row) {
        val room = Leaderboard.roomOf(LiveRooms.rooms, row.friendCode)
        if (room == null) {
            Toast.makeText(activity, R.string.leaderboard_no_room, Toast.LENGTH_SHORT).show()
            return
        }
        viewRoom(room)
    }

    /** The players after the podium, as PlayerListItem rows with their place. */
    private inner class RowAdapter : BaseAdapter() {
        var rows: List<Leaderboard.Row> = emptyList()

        override fun getCount(): Int = rows.size
        override fun getItem(position: Int): Any = rows[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_player, parent, false).also {
                it.findViewById<View>(R.id.player_top).setBackgroundResource(R.drawable.bg_top_label_neutral)
                it.findViewById<TextView>(R.id.player_top).setTextColor(activity.getColor(R.color.neutral_100))
            }
            val player = rows[position]
            PlayerRow.bind(
                activity,
                row,
                mii = player.mii,
                name = name(player),
                friendCode = player.friendCode,
                vrText = vrText(player),
                badges = badges[player.friendCode].orEmpty(),
                topLabel = activity.getString(R.string.leaderboard_rank, player.rank),
                isOpenHost = false,
            )
            row.findViewById<View>(R.id.player_join).apply {
                visibility = if (Leaderboard.roomOf(LiveRooms.rooms, player.friendCode) != null) View.VISIBLE else View.GONE
                setOnClickListener { joinRoom(player) }
            }
            row.setOnClickListener { showActions(player, row) }
            return row
        }
    }

    private companion object {
        const val PODIUM = 3
        val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "Leaderboard").apply { isDaemon = true } }
        val main by lazy { Handler(Looper.getMainLooper()) }
    }
}

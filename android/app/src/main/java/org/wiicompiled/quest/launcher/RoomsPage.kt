package org.wiicompiled.quest.launcher

import android.app.Activity
import android.text.Editable
import android.text.TextWatcher
import android.view.LayoutInflater
import android.view.View
import android.view.ViewGroup
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.BaseAdapter
import android.widget.EditText
import android.widget.ImageView
import android.widget.ListView
import android.widget.TextView
import org.wiicompiled.quest.R

/**
 * The launcher's Rooms page, WheelWizard's RoomsPage and RoomDetailsPage: the rooms open on Retro
 * WFC ([LiveRooms]), or with a search the players whose name or friend code holds it. A room opens
 * its details in place of the list, and a player there their actions ([PlayerActions]): copy their
 * friend code, see their Mii, or their profile. As on the PC, nothing here joins a room.
 */
class RoomsPage(private val activity: Activity, root: View) {

    private val main: View = root.findViewById(R.id.rooms_main)
    private val search: EditText = root.findViewById(R.id.rooms_search)
    private val empty: View = root.findViewById(R.id.rooms_empty)
    private val list: View = root.findViewById(R.id.rooms_list)
    private val listTitle: TextView = root.findViewById(R.id.rooms_list_title)
    private val listCount: TextView = root.findViewById(R.id.rooms_list_count)
    private val items: ListView = root.findViewById(R.id.rooms_items)

    private val details: View = root.findViewById(R.id.rooms_details)
    private val detailsLock: View = root.findViewById(R.id.room_lock)
    private val detailsId: TextView = root.findViewById(R.id.room_id)
    private val detailsTime: TextView = root.findViewById(R.id.room_time)
    private val detailsType: TextView = root.findViewById(R.id.room_type)
    private val detailsVr: TextView = root.findViewById(R.id.room_average_vr)
    private val detailsCount: TextView = root.findViewById(R.id.room_player_count)
    private val detailsPlayers: ListView = root.findViewById(R.id.room_players)

    private val roomAdapter = RoomAdapter()
    private val searchAdapter = PlayerAdapter { player -> roomOf(player)?.let(::openRoom) }
    private val detailsAdapter = PlayerAdapter { player, row -> PlayerActions.show(activity, row, player.friendCode, player.mii) }

    private var visible = false
    private var query = ""
    private var badges: Map<String, List<RetroWfc.Badge>> = emptyMap()
    /** The room whose details are open, as last shown; null on the list. */
    private var detailsRoom: LiveRooms.Room? = null
    private val roomsChanged: () -> Unit = { render() }

    init {
        detailsPlayers.adapter = detailsAdapter
        root.findViewById<View>(R.id.room_back).setOnClickListener { closeRoom() }
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(text: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(text: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(text: Editable?) {
                query = text?.toString().orEmpty()
                renderList()
            }
        })
        search.setOnEditorActionListener { view, action, _ ->
            if (action != EditorInfo.IME_ACTION_SEARCH) return@setOnEditorActionListener false
            activity.getSystemService(InputMethodManager::class.java)?.hideSoftInputFromWindow(view.windowToken, 0)
            true
        }
    }

    /** While the page is on screen it follows every answer from Retro WFC. */
    fun setVisible(shown: Boolean) {
        if (shown == visible) return
        visible = shown
        if (!shown) {
            LiveRooms.removeListener(roomsChanged)
            return
        }
        LiveRooms.addListener(roomsChanged)
        ProfileStore.badges { loaded ->
            badges = loaded
            if (!activity.isDestroyed) render()
        }
        render()
    }

    /** Opens [room]'s details, as the leaderboard's View Room does (RoomDetailsPage). */
    fun showRoom(room: LiveRooms.Room) {
        if (search.text.isNotEmpty()) search.text.clear()
        openRoom(room)
    }

    /** Back closes a room's details before leaving the page. */
    fun back(): Boolean {
        if (detailsRoom == null) return false
        closeRoom()
        return true
    }

    private fun render() {
        if (activity.isDestroyed) return
        renderList()
        renderRoom()
    }

    // The list: rooms, or the players the search finds (RoomsPage.OnUpdate and PerformSearch).

    private fun renderList() {
        val rooms = LiveRooms.rooms
        empty.visibility = if (rooms.isEmpty()) View.VISIBLE else View.GONE
        list.visibility = if (rooms.isEmpty()) View.GONE else View.VISIBLE
        if (query.isBlank()) {
            roomAdapter.rooms = rooms
            listTitle.setText(R.string.rooms_list_rooms)
            listCount.text = rooms.size.toString()
            if (items.adapter !== roomAdapter) items.adapter = roomAdapter
            roomAdapter.notifyDataSetChanged()
        } else {
            searchAdapter.players = LiveRooms.search(rooms, query)
            listTitle.setText(R.string.rooms_list_players)
            listCount.text = searchAdapter.players.size.toString()
            if (items.adapter !== searchAdapter) items.adapter = searchAdapter
            searchAdapter.notifyDataSetChanged()
        }
    }

    /** The room a player is in, as the PC finds it for a player picked in the search. */
    private fun roomOf(player: LiveRooms.Player): LiveRooms.Room? = LiveRooms.rooms.firstOrNull { player in it.players }

    // A room's details (RoomDetailsPage).

    private fun openRoom(room: LiveRooms.Room) {
        detailsRoom = room
        main.visibility = View.GONE
        details.visibility = View.VISIBLE
        detailsPlayers.setSelection(0)
        renderRoom()
    }

    private fun closeRoom() {
        detailsRoom = null
        details.visibility = View.GONE
        main.visibility = View.VISIBLE
    }

    private fun renderRoom() {
        val shown = detailsRoom ?: return
        // The room as Retro WFC now has it; a room it split shares its ID, so the one with the most
        // players in common. A room that closed takes the page back to the list, as on the PC.
        val room = LiveRooms.rooms.filter { it.id == shown.id }.maxByOrNull { candidate -> candidate.players.count { it in shown.players } }
        if (room == null) {
            closeRoom()
            return
        }
        detailsRoom = room
        detailsLock.visibility = if (room.isPublic) View.GONE else View.VISIBLE
        detailsId.text = room.id
        detailsTime.text = timeOnline(room)
        detailsType.text = room.gameMode
        detailsVr.text = room.averageVr.toString()
        detailsCount.text = room.players.size.toString()
        detailsAdapter.players = room.players
        detailsAdapter.notifyDataSetChanged()
    }

    // Rows

    /** RrRoom.TimeOnline: how long ago Retro WFC opened the room. */
    private fun timeOnline(room: LiveRooms.Room): String =
        LiveRooms.timeParts(System.currentTimeMillis() - room.created).joinToString(" ") { (unit, count) ->
            val plural = when (unit) {
                LiveRooms.TimeUnit.Days -> R.plurals.time_days
                LiveRooms.TimeUnit.Hours -> R.plurals.time_hours
                LiveRooms.TimeUnit.Minutes -> R.plurals.time_minutes
                LiveRooms.TimeUnit.Seconds -> R.plurals.time_seconds
            }
            activity.resources.getQuantityString(plural, count, count)
        }

    private inner class RoomAdapter : BaseAdapter() {
        var rooms: List<LiveRooms.Room> = emptyList()

        override fun getCount(): Int = rooms.size
        override fun getItem(position: Int): Any = rooms[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_room, parent, false)
            val room = rooms[position]
            row.findViewById<ImageView>(R.id.room_icon).setImageResource(if (room.isPublic) R.drawable.ic_road else R.drawable.ic_road_locked)
            row.findViewById<TextView>(R.id.room_mode).text = room.gameMode
            row.findViewById<TextView>(R.id.room_code).text = room.id
            row.findViewById<TextView>(R.id.room_age).text = timeOnline(room)
            row.findViewById<TextView>(R.id.room_count).text = room.players.size.toString()
            row.setOnClickListener { openRoom(room) }
            return row
        }
    }

    /** Rows of PlayerListItem; [picked] gets the player and their row. */
    private inner class PlayerAdapter(private val picked: (LiveRooms.Player, View) -> Unit) : BaseAdapter() {
        var players: List<LiveRooms.Player> = emptyList()

        constructor(picked: (LiveRooms.Player) -> Unit) : this({ player, _ -> picked(player) })

        override fun getCount(): Int = players.size
        override fun getItem(position: Int): Any = players[position]
        override fun getItemId(position: Int): Long = position.toLong()

        override fun getView(position: Int, convertView: View?, parent: ViewGroup): View {
            val row = convertView ?: LayoutInflater.from(activity).inflate(R.layout.item_player, parent, false)
            val player = players[position]
            PlayerRow.bind(
                activity,
                row,
                mii = player.mii,
                name = player.name,
                friendCode = player.friendCode,
                vrText = player.vrText,
                badges = badges[player.friendCode].orEmpty(),
                topLabel = player.topLabel.takeIf { player.leaderboardRank != null },
                isOpenHost = player.isOpenHost,
            )
            row.setOnClickListener { picked(player, row) }
            return row
        }
    }
}

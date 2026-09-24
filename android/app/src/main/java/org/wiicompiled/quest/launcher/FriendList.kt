package org.wiicompiled.quest.launcher

import android.content.Context
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.GameStorage

/**
 * The friends of the licence the sidebar shows (GameLicenseService.ActiveCurrentFriends on the
 * PC, which follows its focused licence) in Retro Rewind's save, for the Friends page and the
 * sidebar's count, and the changes to them ([RksysFriends]). The save is read again whenever the
 * launcher comes back, since the game may have changed it, and after every change here. A change
 * reads the whole save, changes it in memory and replaces the file with a finished copy, so a
 * failure part way never leaves half a save. Callers keep changes away from a running game.
 */
object FriendList {
    private const val TAG = "WiiCompiledLauncher"

    private val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "Friends").apply { isDaemon = true } }
    private val main by lazy { Handler(Looper.getMainLooper()) }
    private val listeners = LinkedHashSet<() -> Unit>()

    /** The licence whose friends these are, null without a save or a licence; main thread only. */
    var license: RksysProfiles.License? = null
        private set

    val friends: List<RksysFriends.Friend> get() = license?.friends.orEmpty()

    /** Friends in a room now, as the PC's IsOnline finds them among the live rooms' players. */
    fun isOnline(friend: RksysFriends.Friend): Boolean = Leaderboard.roomOf(LiveRooms.rooms, friend.friendCode) != null

    val onlineCount: Int get() = friends.count(::isOnline)

    /** The Friends page's Sort by (ListOrderCondition), in the PC's order. */
    enum class Order { Online, Vr, Br, Name, Wins, Races }

    /**
     * GetSortedPlayerList: highest first, and online before offline, keeping the save's order
     * among equals. Unlike the PC, which sorts names from Z to A too, names go from A to Z.
     */
    fun sorted(friends: List<RksysFriends.Friend>, order: Order, online: (RksysFriends.Friend) -> Boolean): List<RksysFriends.Friend> =
        when (order) {
            Order.Online -> friends.sortedByDescending(online)
            Order.Vr -> friends.sortedByDescending { it.vr }
            Order.Br -> friends.sortedByDescending { it.br }
            Order.Name -> friends.sortedWith(compareBy(String.CASE_INSENSITIVE_ORDER) { it.name })
            Order.Wins -> friends.sortedByDescending { it.wins }
            Order.Races -> friends.sortedByDescending { it.wins + it.losses }
        }

    /** [listener] is called on the main thread whenever the list is read again. */
    fun addListener(listener: () -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: () -> Unit) {
        listeners -= listener
    }

    /** Reads the save again. */
    fun load(context: Context) = ProfileStore.load(context) { show(context, it) }

    /** Takes the friends of the licence [snapshot] gives the sidebar, as the save was just read. */
    fun show(context: Context, snapshot: ProfileStore.Snapshot?) {
        license = snapshot?.let { ProfileStore.sidebarLicense(context, it) }
        listeners.toList().forEach { it() }
    }

    /**
     * AddFriend: [friendCode] (as [RksysFriends.normalize] gives it) with [mii] and [vr] joins the
     * licence's list as a request not yet answered. [done] gets null once it is saved and the list
     * read again, else why not.
     */
    fun add(context: Context, friendCode: String, mii: Mii, vr: Int, done: (RksysFriends.Problem?) -> Unit) {
        val slot = license?.slot ?: return done(RksysFriends.Problem.NoLicense)
        val data = MiiData.serialize(mii)
        change(context, done) { save -> RksysFriends.add(save, slot, friendCode, data, vr) }
    }

    /** RemoveFriend: [friend] leaves the licence's list; [done] as for [add]. */
    fun remove(context: Context, friend: RksysFriends.Friend, done: (RksysFriends.Problem?) -> Unit) {
        val slot = license?.slot ?: return done(RksysFriends.Problem.NoLicense)
        change(context, done) { save -> if (RksysFriends.remove(save, slot, friend.profileId)) null else RksysFriends.Problem.NotFound }
    }

    private fun change(context: Context, done: (RksysFriends.Problem?) -> Unit, edit: (ByteArray) -> RksysFriends.Problem?) {
        val app = context.applicationContext
        worker.execute {
            val problem = write(GameStorage.retroRewindSave(app), edit)
            main.post {
                if (problem != null) {
                    done(problem)
                    return@post
                }
                ProfileStore.load(app) { snapshot ->
                    show(app, snapshot)
                    done(null)
                }
            }
        }
    }

    private fun write(file: File, edit: (ByteArray) -> RksysFriends.Problem?): RksysFriends.Problem? {
        val save = try {
            file.readBytes()
        } catch (e: IOException) {
            Log.w(TAG, "Cannot read Retro Rewind's save", e)
            return RksysFriends.Problem.NotASave
        }
        if (!RksysFriends.isSave(save)) return RksysFriends.Problem.NotASave
        edit(save)?.let { return it }
        val temporary = File(file.parentFile, "${file.name}.tmp")
        return try {
            FileOutputStream(temporary).use { output ->
                output.write(save)
                output.fd.sync()
            }
            if (!temporary.renameTo(file)) throw IOException("Cannot replace $file")
            null
        } catch (e: IOException) {
            Log.w(TAG, "Cannot write Retro Rewind's save", e)
            temporary.delete()
            RksysFriends.Problem.CannotWrite
        }
    }
}

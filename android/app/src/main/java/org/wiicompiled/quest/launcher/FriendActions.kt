package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.TextWatcher
import android.view.View
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.ImageView
import android.widget.ProgressBar
import android.widget.TextView
import android.widget.Toast
import java.io.IOException
import java.util.concurrent.Executors
import org.wiicompiled.quest.R

/**
 * Adding and removing friends as WheelWizard does, into the friend list of the licence the sidebar
 * shows ([FriendList]): by friend code from the Friends page (FriendsPage.AddFriend_OnClick, which
 * looks the code up on Retro WFC for the player's name and Mii), or a player met in a room or on
 * the leaderboard (their pages' AddFriend_OnClick), each confirmed first as the PC's
 * AddFriendConfirmationWindow does. The game reads the save while it runs, so nothing is changed
 * then. Unlike the PC, removing a friend asks first, since a tap is easy to misplace in a headset.
 */
class FriendActions(private val activity: Activity, private val gameRunning: () -> Boolean) {

    /** The Friends page's Add Friend: a code typed in, looked up, confirmed and saved. */
    fun addByCode() {
        if (!canAdd()) return
        val view = activity.layoutInflater.inflate(R.layout.dialog_friend_code, null)
        val input: EditText = view.findViewById(R.id.friend_code_input)
        val note: TextView = view.findViewById(R.id.friend_code_note)
        val progress: ProgressBar = view.findViewById(R.id.friend_code_progress)
        val dialog = AlertDialog.Builder(activity)
            .setTitle(R.string.friends_add)
            .setView(view)
            .setPositiveButton(R.string.friends_submit, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        var looking = false

        /** The PC's validation (an error, which blocks) and warning (which only tells). */
        fun check(): String? {
            val result = RksysFriends.normalize(input.text.toString())
            val code = result.friendCode
            val problem = result.problem ?: if (code != null && isOwn(code)) RksysFriends.Problem.Own else null
            val submit = dialog.getButton(AlertDialog.BUTTON_POSITIVE)
            submit?.isEnabled = problem == null && !looking
            progress.visibility = View.GONE
            when {
                // Nothing typed yet is not worth a message.
                problem == RksysFriends.Problem.Empty -> note.text = ""
                problem != null -> show(note, message(problem), R.color.danger_400)
                code != null && isFriend(code) -> show(note, activity.getString(R.string.friends_already), R.color.warning_400)
                else -> note.text = ""
            }
            return if (problem == null) code else null
        }

        fun submit() {
            val code = check() ?: return
            looking = true
            input.isEnabled = false
            dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.isEnabled = false
            progress.visibility = View.VISIBLE
            show(note, activity.getString(R.string.friends_looking_up), R.color.neutral_400)
            worker.execute {
                val profile = try {
                    RetroWfc.playerProfile(code)
                } catch (e: IOException) {
                    null
                }
                main.post {
                    if (!dialog.isShowing || activity.isDestroyed) return@post
                    looking = false
                    input.isEnabled = true
                    val mii = profile?.mii?.let { data -> runCatching { MiiData.parse(data) }.getOrNull() }
                    when {
                        profile == null -> fail(note, progress, dialog, R.string.friends_not_found)
                        mii == null -> fail(note, progress, dialog, R.string.friends_profile_no_mii)
                        else -> {
                            dialog.dismiss()
                            confirm(profile.name, profile.friendCode.ifBlank { code }, mii, profile.vr)
                        }
                    }
                }
            }
        }

        input.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(text: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(text: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(text: Editable?) {
                check()
            }
        })
        input.setOnEditorActionListener { _, action, _ ->
            if (action == EditorInfo.IME_ACTION_DONE && !looking) submit()
            action == EditorInfo.IME_ACTION_DONE
        }
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { submit() }
            check()
            input.requestFocus()
        }
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        dialog.show()
    }

    /** A player from a room or the leaderboard, with the Mii and VR shown there. */
    fun addPlayer(name: String, friendCode: String, mii: Mii?, vr: Int?) {
        if (mii == null) {
            toast(activity.getString(R.string.room_no_mii))
            return
        }
        if (!canAdd()) return
        val result = RksysFriends.normalize(friendCode)
        val code = result.friendCode
        val problem = result.problem ?: when {
            code != null && isOwn(code) -> RksysFriends.Problem.Own
            code != null && isFriend(code) -> RksysFriends.Problem.Duplicate
            else -> null
        }
        if (problem != null || code == null) {
            toast(message(problem ?: RksysFriends.Problem.Invalid))
            return
        }
        confirm(name, code, mii, vr ?: 0)
    }

    /** RemoveFriend_OnClick, once confirmed. */
    fun remove(friend: RksysFriends.Friend) {
        if (refuseWhileGameRuns()) return
        AlertDialog.Builder(activity)
            .setTitle(R.string.friends_remove)
            .setMessage(activity.getString(R.string.friends_remove_question, friend.name, friend.friendCode))
            .setPositiveButton(R.string.friends_remove) { _, _ ->
                if (refuseWhileGameRuns()) return@setPositiveButton
                FriendList.remove(activity, friend) { problem ->
                    toast(if (problem == null) activity.getString(R.string.friends_removed, friend.name) else message(problem))
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** AddFriendConfirmationWindow, then AddFriend. */
    private fun confirm(name: String, friendCode: String, mii: Mii, vr: Int) {
        val shownName = name.replace("\u0000", "").ifBlank { activity.getString(R.string.friends_unknown_player) }
        val view = activity.layoutInflater.inflate(R.layout.dialog_add_friend, null)
        view.findViewById<TextView>(R.id.add_friend_name).text = shownName
        view.findViewById<TextView>(R.id.add_friend_code).text = friendCode
        view.findViewById<TextView>(R.id.add_friend_vr).text = activity.getString(R.string.friends_confirm_vr, vr)
        val picture: ImageView = view.findViewById(R.id.add_friend_mii)
        if (MiiRenderResource.installed(activity)) {
            // FriendsSideProfile, drawn at twice its pixels for smooth edges.
            val size = (2 * CONFIRM_MII_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()
            MiiImages.picture(activity, mii, size, MiiRenderer.Pose.SIDE) { bitmap ->
                if (bitmap == null) return@picture
                picture.setImageBitmap(bitmap)
                view.findViewById<View>(R.id.add_friend_placeholder).visibility = View.GONE
            }
        }
        AlertDialog.Builder(activity)
            .setTitle(R.string.friends_add)
            .setView(view)
            .setPositiveButton(R.string.friends_add) { _, _ ->
                if (refuseWhileGameRuns()) return@setPositiveButton
                FriendList.add(activity, friendCode, mii, vr) { problem ->
                    toast(if (problem == null) activity.getString(R.string.friends_added, shownName) else message(problem))
                }
            }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
    }

    /** The PC's checks before asking for a friend: a licence with a friend code, and room for one more. */
    private fun canAdd(): Boolean {
        if (refuseWhileGameRuns()) return false
        val license = FriendList.license
        if (license == null || license.profileId == 0L) {
            toast(message(RksysFriends.Problem.NoLicense))
            return false
        }
        if (FriendList.friends.size >= RksysFriends.MAX) {
            toast(activity.getString(R.string.friends_list_full))
            return false
        }
        return true
    }

    private fun isOwn(friendCode: String): Boolean {
        val own = FriendList.license?.profileId ?: 0L
        return own != 0L && own == RksysFriends.profileId(friendCode)
    }

    private fun isFriend(friendCode: String): Boolean {
        val profileId = RksysFriends.profileId(friendCode)
        return profileId != 0L && FriendList.friends.any { it.profileId == profileId }
    }

    private fun refuseWhileGameRuns(): Boolean {
        if (!gameRunning()) return false
        Toast.makeText(activity, R.string.friends_close_game, Toast.LENGTH_LONG).show()
        return true
    }

    private fun fail(note: TextView, progress: View, dialog: AlertDialog, text: Int) {
        progress.visibility = View.GONE
        show(note, activity.getString(text), R.color.danger_400)
        dialog.getButton(AlertDialog.BUTTON_POSITIVE)?.isEnabled = true
    }

    private fun show(note: TextView, text: String, color: Int) {
        note.text = text
        note.setTextColor(activity.getColor(color))
    }

    private fun toast(text: String) = Toast.makeText(activity, text, Toast.LENGTH_LONG).show()

    private fun message(problem: RksysFriends.Problem): String = activity.getString(
        when (problem) {
            RksysFriends.Problem.Empty -> R.string.friends_code_empty
            RksysFriends.Problem.NotTwelveDigits -> R.string.friends_code_digits
            RksysFriends.Problem.Invalid -> R.string.friends_code_invalid
            RksysFriends.Problem.Own -> R.string.friends_code_own
            RksysFriends.Problem.Duplicate -> R.string.friends_already
            RksysFriends.Problem.Full -> R.string.friends_list_full_remove
            RksysFriends.Problem.NoLicense -> R.string.friends_no_license
            RksysFriends.Problem.NotFound -> R.string.friends_not_in_list
            RksysFriends.Problem.NotASave -> R.string.friends_no_save
            RksysFriends.Problem.CannotWrite -> R.string.friends_cannot_write
        },
    )

    private companion object {
        /** The Mii's circle in dialog_add_friend.xml. */
        const val CONFIRM_MII_DP = 96
        val worker = Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "AddFriend").apply { isDaemon = true } }
        val main by lazy { Handler(Looper.getMainLooper()) }
    }
}

package org.wiicompiled.quest.launcher

import android.app.Activity
import android.content.ClipData
import android.content.ClipboardManager
import android.view.View
import android.widget.ArrayAdapter
import android.widget.ListPopupWindow
import android.widget.Toast
import org.wiicompiled.quest.R

/**
 * The context menu WheelWizard gives a player in a room, on the leaderboard and in the friend
 * list, opened here by a tap on them: copy their friend code, see their Mii, add them as a friend
 * ([addFriend], rooms and leaderboard), their profile, and remove them ([removeFriend], friends).
 */
object PlayerActions {

    /** Opens the menu under [anchor], [offsetDp] from its start. */
    fun show(
        activity: Activity,
        anchor: View,
        friendCode: String,
        mii: Mii?,
        offsetDp: Int = 60,
        addFriend: (() -> Unit)? = null,
        removeFriend: (() -> Unit)? = null,
    ) {
        val actions = listOfNotNull(
            R.string.room_copy_friend_code to { copyFriendCode(activity, friendCode) },
            R.string.room_view_mii to { viewMii(activity, mii) },
            addFriend?.let { R.string.friends_add to it },
            R.string.room_view_profile to { if (friendCode.isNotEmpty()) PlayerProfileDialog.show(activity, friendCode) },
            removeFriend?.let { R.string.friends_remove to it },
        )
        ListPopupWindow(activity).apply {
            anchorView = anchor
            setAdapter(ArrayAdapter(activity, R.layout.item_dropdown_popup, actions.map { activity.getString(it.first) }))
            setBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
            width = PatchesWidgets.dp(activity, 220)
            horizontalOffset = PatchesWidgets.dp(activity, offsetDp)
            isModal = true
            setOnItemClickListener { _, _, position, _ ->
                dismiss()
                actions[position].second()
            }
            show()
        }
    }

    private fun copyFriendCode(activity: Activity, friendCode: String) {
        if (friendCode.isEmpty()) return
        activity.getSystemService(ClipboardManager::class.java)
            ?.setPrimaryClip(ClipData.newPlainText(activity.getString(R.string.profiles_code_label), friendCode))
        Toast.makeText(activity, R.string.profiles_code_copied, Toast.LENGTH_SHORT).show()
    }

    private fun viewMii(activity: Activity, mii: Mii?) {
        if (mii == null) {
            Toast.makeText(activity, R.string.room_no_mii, Toast.LENGTH_SHORT).show()
            return
        }
        MiiViewDialog.show(activity, mii)
    }
}

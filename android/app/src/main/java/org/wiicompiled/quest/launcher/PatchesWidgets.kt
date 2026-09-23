package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.Switch
import kotlin.math.roundToInt
import org.wiicompiled.quest.R

/** What the Patches page and its mod browser both show: the mod name dialog and the page's switches. */
internal object PatchesWidgets {

    fun styleSwitch(activity: Activity, switch: Switch) {
        switch.thumbTintList = checkedColors(activity, R.color.neutral_50, R.color.neutral_300)
        switch.trackTintList = checkedColors(activity, R.color.primary_400, R.color.neutral_600)
    }

    private fun checkedColors(activity: Activity, checked: Int, unchecked: Int) = ColorStateList(
        arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
        intArrayOf(activity.getColor(checked), activity.getColor(unchecked)),
    )

    /**
     * WheelWizard's TextInputWindow: a name field that keeps the dialog open, with the reason shown,
     * until the name is one [validate] accepts.
     */
    fun nameDialog(
        activity: Activity,
        title: Int,
        message: String?,
        initial: String,
        positive: Int,
        validate: (String) -> ModLibrary.NameProblem?,
        onAccept: (String) -> Unit,
    ) {
        val input = EditText(activity).apply {
            setText(initial)
            setSelection(text.length)
            setHint(R.string.patches_name_hint)
            setTextColor(activity.getColor(R.color.neutral_100))
            setHintTextColor(activity.getColor(R.color.neutral_500))
            isSingleLine = true
            imeOptions = EditorInfo.IME_ACTION_DONE
        }
        val container = FrameLayout(activity).apply {
            setPadding(dp(activity, 22), dp(activity, 8), dp(activity, 22), 0)
            addView(input)
        }
        val dialog = AlertDialog.Builder(activity)
            .setTitle(title)
            .apply { message?.let { setMessage(it) } }
            .setView(container)
            .setPositiveButton(positive, null)
            .setNegativeButton(android.R.string.cancel, null)
            .create()
        fun accept() {
            val problem = validate(input.text.toString())
            if (problem != null) {
                input.error = activity.getString(
                    when (problem) {
                        ModLibrary.NameProblem.Empty -> R.string.patches_name_empty
                        ModLibrary.NameProblem.Exists -> R.string.patches_name_exists
                        ModLibrary.NameProblem.IllegalCharacters -> R.string.patches_name_illegal
                    },
                )
                return
            }
            dialog.dismiss()
            onAccept(input.text.toString().trim())
        }
        input.setOnEditorActionListener { _, action, _ ->
            if (action == EditorInfo.IME_ACTION_DONE) accept()
            action == EditorInfo.IME_ACTION_DONE
        }
        dialog.setOnShowListener {
            dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener { accept() }
            input.requestFocus()
        }
        dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_VISIBLE)
        dialog.show()
    }

    fun dp(activity: Activity, value: Int): Int = (value * activity.resources.displayMetrics.density).roundToInt()
}

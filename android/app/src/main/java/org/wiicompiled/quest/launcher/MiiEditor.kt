package org.wiicompiled.quest.launcher

import android.app.Activity
import android.app.AlertDialog
import android.content.res.ColorStateList
import android.graphics.drawable.GradientDrawable
import android.text.Editable
import android.text.InputFilter
import android.text.TextWatcher
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import org.wiicompiled.quest.R

/**
 * WheelWizard's Mii editor (MiiEditorWindow and its EditorStartPage, EditorGeneral, EditorFace
 * and the other part pages) in place of My Miis' list: the pages on the left, the Mii's face on
 * the right, redrawn after every change. The PC draws each choice from icons of its own; here each
 * is drawn from the Mii parts, in the Mii's colours, or as the head wearing it.
 */
class MiiEditor(private val activity: Activity, private val root: View, private val onClose: () -> Unit) {

    private enum class Section(val title: Int) {
        General(R.string.mii_section_general),
        Face(R.string.mii_section_face),
        Hair(R.string.mii_section_hair),
        Eyebrows(R.string.mii_section_eyebrows),
        Eyes(R.string.mii_section_eyes),
        Nose(R.string.mii_section_nose),
        Lips(R.string.mii_section_lips),
        Glasses(R.string.mii_section_glasses),
        FacialHair(R.string.mii_section_facial_hair),
        Mole(R.string.mii_section_mole),
    }

    private val start: View = root.findViewById(R.id.mii_editor_start)
    private val page: View = root.findViewById(R.id.mii_editor_page)
    private val favorite: ImageView = root.findViewById(R.id.mii_editor_favorite)
    private val name: TextView = root.findViewById(R.id.mii_editor_name)
    private val sections: LinearLayout = root.findViewById(R.id.mii_editor_sections)
    private val pageTitle: TextView = root.findViewById(R.id.mii_editor_page_title)
    private val pageScroll: ScrollView = root.findViewById(R.id.mii_editor_page_scroll)
    private val content: LinearLayout = root.findViewById(R.id.mii_editor_page_content)
    private val preview: ImageView = root.findViewById(R.id.mii_editor_preview)
    private val placeholder: View = root.findViewById(R.id.mii_editor_placeholder)
    private val note: View = root.findViewById(R.id.mii_editor_note)

    private var mii = Mii()
    private var original = Mii()
    private var section: Section? = null
    private var onSave: (Mii) -> Unit = {}

    val isOpen get() = root.visibility == View.VISIBLE

    init {
        for (entry in Section.entries) {
            sections.addView(
                TextView(activity).apply {
                    text = activity.getString(entry.title)
                    textSize = 16f
                    setTextColor(activity.getColor(R.color.neutral_100))
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(dp(14), 0, dp(12), 0)
                    background = activity.getDrawable(R.drawable.bg_nav_item)
                    setCompoundDrawablesRelativeWithIntrinsicBounds(null, null, activity.getDrawable(R.drawable.ic_arrow_right), null)
                    compoundDrawableTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_400))
                    isClickable = true
                    isFocusable = true
                    setOnClickListener { showSection(entry) }
                },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(44)).apply { bottomMargin = dp(2) },
            )
        }
        favorite.setOnClickListener {
            mii.favorite = !mii.favorite
            showFavorite()
        }
        root.findViewById<View>(R.id.mii_editor_randomize).setOnClickListener {
            mii = MiiFactory.randomLook(mii)
            changed()
        }
        root.findViewById<View>(R.id.mii_editor_cancel).setOnClickListener { close() }
        root.findViewById<View>(R.id.mii_editor_save).setOnClickListener {
            val edited = mii.copy()
            close()
            onSave(edited)
        }
        root.findViewById<View>(R.id.mii_editor_back).setOnClickListener { showStart() }
    }

    /** Opens the editor on a copy of [mii]; [onSave] gets the edited Mii. */
    fun open(mii: Mii, isNew: Boolean, onSave: (Mii) -> Unit) {
        this.mii = mii.copy()
        // A new Mii counts as changed, so Back asks before throwing it away.
        original = if (isNew) Mii() else mii.copy()
        this.onSave = onSave
        root.visibility = View.VISIBLE
        preview.setImageDrawable(null)
        showStart()
        refreshPicture()
    }

    /** For unattended tests: opens the page named [name], such as Hair. */
    fun debugShow(name: String) {
        Section.entries.firstOrNull { it.name.equals(name, ignoreCase = true) }?.let(::showSection)
    }

    /** Back: from a page to the start page, from there out of the editor, asking first if the Mii changed. */
    fun back(): Boolean {
        if (!isOpen) return false
        if (section != null) {
            showStart()
            return true
        }
        if (mii == original) {
            close()
            return true
        }
        AlertDialog.Builder(activity)
            .setTitle(R.string.mii_editor_discard_title)
            .setMessage(R.string.mii_editor_discard_message)
            .setPositiveButton(R.string.mii_editor_discard) { _, _ -> close() }
            .setNegativeButton(android.R.string.cancel, null)
            .show()
        return true
    }

    /** Draws the face again, such as once the Mii parts are installed. */
    fun refreshPicture() {
        val installed = MiiRenderResource.installed(activity)
        note.visibility = if (installed) View.GONE else View.VISIBLE
        if (!installed) {
            preview.setImageDrawable(null)
            placeholder.visibility = View.VISIBLE
            return
        }
        val size = (PREVIEW_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()
        MiiImages.preview(activity, mii, size) { bitmap ->
            if (!isOpen) return@preview
            preview.setImageBitmap(bitmap)
            placeholder.visibility = if (bitmap == null) View.VISIBLE else View.GONE
        }
        section?.let { renderSection(it, keepScroll = true) }
    }

    private fun close() {
        root.visibility = View.GONE
        section = null
        content.removeAllViews()
        onClose()
    }

    private fun showStart() {
        section = null
        page.visibility = View.GONE
        start.visibility = View.VISIBLE
        content.removeAllViews()
        name.text = mii.name
        showFavorite()
    }

    private fun showFavorite() {
        favorite.setImageResource(if (mii.favorite) R.drawable.ic_star else R.drawable.ic_star_empty)
        favorite.imageTintList = ColorStateList.valueOf(activity.getColor(if (mii.favorite) R.color.warning_500 else R.color.neutral_400))
    }

    private fun showSection(target: Section) {
        section = target
        start.visibility = View.GONE
        page.visibility = View.VISIBLE
        pageTitle.setText(target.title)
        renderSection(target, keepScroll = false)
    }

    /** Something about the Mii changed: the face is drawn again, and the open page shows the new values. */
    private fun changed() {
        name.text = mii.name
        refreshPicture()
    }

    private fun renderSection(target: Section, keepScroll: Boolean) {
        val scrollY = pageScroll.scrollY
        content.removeAllViews()
        when (target) {
            Section.General -> general()
            Section.Face -> face()
            Section.Hair -> hair()
            Section.Eyebrows -> eyebrows()
            Section.Eyes -> eyes()
            Section.Nose -> nose()
            Section.Lips -> lips()
            Section.Glasses -> glasses()
            Section.FacialHair -> facialHair()
            Section.Mole -> mole()
        }
        pageScroll.post { pageScroll.scrollTo(0, if (keepScroll) scrollY else 0) }
    }

    // --- The pages ---

    /** EditorGeneral. */
    private fun general() {
        label(R.string.mii_name)
        val nameError = errorText()
        textField(mii.name, R.string.mii_name_hint) { text ->
            // MiiName: 3 to 10 characters; the name only changes while it is valid.
            val trimmed = text.trim()
            val valid = trimmed.length in 3..MiiRanges.NAME_LENGTH
            nameError.visibility = if (valid) View.GONE else View.VISIBLE
            if (valid) {
                mii.name = text
                name.text = text
            }
        }
        nameError.setText(R.string.mii_name_length)
        content.addView(nameError)
        spacer()

        label(R.string.mii_creator_name)
        textField(mii.creatorName, R.string.mii_creator_hint) { text -> mii.creatorName = text }
        spacer()

        label(R.string.mii_creation_date)
        content.addView(
            TextView(activity).apply {
                text = MiiData.creationTimeMillis(mii.miiId)?.takeIf { mii.miiId != 1L }?.let {
                    SimpleDateFormat("yyyy-MM-dd HH:mm 'UTC'", Locale.ROOT).apply { timeZone = TimeZone.getTimeZone("UTC") }.format(Date(it))
                } ?: activity.getString(R.string.mii_creation_unknown)
                textSize = 13f
                setTextColor(activity.getColor(R.color.neutral_400))
            },
        )
        spacer()

        label(R.string.mii_gender)
        val group = RadioGroup(activity).apply { orientation = RadioGroup.HORIZONTAL }
        for ((index, text) in listOf(R.string.mii_gender_male, R.string.mii_gender_female).withIndex()) {
            group.addView(
                RadioButton(activity).apply {
                    id = View.generateViewId()
                    setText(text)
                    setTextColor(activity.getColor(R.color.neutral_100))
                    buttonTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_400))
                    isChecked = mii.girl == (index == 1)
                    setOnCheckedChangeListener { _, checked ->
                        if (checked && mii.girl != (index == 1)) {
                            mii.girl = index == 1
                            changed()
                        }
                    }
                },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { marginEnd = dp(16) },
            )
        }
        content.addView(group)
        spacer()

        slider(R.string.mii_height, mii.height) { mii.height = it }
        slider(R.string.mii_width, mii.weight) { mii.weight = it }
        spacer()

        label(R.string.mii_favorite_color)
        colorGrid(FAVORITE_COLORS, 5, mii.favoriteColor) { mii.favoriteColor = it }
    }

    /** EditorFace. */
    private fun face() {
        label(R.string.mii_skin_color)
        colorGrid(SKIN_COLORS, 6, mii.skinColor) { mii.skinColor = it }
        spacer()
        label(R.string.mii_head_shape)
        choiceGrid(MiiRanges.FACE_SHAPES, mii.faceShape, { view, i -> head(view, mii.copy(faceShape = i)) }) { mii.faceShape = it }
        spacer()
        label(R.string.mii_facial_feature)
        dropdown(activity.resources.getStringArray(R.array.mii_facial_features).toList(), mii.facialFeature) { mii.facialFeature = it }
    }

    /** EditorHair. */
    private fun hair() {
        label(R.string.mii_hair_color)
        colorGrid(HAIR_COLORS, 5, mii.hairColor) { mii.hairColor = it }
        spacer()
        checkbox(R.string.mii_mirror_hair, mii.hairFlipped) { mii.hairFlipped = it }
        spacer()
        label(R.string.mii_hair_type)
        choiceGrid(MiiRanges.HAIR_TYPES, mii.hairType, { view, i -> head(view, mii.copy(hairType = i)) }) { mii.hairType = it }
    }

    /** EditorEyebrows. */
    private fun eyebrows() {
        label(R.string.mii_hair_color)
        colorGrid(HAIR_COLORS, 5, mii.eyebrowColor) { mii.eyebrowColor = it }
        spacer()
        stepper(R.string.mii_vertical_position, mii.eyebrowVertical, MiiRanges.EYEBROW_VERTICAL, { (it - 10) * -1 }, VERTICAL) { mii.eyebrowVertical = it }
        stepper(R.string.mii_rotation, mii.eyebrowRotation, MiiRanges.EYEBROW_ROTATION, { it - 6 }, ROTATION) { mii.eyebrowRotation = it }
        stepper(R.string.mii_size, mii.eyebrowSize, MiiRanges.EYEBROW_SIZE, { it }, SIZE) { mii.eyebrowSize = it }
        stepper(R.string.mii_space_between, mii.eyebrowSpacing, MiiRanges.EYEBROW_SPACING, { it }, SIZE) { mii.eyebrowSpacing = it }
        label(R.string.mii_eyebrow_type)
        choiceGrid(MiiRanges.EYEBROW_TYPES, mii.eyebrowType, part(MiiRenderer.Part.Eyebrow, none = NO_EYEBROWS)) { mii.eyebrowType = it }
    }

    /** EditorEyes. */
    private fun eyes() {
        label(R.string.mii_eye_color)
        colorGrid(EYE_COLORS, 6, mii.eyeColor) { mii.eyeColor = it }
        spacer()
        stepper(R.string.mii_vertical_position, mii.eyeVertical, MiiRanges.EYE_VERTICAL, { (it - 12) * -1 }, VERTICAL) { mii.eyeVertical = it }
        stepper(R.string.mii_rotation, mii.eyeRotation, MiiRanges.EYE_ROTATION, { it - 4 }, ROTATION) { mii.eyeRotation = it }
        stepper(R.string.mii_size, mii.eyeSize, MiiRanges.EYE_SIZE, { it }, SIZE) { mii.eyeSize = it }
        stepper(R.string.mii_space_between, mii.eyeSpacing, MiiRanges.EYE_SPACING, { it }, SIZE) { mii.eyeSpacing = it }
        label(R.string.mii_eye_type)
        choiceGrid(MiiRanges.EYE_TYPES, mii.eyeType, part(MiiRenderer.Part.Eye)) { mii.eyeType = it }
    }

    /** EditorNose. */
    private fun nose() {
        stepper(R.string.mii_vertical_position, mii.noseVertical, MiiRanges.NOSE_VERTICAL, { (it - 9) * -1 }, VERTICAL) { mii.noseVertical = it }
        stepper(R.string.mii_size, mii.noseSize, MiiRanges.NOSE_SIZE, { it }, SIZE) { mii.noseSize = it }
        label(R.string.mii_nose_type)
        choiceGrid(MiiRanges.NOSE_TYPES, mii.noseType, part(MiiRenderer.Part.Nose)) { mii.noseType = it }
    }

    /** EditorLips. */
    private fun lips() {
        label(R.string.mii_lip_color)
        colorGrid(LIP_COLORS, 5, mii.lipColor) { mii.lipColor = it }
        spacer()
        stepper(R.string.mii_vertical_position, mii.lipVertical, MiiRanges.LIP_VERTICAL, { (it - 13) * -1 }, VERTICAL) { mii.lipVertical = it }
        stepper(R.string.mii_size, mii.lipSize, MiiRanges.LIP_SIZE, { it }, SIZE) { mii.lipSize = it }
        label(R.string.mii_mouth_type)
        choiceGrid(MiiRanges.LIP_TYPES, mii.lipType, part(MiiRenderer.Part.Mouth)) { mii.lipType = it }
    }

    /** EditorGlasses. */
    private fun glasses() {
        label(R.string.mii_glasses_type)
        choiceGrid(MiiRanges.GLASSES_TYPES, mii.glassesType, part(MiiRenderer.Part.Glasses, none = 0)) { mii.glassesType = it }
        spacer()
        label(R.string.mii_glasses_color)
        colorGrid(GLASSES_COLORS, 6, mii.glassesColor) { mii.glassesColor = it }
        spacer()
        stepper(R.string.mii_vertical_position, mii.glassesVertical, MiiRanges.GLASSES_VERTICAL, { (it - 10) * -1 }, VERTICAL) { mii.glassesVertical = it }
        stepper(R.string.mii_size, mii.glassesSize, MiiRanges.GLASSES_SIZE, { it }, SIZE) { mii.glassesSize = it }
    }

    /** EditorBeardPage. */
    private fun facialHair() {
        label(R.string.mii_hair_color)
        colorGrid(HAIR_COLORS, 5, mii.facialHairColor) { mii.facialHairColor = it }
        spacer()
        label(R.string.mii_mustache_type)
        choiceGrid(MiiRanges.MUSTACHE_TYPES, mii.mustacheType, part(MiiRenderer.Part.Mustache, none = 0)) { mii.mustacheType = it }
        spacer()
        stepper(R.string.mii_mustache_vertical, mii.mustacheVertical, MiiRanges.FACIAL_HAIR_VERTICAL, { (it - 10) * -1 }, VERTICAL) { mii.mustacheVertical = it }
        stepper(R.string.mii_mustache_size, mii.mustacheSize, MiiRanges.FACIAL_HAIR_SIZE, { it }, SIZE) { mii.mustacheSize = it }
        label(R.string.mii_beard_type)
        choiceGrid(MiiRanges.BEARD_TYPES, mii.beardType, { view, i -> head(view, mii.copy(beardType = i)) }) { mii.beardType = it }
    }

    /** EditorMole. */
    private fun mole() {
        checkbox(R.string.mii_mole_enabled, mii.moleEnabled) { mii.moleEnabled = it }
        spacer()
        stepper(R.string.mii_vertical_position, mii.moleVertical, MiiRanges.MOLE_VERTICAL, { (it - 20) * -1 }, VERTICAL) { mii.moleVertical = it }
        stepper(R.string.mii_horizontal_position, mii.moleHorizontal, MiiRanges.MOLE_HORIZONTAL, { it - 8 }, HORIZONTAL) { mii.moleHorizontal = it }
        stepper(R.string.mii_size, mii.moleSize, MiiRanges.MOLE_SIZE, { it }, SIZE) { mii.moleSize = it }
    }

    // --- Fields ---

    private fun label(text: Int) {
        content.addView(
            TextView(activity, null, 0, R.style.Launcher_FieldLabel).apply { setText(text) },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(6) },
        )
    }

    private fun spacer() {
        content.addView(View(activity), LinearLayout.LayoutParams(1, dp(14)))
    }

    private fun errorText() = TextView(activity).apply {
        textSize = 12f
        setTextColor(activity.getColor(R.color.danger_400))
        setPadding(0, dp(4), 0, 0)
        visibility = View.GONE
    }

    private fun textField(value: String, hint: Int, set: (String) -> Unit) {
        content.addView(
            EditText(activity).apply {
                setText(value)
                setHint(hint)
                filters = arrayOf(InputFilter.LengthFilter(MiiRanges.NAME_LENGTH))
                isSingleLine = true
                textSize = 15f
                setTextColor(activity.getColor(R.color.neutral_100))
                setHintTextColor(activity.getColor(R.color.neutral_500))
                background = activity.getDrawable(R.drawable.bg_search_field)
                setPadding(dp(12), 0, dp(12), 0)
                importantForAutofill = View.IMPORTANT_FOR_AUTOFILL_NO
                addTextChangedListener(object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
                    override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
                    override fun afterTextChanged(s: Editable?) = set(s?.toString().orEmpty())
                })
            },
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(40)),
        )
    }

    /** Height and Width: 0 to 127. */
    private fun slider(text: Int, value: Int, set: (Int) -> Unit) {
        label(text)
        content.addView(
            SeekBar(activity).apply {
                max = MiiRanges.SCALE_MAX
                progress = value
                progressTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_400))
                thumbTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_300))
                progressBackgroundTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_600))
                setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                    override fun onProgressChanged(bar: SeekBar, progress: Int, fromUser: Boolean) {
                        if (!fromUser) return
                        set(progress)
                        refreshFaceOnly()
                    }

                    override fun onStartTrackingTouch(bar: SeekBar) = Unit
                    override fun onStopTrackingTouch(bar: SeekBar) = Unit
                })
            },
            LinearLayout.LayoutParams(dp(260), dp(36)),
        )
    }

    private fun checkbox(text: Int, checked: Boolean, set: (Boolean) -> Unit) {
        content.addView(
            CheckBox(activity).apply {
                setText(text)
                isChecked = checked
                setTextColor(activity.getColor(R.color.neutral_200))
                buttonTintList = ColorStateList.valueOf(activity.getColor(R.color.primary_400))
                setOnCheckedChangeListener { _, value ->
                    set(value)
                    changed()
                }
            },
        )
    }

    private fun dropdown(entries: List<String>, selected: Int, set: (Int) -> Unit) {
        content.addView(
            Spinner(activity).apply {
                background = activity.getDrawable(R.drawable.bg_dropdown)
                setPopupBackgroundDrawable(activity.getDrawable(R.drawable.bg_dropdown_popup))
                adapter = ArrayAdapter(activity, R.layout.item_dropdown, entries).apply { setDropDownViewResource(R.layout.item_dropdown_popup) }
                setSelection(selected, false)
                onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
                    override fun onItemSelected(parent: AdapterView<*>?, view: View?, position: Int, id: Long) {
                        if (position == selected) return
                        set(position)
                        changed()
                    }

                    override fun onNothingSelected(parent: AdapterView<*>?) = Unit
                }
            },
            LinearLayout.LayoutParams(dp(220), dp(40)),
        )
    }

    /**
     * WheelWizard's transform controls: the value as the PC shows it, between buttons that move it
     * by one within [range]. [icons] are the decrease and increase buttons' pictures.
     */
    private fun stepper(text: Int, value: Int, range: IntRange, display: (Int) -> Int, icons: Pair<Int, Int>, set: (Int) -> Unit) {
        label(text)
        val row = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            background = GradientDrawable().apply {
                setColor(activity.getColor(R.color.neutral_950))
                cornerRadius = dp(6).toFloat()
            }
            setPadding(dp(2), dp(2), dp(2), dp(2))
        }
        fun button(icon: Int, step: Int) = ImageView(activity).apply {
            setImageResource(icon)
            imageTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_100))
            background = activity.getDrawable(R.drawable.bg_button_secondary)
            setPadding(dp(9), dp(9), dp(9), dp(9))
            isEnabled = value + step in range
            alpha = if (isEnabled) 1f else 0.4f
            setOnClickListener {
                set(value + step)
                changed()
            }
        }
        row.addView(button(icons.first, -1), LinearLayout.LayoutParams(dp(34), dp(34)))
        row.addView(
            TextView(activity).apply {
                setText(display(value).toString())
                gravity = Gravity.CENTER
                textSize = 15f
                setTextColor(activity.getColor(R.color.neutral_100))
            },
            LinearLayout.LayoutParams(dp(44), ViewGroup.LayoutParams.WRAP_CONTENT),
        )
        row.addView(button(icons.second, 1), LinearLayout.LayoutParams(dp(34), dp(34)))
        content.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        spacer()
    }

    /** The PC's paint-brush buttons: one swatch per colour. */
    private fun colorGrid(colors: IntArray, columns: Int, selected: Int, set: (Int) -> Unit) {
        val grid = TileGrid(activity).apply {
            this.columns = columns
            spacing = dp(8)
        }
        for ((index, color) in colors.withIndex()) {
            val cell = FrameLayout(activity).apply {
                background = activity.getDrawable(R.drawable.bg_mii_choice)
                isSelected = index == selected
                isClickable = true
                isFocusable = true
                setOnClickListener {
                    if (index == selected) return@setOnClickListener
                    set(index)
                    changed()
                }
            }
            cell.addView(
                View(activity).apply {
                    background = GradientDrawable().apply {
                        shape = GradientDrawable.OVAL
                        setColor(color)
                        setStroke(dp(1), activity.getColor(R.color.neutral_600))
                    }
                },
                FrameLayout.LayoutParams(dp(26), dp(26), Gravity.CENTER),
            )
            grid.addView(cell, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(40)))
        }
        content.addView(grid)
    }

    /** The PC's icon buttons: [count] choices, five to a row, each pictured by [bind]. */
    private fun choiceGrid(count: Int, selected: Int, bind: (ImageView, Int) -> Unit, set: (Int) -> Unit) {
        val grid = TileGrid(activity).apply {
            columns = 5
            spacing = dp(8)
        }
        for (index in 0 until count) {
            val cell = FrameLayout(activity).apply {
                background = activity.getDrawable(R.drawable.bg_mii_choice)
                isSelected = index == selected
                isClickable = true
                isFocusable = true
                contentDescription = (index + 1).toString()
                setOnClickListener {
                    if (index == selected) return@setOnClickListener
                    set(index)
                    changed()
                }
            }
            val picture = ImageView(activity)
            cell.addView(picture, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
            if (MiiRenderResource.installed(activity)) {
                bind(picture, index)
            } else {
                // Without the parts there is nothing to draw: the PC's order, numbered.
                cell.addView(
                    TextView(activity).apply {
                        text = (index + 1).toString()
                        gravity = Gravity.CENTER
                        setTextColor(activity.getColor(R.color.neutral_300))
                    },
                    FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT),
                )
            }
            grid.addView(cell, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(CHOICE_DP)))
        }
        content.addView(grid)
        spacer()
    }

    /** Pictures a choice as the Mii's head wearing it, without the body, enlarged past the cell's edges so the head fills it. */
    private fun head(view: ImageView, variant: Mii) {
        view.scaleX = HEAD_ZOOM
        view.scaleY = HEAD_ZOOM
        view.translationY = -dp(4).toFloat()
        (view.parent as? View)?.clipToOutline = true
        MiiImages.head(activity, view, variant, (choicePixels() * HEAD_ZOOM).toInt() and 1.inv(), withBody = false)
    }

    /** Pictures a flat part's choices from its texture; [none] is the choice that is no part at all. */
    private fun part(part: MiiRenderer.Part, none: Int = -1): (ImageView, Int) -> Unit = { view, index ->
        view.setPadding(dp(6), dp(6), dp(6), dp(6))
        if (index == none) {
            view.setImageResource(R.drawable.ic_x_mark)
            view.imageTintList = ColorStateList.valueOf(activity.getColor(R.color.neutral_500))
            view.setPadding(dp(18), dp(18), dp(18), dp(18))
        } else {
            view.background = GradientDrawable().apply {
                setColor(SKIN_COLORS[mii.skinColor.coerceIn(0, SKIN_COLORS.size - 1)])
                cornerRadius = dp(6).toFloat()
            }
            (view.layoutParams as? FrameLayout.LayoutParams)?.setMargins(dp(4), dp(4), dp(4), dp(4))
            MiiImages.part(activity, view, mii, part, index, choicePixels())
        }
    }

    /** A redraw of the face alone, for sliders, whose page must not be rebuilt under the finger. */
    private fun refreshFaceOnly() {
        val size = (PREVIEW_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()
        MiiImages.preview(activity, mii, size) { bitmap ->
            if (!isOpen) return@preview
            preview.setImageBitmap(bitmap)
            placeholder.visibility = if (bitmap == null) View.VISIBLE else View.GONE
        }
    }

    private fun choicePixels() = (CHOICE_DP * activity.resources.displayMetrics.density).toInt() and 1.inv()

    private fun dp(value: Int): Int = PatchesWidgets.dp(activity, value)

    private companion object {
        const val PREVIEW_DP = 340
        const val CHOICE_DP = 60
        const val HEAD_ZOOM = 1.45f
        /** The Wii's last eyebrow is none. */
        const val NO_EYEBROWS = 23

        val VERTICAL = R.drawable.ic_arrow_up to R.drawable.ic_arrow_down
        val HORIZONTAL = R.drawable.ic_arrow_left to R.drawable.ic_arrow_right
        val ROTATION = R.drawable.ic_rotate_left to R.drawable.ic_rotate_right
        val SIZE = R.drawable.ic_minus to R.drawable.ic_plus

        private fun rgb(r: Int, g: Int, b: Int) = (0xFF shl 24) or (r shl 16) or (g shl 8) or b

        // MiiColorMappings on the PC, in the Wii's order of each colour.
        val FAVORITE_COLORS = intArrayOf(
            rgb(252, 33, 20), rgb(255, 119, 27), rgb(255, 237, 33), rgb(143, 240, 31), rgb(0, 130, 50), rgb(10, 80, 184),
            rgb(71, 186, 225), rgb(255, 98, 126), rgb(138, 42, 176), rgb(87, 62, 23), rgb(255, 255, 250), rgb(0, 0, 0),
        )
        val SKIN_COLORS = intArrayOf(
            rgb(255, 211, 157), rgb(255, 185, 99), rgb(222, 123, 61), rgb(255, 171, 128), rgb(200, 83, 39), rgb(117, 46, 23),
        )
        val HAIR_COLORS = intArrayOf(
            rgb(0, 0, 0), rgb(86, 45, 27), rgb(120, 37, 21), rgb(157, 74, 32), rgb(152, 139, 140), rgb(104, 78, 27),
            rgb(171, 106, 36), rgb(255, 183, 87),
        )
        val EYE_COLORS = intArrayOf(rgb(0, 0, 0), rgb(0x47, 0x4B, 0x5D), rgb(150, 72, 45), rgb(165, 152, 55), rgb(85, 93, 195), rgb(72, 143, 100))
        val LIP_COLORS = intArrayOf(rgb(255, 93, 13), rgb(255, 18, 13), rgb(255, 83, 77))
        val GLASSES_COLORS = intArrayOf(rgb(144, 144, 144), rgb(202, 147, 102), rgb(255, 87, 77), rgb(123, 135, 189), rgb(255, 175, 71), rgb(220, 197, 190))
    }
}

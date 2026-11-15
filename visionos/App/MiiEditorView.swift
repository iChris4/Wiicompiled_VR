// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI

/// WheelWizard's Mii editor (MiiEditorWindow and its EditorStartPage, EditorGeneral, EditorFace
/// and the other part pages) in place of My Miis' list, as the Quest launcher has it
/// (MiiEditor.kt): the pages on the left, the Mii's face on the right, redrawn after every change.
/// The PC draws each choice from icons of its own; here each is drawn from the Mii parts, in the
/// Mii's colours, or as the head wearing it.
struct MiiEditorView: View {
    let session: MiiStore.EditorSession
    /// Whether the Mii parts are installed, so that there is a face to draw.
    let installed: Bool

    @Environment(\.dismiss) private var dismiss
    @State private var mii: Mii
    /// What Cancel compares with; a new Mii counts as changed, so Cancel asks before throwing it away.
    private let original: Mii
    @State private var section: Section?
    @State private var name: String
    @State private var creator: String
    @State private var confirmingDiscard = false

    private enum Section: String, CaseIterable, Identifiable {
        case general = "General"
        case face = "Face"
        case hair = "Hair"
        case eyebrows = "Eyebrows"
        case eyes = "Eyes"
        case nose = "Nose"
        case lips = "Lips"
        case glasses = "Glasses"
        case facialHair = "Facial Hair"
        case mole = "Mole"

        var id: String { rawValue }
    }

    private static let previewSide: CGFloat = 300
    private static let choiceSide: CGFloat = 60
    private static let headZoom: CGFloat = 1.45
    /// The Wii's last eyebrow is none.
    private static let noEyebrows = 23

    init(session: MiiStore.EditorSession, installed: Bool) {
        self.session = session
        self.installed = installed
        original = session.isNew ? Mii() : session.mii
        _mii = State(initialValue: session.mii)
        _name = State(initialValue: session.mii.name)
        _creator = State(initialValue: session.mii.creatorName)
    }

    var body: some View {
        HStack(alignment: .top, spacing: 28) {
            VStack(alignment: .leading, spacing: 16) {
                if let section {
                    page(section)
                } else {
                    start
                }
            }
            .frame(maxWidth: .infinity, alignment: .topLeading)

            VStack(spacing: 12) {
                MiiPicture(mii: mii, side: Self.previewSide, preview: true, installed: installed) {
                    Image(systemName: "person.fill")
                        .font(.system(size: 120))
                        .foregroundStyle(.secondary)
                }
                .background(RoundedRectangle(cornerRadius: 28).fill(Color.primary.opacity(0.06)))
                if !installed {
                    Text("Download the Mii parts on My Miis to see your Mii here.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
            }
            .frame(width: Self.previewSide)
        }
        .padding(28)
        .navigationTitle(session.isNew ? "New Mii" : "Edit Mii")
        .navigationBarBackButtonHidden(true)
        .toolbar {
            ToolbarItem(placement: .cancellationAction) {
                Button("Cancel") {
                    if mii == original { dismiss() } else { confirmingDiscard = true }
                }
            }
            ToolbarItem(placement: .confirmationAction) {
                Button("Save") {
                    let edited = mii
                    dismiss()
                    session.save(edited)
                }
            }
        }
        .alert("Discard your changes?", isPresented: $confirmingDiscard) {
            Button("Discard", role: .destructive) { dismiss() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Leaving the editor without saving loses them.")
        }
    }

    // MARK: The start page

    /// EditorStartPage: the name, the favourite star, the pages and Randomize.
    private var start: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                HStack {
                    Text(mii.name).font(.title.bold()).lineLimit(1)
                    Spacer()
                    Button {
                        mii.favorite.toggle()
                    } label: {
                        Image(systemName: mii.favorite ? "star.fill" : "star")
                            .foregroundStyle(mii.favorite ? .yellow : .secondary)
                    }
                    .buttonStyle(.bordered)
                    .accessibilityLabel(mii.favorite ? "Unfavorite" : "Favorite")
                }
                VStack(spacing: 2) {
                    ForEach(Section.allCases) { entry in
                        Button {
                            section = entry
                        } label: {
                            HStack {
                                Text(entry.rawValue)
                                Spacer()
                                Image(systemName: "chevron.right").foregroundStyle(.secondary)
                            }
                            .padding(.horizontal, 16)
                            .frame(height: 44)
                            .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 12))
                        }
                        .buttonStyle(.plain)
                        .hoverEffect()
                    }
                }
                .background(RoundedRectangle(cornerRadius: 16).fill(Color.primary.opacity(0.06)))
                Button {
                    mii = MiiFactory.randomLook(of: mii)
                } label: {
                    Label("Randomize", systemImage: "dice")
                }
                .buttonStyle(.bordered)
            }
        }
    }

    // MARK: The pages

    private func page(_ target: Section) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            Button {
                section = nil
            } label: {
                Label(target.rawValue, systemImage: "chevron.left").font(.title3.bold())
            }
            .buttonStyle(.plain)
            .hoverEffect()
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    switch target {
                    case .general: general
                    case .face: face
                    case .hair: hair
                    case .eyebrows: eyebrows
                    case .eyes: eyes
                    case .nose: nose
                    case .lips: lips
                    case .glasses: glasses
                    case .facialHair: facialHair
                    case .mole: mole
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.bottom, 20)
            }
        }
    }

    /// EditorGeneral.
    @ViewBuilder private var general: some View {
        label("Mii Name")
        TextField("Enter Mii name…", text: $name)
            .textFieldStyle(.roundedBorder)
            .autocorrectionDisabled()
            .onChange(of: name) { _, text in
                let limited = Self.limited(text)
                if limited != text {
                    name = limited
                    return
                }
                // MiiName: 3 to 10 characters; the name only changes while it is valid.
                if Self.validName(text) { mii.name = text }
            }
        if !Self.validName(name) {
            Text("Names must be between 3 and 10 characters long.").font(.caption).foregroundStyle(.red)
        }
        label("Creator Name")
        TextField("Optional", text: $creator)
            .textFieldStyle(.roundedBorder)
            .autocorrectionDisabled()
            .onChange(of: creator) { _, text in
                let limited = Self.limited(text)
                if limited != text {
                    creator = limited
                    return
                }
                mii.creatorName = text
            }
        label("Creation Date")
        Text(creationDate).font(.callout).foregroundStyle(.secondary)
        label("Gender")
        Picker("Gender", selection: $mii.girl) {
            Text("Male").tag(false)
            Text("Female").tag(true)
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .fixedSize()
        scale("Height", $mii.height)
        scale("Width", $mii.weight)
        label("Favorite Color")
        colorGrid(Self.favoriteColors, $mii.favoriteColor)
    }

    /// EditorFace.
    @ViewBuilder private var face: some View {
        label("Skin Color")
        colorGrid(Self.skinColors, $mii.skinColor)
        label("Head Shape")
        choiceGrid(MiiRanges.faceShapes, $mii.faceShape) { index in head { $0.faceShape = index } }
        label("Facial Feature")
        Picker("Facial Feature", selection: $mii.facialFeature) {
            ForEach(Self.facialFeatures.indices, id: \.self) { Text(Self.facialFeatures[$0]).tag($0) }
        }
        .pickerStyle(.menu)
        .labelsHidden()
        .fixedSize()
    }

    /// EditorHair.
    @ViewBuilder private var hair: some View {
        label("Hair Color")
        colorGrid(Self.hairColors, $mii.hairColor)
        Toggle("Mirror Hair", isOn: $mii.hairFlipped).fixedSize()
        label("Hair Type")
        choiceGrid(MiiRanges.hairTypes, $mii.hairType) { index in head { $0.hairType = index } }
    }

    /// EditorEyebrows.
    @ViewBuilder private var eyebrows: some View {
        label("Hair Color")
        colorGrid(Self.hairColors, $mii.eyebrowColor)
        stepper("Vertical Position (Up/Down)", $mii.eyebrowVertical, MiiRanges.eyebrowVertical, display: { ($0 - 10) * -1 }, icons: Self.vertical)
        stepper("Rotation (Rotate Left/Right)", $mii.eyebrowRotation, MiiRanges.eyebrowRotation, display: { $0 - 6 }, icons: Self.rotation)
        stepper("Size", $mii.eyebrowSize, MiiRanges.eyebrowSize, display: { $0 }, icons: Self.size)
        stepper("Spacing in between", $mii.eyebrowSpacing, MiiRanges.eyebrowSpacing, display: { $0 }, icons: Self.size)
        label("Eyebrow Type")
        choiceGrid(MiiRanges.eyebrowTypes, $mii.eyebrowType) { index in part(.eyebrow, index, none: Self.noEyebrows) }
    }

    /// EditorEyes.
    @ViewBuilder private var eyes: some View {
        label("Eye Color")
        colorGrid(Self.eyeColors, $mii.eyeColor)
        stepper("Vertical Position (Up/Down)", $mii.eyeVertical, MiiRanges.eyeVertical, display: { ($0 - 12) * -1 }, icons: Self.vertical)
        stepper("Rotation (Rotate Left/Right)", $mii.eyeRotation, MiiRanges.eyeRotation, display: { $0 - 4 }, icons: Self.rotation)
        stepper("Size", $mii.eyeSize, MiiRanges.eyeSize, display: { $0 }, icons: Self.size)
        stepper("Spacing in between", $mii.eyeSpacing, MiiRanges.eyeSpacing, display: { $0 }, icons: Self.size)
        label("Eye Type")
        choiceGrid(MiiRanges.eyeTypes, $mii.eyeType) { index in part(.eye, index) }
    }

    /// EditorNose.
    @ViewBuilder private var nose: some View {
        stepper("Vertical Position (Up/Down)", $mii.noseVertical, MiiRanges.noseVertical, display: { ($0 - 9) * -1 }, icons: Self.vertical)
        stepper("Size", $mii.noseSize, MiiRanges.noseSize, display: { $0 }, icons: Self.size)
        label("Nose Type")
        choiceGrid(MiiRanges.noseTypes, $mii.noseType) { index in part(.nose, index) }
    }

    /// EditorLips.
    @ViewBuilder private var lips: some View {
        label("Lip Color")
        colorGrid(Self.lipColors, $mii.lipColor)
        stepper("Vertical Position (Up/Down)", $mii.lipVertical, MiiRanges.lipVertical, display: { ($0 - 13) * -1 }, icons: Self.vertical)
        stepper("Size", $mii.lipSize, MiiRanges.lipSize, display: { $0 }, icons: Self.size)
        label("Mouth Type")
        choiceGrid(MiiRanges.lipTypes, $mii.lipType) { index in part(.mouth, index) }
    }

    /// EditorGlasses.
    @ViewBuilder private var glasses: some View {
        label("Glasses Type")
        choiceGrid(MiiRanges.glassesTypes, $mii.glassesType) { index in part(.glasses, index, none: 0) }
        label("Glasses Color")
        colorGrid(Self.glassesColors, $mii.glassesColor)
        stepper("Vertical Position (Up/Down)", $mii.glassesVertical, MiiRanges.glassesVertical, display: { ($0 - 10) * -1 }, icons: Self.vertical)
        stepper("Size", $mii.glassesSize, MiiRanges.glassesSize, display: { $0 }, icons: Self.size)
    }

    /// EditorBeardPage.
    @ViewBuilder private var facialHair: some View {
        label("Hair Color")
        colorGrid(Self.hairColors, $mii.facialHairColor)
        label("Mustache Type")
        choiceGrid(MiiRanges.mustacheTypes, $mii.mustacheType) { index in part(.mustache, index, none: 0) }
        stepper("Mustache Vertical Position (Up/Down)", $mii.mustacheVertical, MiiRanges.facialHairVertical, display: { ($0 - 10) * -1 }, icons: Self.vertical)
        stepper("Mustache Size", $mii.mustacheSize, MiiRanges.facialHairSize, display: { $0 }, icons: Self.size)
        label("Beard Type")
        choiceGrid(MiiRanges.beardTypes, $mii.beardType) { index in head { $0.beardType = index } }
    }

    /// EditorMole.
    @ViewBuilder private var mole: some View {
        Toggle("Enable Mole?", isOn: $mii.moleEnabled).fixedSize()
        stepper("Vertical Position (Up/Down)", $mii.moleVertical, MiiRanges.moleVertical, display: { ($0 - 20) * -1 }, icons: Self.vertical)
        stepper("Horizontal Position (Left/Right)", $mii.moleHorizontal, MiiRanges.moleHorizontal, display: { $0 - 8 }, icons: Self.horizontal)
        stepper("Size", $mii.moleSize, MiiRanges.moleSize, display: { $0 }, icons: Self.size)
    }

    // MARK: Fields

    private func label(_ text: String) -> some View {
        Text(text).font(.callout.bold()).padding(.top, 4)
    }

    private var creationDate: String {
        guard mii.miiId != 1, let date = MiiData.creationDate(miiId: mii.miiId) else { return "Unknown" }
        let format = DateFormatter()
        format.locale = Locale(identifier: "en_US_POSIX")
        format.timeZone = TimeZone(identifier: "UTC")
        format.dateFormat = "yyyy-MM-dd HH:mm 'UTC'"
        return format.string(from: date)
    }

    /// Height and Width: 0 to 127, the face redrawn as the slider moves.
    private func scale(_ title: String, _ value: Binding<Int>) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            label(title)
            // Rounded here rather than stepped: visionOS draws a stepped slider's 128 steps as ticks.
            Slider(value: Binding(get: { Double(value.wrappedValue) }, set: { value.wrappedValue = Int($0.rounded()) }),
                   in: 0...Double(MiiRanges.scaleMax))
                .frame(maxWidth: 320)
        }
    }

    /// WheelWizard's transform controls: the value as the PC shows it, between buttons that move it
    /// by one within `range`. `icons` are the decrease and increase buttons' symbols.
    private func stepper(_ title: String, _ value: Binding<Int>, _ range: ClosedRange<Int>,
                         display: (Int) -> Int, icons: (String, String)) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            label(title)
            HStack(spacing: 6) {
                Button { value.wrappedValue -= 1 } label: { Image(systemName: icons.0) }
                    .disabled(!range.contains(value.wrappedValue - 1))
                Text("\(display(value.wrappedValue))")
                    .monospacedDigit()
                    .frame(width: 44)
                Button { value.wrappedValue += 1 } label: { Image(systemName: icons.1) }
                    .disabled(!range.contains(value.wrappedValue + 1))
            }
            .buttonStyle(.bordered)
        }
    }

    /// The PC's paint-brush buttons: one swatch per colour.
    private func colorGrid(_ colors: [Color], _ selection: Binding<Int>) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.fixed(44), spacing: 10), count: min(colors.count, 6)), alignment: .leading, spacing: 10) {
            ForEach(colors.indices, id: \.self) { index in
                Button {
                    selection.wrappedValue = index
                } label: {
                    Circle()
                        .fill(colors[index])
                        .overlay(Circle().strokeBorder(Color.secondary.opacity(0.6), lineWidth: 1))
                        .frame(width: 28, height: 28)
                        .frame(width: 44, height: 44)
                        .background(RoundedRectangle(cornerRadius: 10).strokeBorder(index == selection.wrappedValue ? Color.accentColor : .clear, lineWidth: 3))
                        .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 10))
                }
                .buttonStyle(.plain)
                .hoverEffect()
                .accessibilityAddTraits(index == selection.wrappedValue ? .isSelected : [])
            }
        }
    }

    /// The PC's icon buttons: `count` choices, five to a row, each pictured by `picture`.
    private func choiceGrid<Picture: View>(_ count: Int, _ selection: Binding<Int>,
                                           @ViewBuilder picture: @escaping (Int) -> Picture) -> some View {
        LazyVGrid(columns: Array(repeating: GridItem(.fixed(Self.choiceSide), spacing: 8), count: 5), alignment: .leading, spacing: 8) {
            ForEach(0..<count, id: \.self) { index in
                Button {
                    selection.wrappedValue = index
                } label: {
                    ZStack {
                        if installed {
                            picture(index)
                        } else {
                            // Without the parts there is nothing to draw: the PC's order, numbered.
                            Text("\(index + 1)").foregroundStyle(.secondary)
                        }
                    }
                    .frame(width: Self.choiceSide, height: Self.choiceSide)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Color.primary.opacity(0.06)))
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                    .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(index == selection.wrappedValue ? Color.accentColor : .clear, lineWidth: 3))
                    .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 10))
                }
                .buttonStyle(.plain)
                .hoverEffect()
                .accessibilityLabel("\(index + 1)")
                .accessibilityAddTraits(index == selection.wrappedValue ? .isSelected : [])
            }
        }
    }

    /// Pictures a choice as the Mii's head wearing it, without the body, enlarged past the cell's
    /// edges so the head fills it.
    private func head(_ wearing: (inout Mii) -> Void) -> some View {
        var variant = mii
        wearing(&variant)
        return MiiPicture(mii: variant, side: Self.choiceSide, withBody: false, zoom: Self.headZoom, installed: installed) {
            EmptyView()
        }
    }

    /// Pictures a flat part's choice from its texture, on the Mii's skin; `none` is the choice
    /// that is no part at all.
    @ViewBuilder private func part(_ part: MiiRenderer.Part, _ index: Int, none: Int = -1) -> some View {
        if index == none {
            Image(systemName: "xmark").font(.title2).foregroundStyle(.secondary)
        } else {
            MiiPartIcon(mii: mii, part: part, index: index, side: Self.choiceSide - 20)
                .padding(6)
                .background(RoundedRectangle(cornerRadius: 6).fill(Self.skinColors[min(max(mii.skinColor, 0), Self.skinColors.count - 1)]))
        }
    }

    // MARK: Values

    /// Mii names are stored as ten UTF-16 code units.
    private static func limited(_ text: String) -> String {
        var result = ""
        for character in text {
            guard result.utf16.count + String(character).utf16.count <= MiiRanges.nameLength else { break }
            result.append(character)
        }
        return result
    }

    private static func validName(_ text: String) -> Bool {
        (3...MiiRanges.nameLength).contains(text.trimmingCharacters(in: .whitespacesAndNewlines).utf16.count)
    }

    private static let vertical = ("arrow.up", "arrow.down")
    private static let horizontal = ("arrow.left", "arrow.right")
    private static let rotation = ("rotate.left", "rotate.right")
    private static let size = ("minus", "plus")

    private static let facialFeatures = [
        "None", "Cheeks", "Cheek and eyes", "Freckles", "Baggy eyes", "Chad", "Tired", "Chin", "Eye shadow", "Beard",
        "Mouth corners", "Wrinkles",
    ]

    private static func rgb(_ r: Int, _ g: Int, _ b: Int) -> Color {
        Color(.sRGB, red: Double(r) / 255, green: Double(g) / 255, blue: Double(b) / 255)
    }

    // MiiColorMappings on the PC, in the Wii's order of each colour.
    private static let favoriteColors = [
        rgb(252, 33, 20), rgb(255, 119, 27), rgb(255, 237, 33), rgb(143, 240, 31), rgb(0, 130, 50), rgb(10, 80, 184),
        rgb(71, 186, 225), rgb(255, 98, 126), rgb(138, 42, 176), rgb(87, 62, 23), rgb(255, 255, 250), rgb(0, 0, 0),
    ]
    private static let skinColors = [
        rgb(255, 211, 157), rgb(255, 185, 99), rgb(222, 123, 61), rgb(255, 171, 128), rgb(200, 83, 39), rgb(117, 46, 23),
    ]
    private static let hairColors = [
        rgb(0, 0, 0), rgb(86, 45, 27), rgb(120, 37, 21), rgb(157, 74, 32), rgb(152, 139, 140), rgb(104, 78, 27),
        rgb(171, 106, 36), rgb(255, 183, 87),
    ]
    private static let eyeColors = [rgb(0, 0, 0), rgb(0x47, 0x4B, 0x5D), rgb(150, 72, 45), rgb(165, 152, 55), rgb(85, 93, 195), rgb(72, 143, 100)]
    private static let lipColors = [rgb(255, 93, 13), rgb(255, 18, 13), rgb(255, 83, 77)]
    private static let glassesColors = [rgb(144, 144, 144), rgb(202, 147, 102), rgb(255, 87, 77), rgb(123, 135, 189), rgb(255, 175, 71), rgb(220, 197, 190)]
}

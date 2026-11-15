// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UniformTypeIdentifiers

/// The Miis tab's state: the game's Mii database (MiiDatabase), the Mii parts its pictures are
/// drawn from (MiiRenderResource), and what the player has selected. Database work runs off the
/// main thread, one change at a time, as the Quest launcher's MiisPage does it.
@MainActor
final class MiiStore: ObservableObject {
    struct AlertMessage: Identifiable {
        let id = UUID()
        let title: String
        let message: String
    }

    /// A Mii open in the editor, and what Save does with it.
    struct EditorSession: Identifiable, Hashable {
        let id = UUID()
        let mii: Mii
        let isNew: Bool
        let save: @MainActor (Mii) -> Void

        static func == (a: EditorSession, b: EditorSession) -> Bool { a.id == b.id }
        func hash(into hasher: inout Hasher) { hasher.combine(id) }
    }

    enum NewMii: CaseIterable {
        case random, male, female

        var title: String {
            switch self {
            case .random: return "Randomize"
            case .male: return "Male"
            case .female: return "Female"
            }
        }
    }

    /// Favourites first, otherwise in the database's order (MiiListPage).
    @Published private(set) var miis: [Mii] = []
    /// Why the database cannot be opened, if it cannot.
    @Published private(set) var loadError: String?
    @Published private(set) var loaded = false
    /// The selected Miis' IDs, in the order they were picked.
    @Published private(set) var selected: [UInt32] = []
    /// Whether a tap adds a Mii to the selection (or takes it out) rather than selecting it alone.
    @Published var selecting = false
    /// The console's system ID, which marks the Miis made on this headset; nil before the console exists.
    @Published private(set) var consoleSystemId: UInt32?
    @Published private(set) var partsInstalled = MiiRenderResource.installed
    @Published private(set) var partsComplete = MiiRenderResource.complete
    /// The bytes downloaded so far while the Mii parts download, of `partsTotal`.
    @Published private(set) var partsProgress: Int64?
    @Published private(set) var partsTotal: Int64 = 0
    @Published private(set) var partsError = ""
    @Published var alert: AlertMessage?
    /// A passing word on what a change did, as the Quest shows a toast.
    @Published private(set) var notice = ""
    @Published var editor: EditorSession?
    /// Miis waiting for the player to choose where Export saves them.
    @Published private(set) var exportFiles: [URL] = []
    @Published var exporting = false

    private let worker = DispatchQueue(label: "MiiDatabase", qos: .userInitiated)
    private var noticeTask: Task<Void, Never>?
    private var partsTask: Task<Void, Never>?

    static let defaultName = "New Mii"

    var nand: URL { GameStorage.nandDirectory }
    var databasePath: String { MiiDatabase.file(nand: nand).path }
    var selectedMiis: [Mii] { selected.compactMap { id in miis.first { $0.miiId == id } } }
    var partsDownloadBytes: Int64 { MiiRenderResource.downloadBytes }

    // MARK: The list

    /// Reads the database again, creating it empty the first time.
    func load() {
        refreshParts()
        let nand = self.nand
        Task {
            let result = await run {
                let file = MiiDatabase.file(nand: nand)
                try MiiDatabase.create(file)
                return (try MiiDatabase.miis(file), MiiIds.consoleMac(nand: nand).map { MiiIds.systemId(mac: $0) })
            }
            loaded = true
            switch result {
            case .success(let (all, systemId)):
                consoleSystemId = systemId
                show(all)
            case .failure(let error):
                miis = []
                selected = []
                loadError = error.localizedDescription
            }
        }
    }

    private func show(_ all: [Mii]) {
        loadError = nil
        // A stable sort: favourites first, the rest in the database's order.
        miis = all.enumerated().sorted { a, b in
            a.element.favorite != b.element.favorite ? a.element.favorite : a.offset < b.offset
        }.map(\.element)
        selected.removeAll { id in !miis.contains { $0.miiId == id } }
    }

    /// MiiExtensions.IsGlobal: a special Mii, or one another console made.
    func isForeign(_ mii: Mii) -> Bool {
        if mii.miiId >> 29 == 0b110 { return true }
        guard let own = consoleSystemId else { return false }
        return mii.systemId != own
    }

    func isSelected(_ mii: Mii) -> Bool { selected.contains(mii.miiId) }

    /// A tap selects only that Mii, or nothing when it was the one selected; while selecting
    /// several, it adds the Mii or takes it out.
    func select(_ mii: Mii) {
        let wasSelected = isSelected(mii)
        if !selecting { selected.removeAll() }
        if wasSelected {
            selected.removeAll { $0 == mii.miiId }
        } else {
            selected.append(mii.miiId)
        }
    }

    func clearSelection() {
        selected.removeAll()
    }

    // MARK: Actions

    func toggleFavorite(_ chosen: [Mii]) {
        guard !chosen.isEmpty else { return }
        let favorite = !chosen.allSatisfy(\.favorite)
        change { file in
            for var mii in chosen {
                mii.favorite = favorite
                try MiiDatabase.update(file, mii)
            }
        }
    }

    func edit(_ mii: Mii) {
        editor = EditorSession(mii: mii, isNew: false) { [weak self] edited in
            self?.change { file in try MiiDatabase.update(file, edited) }
        }
    }

    /// MiiListPage.CreateNewMii: a random, male or female start, then the editor.
    func create(_ kind: NewMii) {
        withConsoleMac { [weak self] mac in
            let name = MiiStore.defaultName
            let mii: Mii = switch kind {
            case .random: MiiFactory.random(name)
            case .male: MiiFactory.male(name)
            case .female: MiiFactory.female(name)
            }
            self?.editor = EditorSession(mii: mii, isNew: true) { [weak self] edited in
                self?.change { file in try MiiDatabase.add(file, MiiStore.stamped(edited, mac: mac)) }
            }
        }
    }

    /// MiiListPage.DuplicateMii: copies with new IDs, as this headset's own Miis.
    func duplicate(_ chosen: [Mii]) {
        guard !chosen.isEmpty else { return }
        withConsoleMac { [weak self] mac in
            let message = chosen.count == 1 ? "Created duplicate of '\(chosen[0].name)'" : "Created \(chosen.count) duplicate Miis"
            self?.change({ file in
                for mii in chosen { try MiiDatabase.add(file, MiiStore.stamped(mii, mac: mac)) }
            }) { [weak self] _ in self?.say(message) }
        }
    }

    /// MiiListPage.DeleteMii, once the player confirmed; never favourites.
    func delete(_ chosen: [Mii]) {
        guard !chosen.isEmpty else { return }
        let message = chosen.count == 1 ? "Deleted '\(chosen[0].name)'" : "Deleted \(chosen.count) Miis"
        change({ file in
            for mii in chosen { try MiiDatabase.remove(file, miiId: mii.miiId) }
        }) { [weak self] _ in self?.say(message) }
    }

    /// The files the player picked with Import: .mii files, 74 bytes each. As on the PC, each is
    /// added as a copy with a new ID, marked as made on another console. Unlike the PC, one bad
    /// file does not stop the others.
    func importFiles(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        change({ file -> [String] in
            var problems: [String] = []
            for url in urls {
                let scoped = url.startAccessingSecurityScopedResource()
                defer { if scoped { url.stopAccessingSecurityScopedResource() } }
                do {
                    var mii = try MiiData.parse(try Data(contentsOf: url))
                    mii.invalid = false
                    mii.systemId = MiiIds.systemId(mac: MiiIds.importMac)
                    mii.miiId = MiiIds.newMiiId()
                    try MiiDatabase.add(file, mii)
                } catch {
                    problems.append("\(url.lastPathComponent): \(error.localizedDescription)")
                }
            }
            return problems
        }) { [weak self] problems in
            let added = urls.count - problems.count
            let summary = added == 1 ? "Imported 1 Mii" : "Imported \(added) Miis"
            if problems.isEmpty {
                self?.say(summary)
            } else {
                self?.alert = AlertMessage(title: summary, message: problems.joined(separator: "\n"))
            }
        }
    }

    /// MiiListPage.ExportMultipleMiiFiles: each Mii as its 74 bytes in a .mii file, written to a
    /// folder of ours first and moved to where the player chooses.
    func export(_ chosen: [Mii]) {
        guard !chosen.isEmpty else { return }
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("MiiExport-\(UUID().uuidString)", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            var files: [URL] = []
            for mii in chosen {
                var name = MiiStore.fileName(mii)
                var copy = 2
                while files.contains(where: { $0.lastPathComponent == name }) {
                    name = (MiiStore.fileName(mii) as NSString).deletingPathExtension + " \(copy).mii"
                    copy += 1
                }
                let url = folder.appendingPathComponent(name)
                try MiiData.serialize(mii).write(to: url)
                files.append(url)
            }
            exportFiles = files
            exporting = true
        } catch {
            try? FileManager.default.removeItem(at: folder)
            alert = AlertMessage(title: "The Miis could not be saved", message: error.localizedDescription)
        }
    }

    /// Where the Export files went, or why they did not.
    func exported(_ result: Result<[URL], Error>) {
        let count = exportFiles.count
        if let folder = exportFiles.first?.deletingLastPathComponent() {
            try? FileManager.default.removeItem(at: folder)
        }
        exportFiles = []
        switch result {
        case .success(let moved):
            if moved.isEmpty { return }
            say(count == 1 ? "Saved Mii '\(moved[0].deletingPathExtension().lastPathComponent)'" : "Saved \(count) Miis")
        case .failure(let error):
            if (error as? CocoaError)?.code == .userCancelled { return }
            alert = AlertMessage(title: "The Miis could not be saved", message: error.localizedDescription)
        }
    }

    /// A new Mii of the database: valid, from `mac`'s console, with a fresh ID.
    nonisolated private static func stamped(_ mii: Mii, mac: [UInt8]) -> Mii {
        var stamped = mii
        stamped.invalid = false
        stamped.systemId = MiiIds.systemId(mac: mac)
        stamped.miiId = MiiIds.newMiiId()
        return stamped
    }

    /// CustomCharactersService.NormalizeToAscii and ReplaceInvalidFileNameChars, then .mii.
    static func fileName(_ mii: Mii) -> String {
        let ascii = String(String.UnicodeScalarView(mii.name.decomposedStringWithCanonicalMapping.unicodeScalars.filter {
            (0x20...0x7E).contains($0.value)
        }))
        let joined = ascii.split(whereSeparator: { "/\\:*?\"<>|".contains($0) }).joined(separator: "_")
            .trimmingCharacters(in: .whitespaces)
        return (joined.isEmpty ? "Mii" : joined) + ".mii"
    }

    /// The MAC address Miis made on this headset are stamped with (MiiDbService.AddToDatabase),
    /// creating the console the first time (MiiIds.ensureConsoleMac), then `use` on the main actor.
    private func withConsoleMac(_ use: @escaping @MainActor ([UInt8]) -> Void) {
        let nand = self.nand
        Task {
            switch await run({ try MiiIds.ensureConsoleMac(nand: nand) }) {
            case .success(let mac):
                consoleSystemId = MiiIds.systemId(mac: mac)
                use(mac)
            case .failure(let error):
                alert = AlertMessage(title: "A Mii cannot be made yet", message: error.localizedDescription)
            }
        }
    }

    /// Changes the database off the main thread, then lists the Miis again and hands the change's
    /// result to `done`. A failure is shown instead.
    private func change<T>(_ work: @escaping @Sendable (URL) throws -> T, done: @escaping @MainActor (T) -> Void = { _ in }) {
        let file = MiiDatabase.file(nand: nand)
        Task {
            switch await run({ try work(file) }) {
            case .success(let value):
                done(value)
            case .failure(let error):
                alert = AlertMessage(title: "The Mii database could not be changed", message: error.localizedDescription)
            }
            load()
        }
    }

    private func run<T>(_ work: @escaping @Sendable () throws -> T) async -> Result<T, Error> {
        await withCheckedContinuation { continuation in
            worker.async { continuation.resume(returning: Result { try work() }) }
        }
    }

    private func say(_ message: String) {
        notice = message
        noticeTask?.cancel()
        noticeTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(4))
            if !Task.isCancelled { self?.notice = "" }
        }
    }

    // MARK: The Mii parts

    func refreshParts() {
        partsInstalled = MiiRenderResource.installed
        partsComplete = MiiRenderResource.complete
    }

    var downloadingParts: Bool { partsTask != nil }

    /// Downloads the parts, or the bodies once the parts are here.
    func downloadParts() {
        guard partsTask == nil else { return }
        partsError = ""
        partsProgress = 0
        partsTotal = max(MiiRenderResource.downloadBytes, 1)
        partsTask = Task {
            do {
                try await MiiRenderResource.install { bytes in
                    Task { @MainActor in
                        if self.partsTask != nil { self.partsProgress = bytes }
                    }
                }
            } catch is CancellationError {
            } catch {
                partsError = error.localizedDescription
            }
            partsTask = nil
            partsProgress = nil
            MiiImages.shared.clear()
            refreshParts()
        }
    }
}

/// My Miis: WheelWizard's MiiListPage for the headset, as the Quest launcher has it (MiisPage.kt).
/// It lists the Miis of the game's own Mii database, favourites first, and makes, edits,
/// duplicates, imports, exports and deletes them. The game reads the same file, so a Mii made
/// here can be picked for a licence.
struct MiisView: View {
    @EnvironmentObject private var model: GameModel
    @StateObject private var store = MiiStore()
    @State private var choosingNew = false
    @State private var importing = false
    @State private var deleting: [Mii] = []
    @State private var confirmingDelete = false

    private static let miiType = UTType(filenameExtension: "mii", conformingTo: .data) ?? .data
    private static let tileSide: CGFloat = 104

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("Your Mii is who other players see online: its face and its name. Make one here, then pick it in the game, when you create a licence or later in License Settings > Change Mii. Both games read these Miis.")
                    .font(.callout)
                    .foregroundStyle(.secondary)

                if !store.partsComplete {
                    partsView
                }

                if let error = store.loadError {
                    GroupBox("No Miis yet!") {
                        Label("The Mii database in the game's NAND cannot be opened: \(error)", systemImage: "xmark.octagon.fill")
                            .foregroundStyle(.red)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                } else if store.loaded {
                    actions
                    grid
                }
            }
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle(store.loaded && store.loadError == nil ? "My Miis (\(store.miis.count) of \(MiiDatabase.slots))" : "My Miis")
        .toolbar {
            // Photos' Select: taps then add Miis to the selection or take them out.
            ToolbarItem(placement: .topBarTrailing) {
                if store.loaded && store.loadError == nil && !store.miis.isEmpty {
                    Button(store.selecting ? "Done" : "Select") { store.selecting.toggle() }
                }
            }
        }
        .navigationDestination(item: $store.editor) { session in
            MiiEditorView(session: session, installed: store.partsInstalled)
        }
        .onAppear { store.load() }
        .confirmationDialog("New Mii", isPresented: $choosingNew, titleVisibility: .visible) {
            ForEach(MiiStore.NewMii.allCases, id: \.self) { kind in
                Button(kind.title) { store.create(kind) }
            }
            Button("Cancel", role: .cancel) {}
        }
        .alert(deleteTitle, isPresented: $confirmingDelete) {
            Button("Delete", role: .destructive) { store.delete(deleting) }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Deleting is permanent and cannot be undone.")
        }
        .alert(store.alert?.title ?? "", isPresented: Binding(get: { store.alert != nil }, set: { if !$0 { store.alert = nil } }),
               presenting: store.alert) { _ in
            Button("OK", role: .cancel) {}
        } message: { alert in
            Text(alert.message)
        }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.data], allowsMultipleSelection: true) { result in
            switch result {
            case .success(let urls): store.importFiles(urls)
            case .failure(let error): store.alert = MiiStore.AlertMessage(title: "The Miis could not be imported", message: error.localizedDescription)
            }
        }
        .fileMover(isPresented: $store.exporting, files: store.exportFiles) { result in
            store.exported(result)
        }
    }

    private var gameBusy: Bool { model.phase == .running || model.phase == .opening }

    /// Changes are refused while the game runs: it reads the Mii database.
    private func whenGameClosed(_ action: () -> Void) {
        if gameBusy {
            store.alert = MiiStore.AlertMessage(title: "Close the game first", message: "The game reads the Mii database while it runs.")
        } else {
            action()
        }
    }

    // MARK: Sections

    /// MiiListPage.ChangeTopButtons: what can be done with the selection, or without one.
    private var actions: some View {
        let chosen = store.selectedMiis
        let unfavorite = !chosen.isEmpty && chosen.allSatisfy(\.favorite)
        return VStack(alignment: .leading, spacing: 10) {
            // One line of buttons, which scrolls sideways in a window made narrow.
            ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 12) {
                if chosen.isEmpty {
                    Button {
                        whenGameClosed { choosingNew = true }
                    } label: {
                        Label("New Mii", systemImage: "plus")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(store.miis.count >= MiiDatabase.slots)
                    Button {
                        whenGameClosed { importing = true }
                    } label: {
                        Label("Import", systemImage: "square.and.arrow.down")
                    }
                    .buttonStyle(.bordered)
                } else {
                    if chosen.count == 1 {
                        Button {
                            whenGameClosed { store.edit(chosen[0]) }
                        } label: {
                            Label("Edit", systemImage: "pencil")
                        }
                        .buttonStyle(.borderedProminent)
                    }
                    Button {
                        whenGameClosed { store.toggleFavorite(chosen) }
                    } label: {
                        Label(unfavorite ? "Unfavorite" : "Favorite", systemImage: unfavorite ? "star.slash" : "star")
                    }
                    .buttonStyle(.bordered)
                    Button {
                        whenGameClosed { store.duplicate(chosen) }
                    } label: {
                        Label("Duplicate", systemImage: "plus.square.on.square")
                    }
                    .buttonStyle(.bordered)
                    .disabled(store.miis.count + chosen.count > MiiDatabase.slots)
                    Button {
                        store.export(chosen)
                    } label: {
                        Label("Export", systemImage: "square.and.arrow.up")
                    }
                    .buttonStyle(.bordered)
                    Button(role: .destructive) {
                        whenGameClosed { askDelete(chosen) }
                    } label: {
                        Label("Delete", systemImage: "trash")
                    }
                    .buttonStyle(.bordered)
                }
            }
            .lineLimit(1)
            .fixedSize()
            .padding(.vertical, 4)
            }
            if !store.notice.isEmpty {
                Label(store.notice, systemImage: "checkmark.circle.fill").foregroundStyle(.green)
            }
        }
    }

    private var grid: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: Self.tileSide + 24), spacing: 16)], alignment: .leading, spacing: 16) {
            ForEach(store.miis, id: \.miiId) { mii in
                tile(mii)
            }
            if store.miis.count < MiiDatabase.slots {
                Button {
                    whenGameClosed {
                        store.clearSelection()
                        choosingNew = true
                    }
                } label: {
                    VStack(spacing: 8) {
                        Image(systemName: "plus")
                            .font(.system(size: 36, weight: .medium))
                            .frame(width: Self.tileSide, height: Self.tileSide)
                        Text("New Mii").lineLimit(1)
                    }
                    .padding(10)
                    .foregroundStyle(.secondary)
                    .background(RoundedRectangle(cornerRadius: 18).strokeBorder(.secondary.opacity(0.5), style: StrokeStyle(lineWidth: 2, dash: [6, 5])))
                    .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 18))
                }
                .buttonStyle(.plain)
                .hoverEffect()
            }
        }
    }

    private func tile(_ mii: Mii) -> some View {
        let selected = store.isSelected(mii)
        return Button {
            store.select(mii)
        } label: {
            VStack(spacing: 8) {
                MiiPicture(mii: mii, side: Self.tileSide, installed: store.partsInstalled) {
                    Image(systemName: "person.fill")
                        .font(.system(size: 48))
                        .foregroundStyle(.secondary)
                }
                Text(mii.name).lineLimit(1)
            }
            .padding(10)
            .overlay(alignment: .topTrailing) {
                if mii.favorite {
                    Image(systemName: "star.fill").foregroundStyle(.yellow).padding(10)
                }
            }
            .overlay(alignment: .topLeading) {
                if store.isForeign(mii) {
                    Image(systemName: "globe").foregroundStyle(.secondary).padding(10)
                }
            }
            .background(RoundedRectangle(cornerRadius: 18).fill(selected ? Color.accentColor.opacity(0.35) : Color.primary.opacity(0.06)))
            .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(selected ? Color.accentColor : .clear, lineWidth: 2))
            .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 18))
        }
        .buttonStyle(.plain)
        .hoverEffect()
        .accessibilityLabel(mii.name)
        .accessibilityAddTraits(selected ? .isSelected : [])
        .contextMenu {
            Button { whenGameClosed { store.edit(mii) } } label: { Label("Edit", systemImage: "pencil") }
            Button { whenGameClosed { store.toggleFavorite([mii]) } } label: {
                Label(mii.favorite ? "Unfavorite" : "Favorite", systemImage: mii.favorite ? "star.slash" : "star")
            }
            Button { whenGameClosed { store.duplicate([mii]) } } label: { Label("Duplicate", systemImage: "plus.square.on.square") }
            Button { store.export([mii]) } label: { Label("Export", systemImage: "square.and.arrow.up") }
            Button(role: .destructive) { whenGameClosed { askDelete([mii]) } } label: { Label("Delete", systemImage: "trash") }
        }
    }

    /// The download offer: the parts and bodies, or only the bodies once the parts are here.
    private var partsView: some View {
        GroupBox("Mii pictures") {
            VStack(alignment: .leading, spacing: 8) {
                Text(store.partsInstalled
                     ? "Wheel Wizard draws each Mii with its upper body, from the 3DS Mii bodies in its own repository (\(Self.bytes(store.partsDownloadBytes))). Download them once to see Miis the same way here."
                     : "Mii pictures are drawn from FFL's Mii parts and the 3DS Mii bodies (\(Self.bytes(store.partsDownloadBytes))), the files Wheel Wizard uses for the same reason: the parts from a copy of Miitomo's kept by the Internet Archive, the bodies from Wheel Wizard's own repository. They are downloaded once and stay on this headset. Until then, Miis show as silhouettes and the editor's choices as numbers.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                if let bytes = store.partsProgress {
                    let total = store.partsTotal
                    VStack(alignment: .leading, spacing: 4) {
                        HStack {
                            Text(store.partsInstalled ? "Downloading the Mii bodies" : "Downloading the Mii parts").font(.callout.bold())
                            Spacer()
                            Text("\(Self.bytes(bytes)) of \(Self.bytes(total))").font(.callout).foregroundStyle(.secondary)
                        }
                        ProgressView(value: min(Double(bytes) / Double(total), 1))
                    }
                    .padding(.top, 4)
                }
                if !store.partsError.isEmpty {
                    Label("The Mii parts could not be downloaded: \(store.partsError)", systemImage: "xmark.octagon.fill")
                        .foregroundStyle(.red)
                }
                Button("Download") { store.downloadParts() }
                    .buttonStyle(.borderedProminent)
                    .disabled(store.downloadingParts)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }

    private var deleteTitle: String {
        deleting.count == 1 ? "Are you sure you want to delete \(deleting[0].name)?" : "Are you sure you want to delete \(deleting.count) Miis?"
    }

    private func askDelete(_ chosen: [Mii]) {
        guard !chosen.isEmpty else { return }
        if chosen.contains(where: \.favorite) {
            store.alert = MiiStore.AlertMessage(title: "Cannot delete favorite Mii(s)",
                                         message: "One or more of the selected Mii(s) is a favorite. Miis can only be deleted if they are not favorites to prevent accidental deletions.")
            return
        }
        deleting = chosen
        confirmingDelete = true
    }

    private static func bytes(_ count: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: count, countStyle: .file)
    }
}

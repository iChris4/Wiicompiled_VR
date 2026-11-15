// SPDX-License-Identifier: GPL-3.0-or-later

import SwiftUI
import UIKit
import UniformTypeIdentifiers

/// My profiles: WheelWizard's UserProfilePage (Views/Pages/UserProfilePage.axaml.cs) for the
/// headset, as the Quest launcher has it (ProfilesPage.kt). The four licences of Retro Rewind's
/// save, one shown at a time with its Mii, name, friend code and badges, beside its VR history and
/// its numbers. As on the PC, a licence is Online, with a glow, while its friend code is in a Retro
/// WFC room, and one licence is primary: the one the tab opens on.
///
/// Nothing here renames a licence or changes its Mii, which the game does in License Settings; the
/// PC's region picker is left out too, since this build runs PAL only. What the PC and the Quest do
/// not have: Import and Export move the whole profile between devices, its identity included
/// (ProfileTransfer), with what an import replaces backed up first.
struct ProfilesView: View {
    @EnvironmentObject private var model: GameModel
    @StateObject private var store = ProfileStore()
    @State private var importing = false
    /// The carousel's page: the VR history, then the numbers.
    @State private var page = 0
    @State private var notice = ""
    @State private var noticeTask: Task<Void, Never>?

    private static let pictureSide: CGFloat = 260

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("Your Retro Rewind licences, as its save holds them and Retro WFC sees them online. Rename a licence or change its Mii in the game's License Settings.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                transfer
                if let snapshot = store.snapshot, snapshot.any, let license = store.current {
                    slots(snapshot)
                    HStack(alignment: .top, spacing: 28) {
                        card(license)
                            .frame(width: 300)
                        VStack(alignment: .leading, spacing: 16) {
                            Picker("Page", selection: $page) {
                                Text("VR History").tag(0)
                                Text("Stats").tag(1)
                            }
                            .pickerStyle(.segmented)
                            .labelsHidden()
                            .fixedSize()
                            if page == 0 {
                                VrHistoryView(friendCode: license.friendCode, generation: store.generation)
                            } else {
                                stats(license)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                } else if store.loaded {
                    GroupBox("No profiles") {
                        Text("You have to play Retro Rewind at least once in order to see your profiles listed here, or import a profile from another device.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            .padding(28)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .navigationTitle("My profiles")
        .onAppear {
            store.refresh()
            store.watchRooms()
        }
        .onDisappear { store.stopWatchingRooms() }
        .fileImporter(isPresented: $importing, allowedContentTypes: [.zip]) { result in
            switch result {
            case .success(let url): store.inspect(url)
            case .failure(let error): store.message = ProfileStore.Message(title: "This profile cannot be imported", text: error.localizedDescription)
            }
        }
        .fileMover(isPresented: $store.exporting, files: store.exportFiles) { store.exported($0) }
        .sheet(item: $store.pendingImport) { pending in
            ImportSheet(pending: pending) {
                whenGameClosed { store.confirmImport(pending) }
            } cancel: {
                store.pendingImport = nil
            }
        }
        .alert(store.message?.title ?? "", isPresented: Binding(get: { store.message != nil }, set: { if !$0 { store.message = nil } }),
               presenting: store.message) { _ in
            Button("OK", role: .cancel) {}
        } message: { message in
            Text(message.text)
        }
    }

    /// Import and Export: the whole profile, from or to a zip the player picks.
    private var transfer: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                Button {
                    whenGameClosed { importing = true }
                } label: {
                    Label("Import…", systemImage: "square.and.arrow.down")
                }
                Button {
                    whenGameClosed { store.export(store.current) }
                } label: {
                    Label("Export…", systemImage: "square.and.arrow.up")
                }
                if let doing = store.transferring {
                    ProgressView()
                    Text(doing).font(.callout).foregroundStyle(.secondary)
                }
            }
            .buttonStyle(.bordered)
            .disabled(store.transferring != nil)
            if !store.notice.isEmpty {
                Label(store.notice, systemImage: "checkmark.circle.fill").font(.callout).foregroundStyle(.green)
            }
        }
    }

    /// The saves and the NAND are the game's while it runs.
    private func whenGameClosed(_ action: () -> Void) {
        if model.phase == .running || model.phase == .opening {
            store.message = ProfileStore.Message(title: "Close the game first", text: "The game uses its saves and its NAND while it runs.")
        } else {
            action()
        }
    }

    // MARK: The licences

    /// One tab per licence slot; an empty slot reads No license and cannot be picked.
    private func slots(_ snapshot: ProfileStore.Snapshot) -> some View {
        HStack(spacing: 8) {
            ForEach(snapshot.licenses.indices, id: \.self) { index in
                let license = snapshot.licenses[index]
                let chosen = index == store.slot
                Button {
                    store.slot = index
                } label: {
                    Text(license.map(ProfileStore.displayName) ?? "No license")
                        .italic(license == nil)
                        .lineLimit(1)
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 10)
                        .background(RoundedRectangle(cornerRadius: 12).fill(chosen ? Color.accentColor.opacity(0.35) : Color.primary.opacity(0.06)))
                        .foregroundStyle(license == nil ? .secondary : .primary)
                        .contentShape(.hoverEffect, RoundedRectangle(cornerRadius: 12))
                }
                .buttonStyle(.plain)
                .hoverEffect()
                .disabled(license == nil)
                .accessibilityAddTraits(chosen ? .isSelected : [])
            }
        }
    }

    /// UserProfilePage's card: the Mii turned three-quarters (CurrentUserSideProfile) in a frame
    /// that glows while the licence is online, the name, the friend code, Make Primary and the badges.
    private func card(_ license: RksysProfiles.License) -> some View {
        let online = store.isOnline(license)
        let primary = store.primarySlot == license.slot
        return VStack(alignment: .leading, spacing: 14) {
            miiPicture(license)
                .frame(width: Self.pictureSide, height: Self.pictureSide)
                .background(RoundedRectangle(cornerRadius: 24).fill(Color.primary.opacity(0.06)))
                .overlay(RoundedRectangle(cornerRadius: 24).strokeBorder(online ? Color.wheelWizard : Color.secondary.opacity(0.3), lineWidth: online ? 3 : 1))
                .shadow(color: online ? Color.wheelWizard.opacity(0.6) : .clear, radius: 18)
                .accessibilityLabel(online ? "Online" : "Offline")
            if online {
                Label("Online", systemImage: "circle.fill")
                    .font(.callout)
                    .foregroundStyle(Color.wheelWizard)
            }
            Text(ProfileStore.displayName(license))
                .font(.title.bold())
                .lineLimit(1)
            if !license.friendCode.isEmpty {
                HStack(spacing: 10) {
                    Text(license.friendCode)
                        .font(.title3.monospacedDigit())
                        .textSelection(.enabled)
                    Button {
                        UIPasteboard.general.string = license.friendCode
                        say("Copied friend code to clipboard")
                    } label: {
                        Image(systemName: "doc.on.doc")
                    }
                    .buttonStyle(.bordered)
                    .help("Copy friend code")
                    .accessibilityLabel("Copy friend code")
                }
            }
            VStack(alignment: .leading, spacing: 4) {
                Button {
                    store.makePrimary(license.slot)
                    say("Set profile as primary")
                } label: {
                    Label("Make Primary", systemImage: primary ? "largecircle.fill.circle" : "circle")
                }
                .buttonStyle(.bordered)
                .disabled(primary)
                Text("The primary profile is the one this tab opens on.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            let badges = store.badges[license.friendCode] ?? []
            if !badges.isEmpty {
                // Rows of badges within the card, however many a player has.
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 30, maximum: 30), spacing: 6)], alignment: .leading, spacing: 6) {
                    ForEach(badges, id: \.self) { BadgeView(badge: $0).frame(width: 30, height: 30) }
                }
            }
            if !notice.isEmpty {
                Label(notice, systemImage: "checkmark.circle.fill").font(.callout).foregroundStyle(.green)
            }
        }
    }

    /// ProfileStore.picture: the licence's Mii drawn from the parts, Retro WFC's picture of it, or a silhouette.
    @ViewBuilder private func miiPicture(_ license: RksysProfiles.License) -> some View {
        switch store.picture(for: license) {
        case .mii(let mii, let fallback):
            MiiPicture(mii: mii, side: Self.pictureSide, pose: .side, installed: true) {
                if let fallback { remoteImage(fallback) } else { silhouette }
            }
        case .image(let image):
            remoteImage(image)
        case .none:
            silhouette
        }
    }

    private func remoteImage(_ image: CGImage) -> some View {
        Image(decorative: image, scale: 1)
            .resizable()
            .interpolation(.medium)
            .scaledToFit()
            .padding(24)
    }

    private var silhouette: some View {
        Image(systemName: "person.fill")
            .font(.system(size: 110))
            .foregroundStyle(.secondary)
    }

    /// The carousel's second page: the licence's ratings and race counts.
    private func stats(_ license: RksysProfiles.License) -> some View {
        Grid(alignment: .leading, horizontalSpacing: 40, verticalSpacing: 20) {
            GridRow {
                stat("VR", "Versus Rating", "\(license.vr)")
                stat("BR", "Battle Rating", "\(license.br)")
            }
            GridRow {
                stat("Total games won", nil, "\(license.wins)")
                stat("Total games played", nil, "\(license.races)")
            }
        }
        .padding(.top, 8)
    }

    private func stat(_ title: String, _ full: String?, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.callout.bold())
            if let full { Text(full).font(.caption).foregroundStyle(.secondary) }
            Text(value).font(.largeTitle.bold().monospacedDigit())
        }
    }

    private func say(_ message: String) {
        notice = message
        noticeTask?.cancel()
        noticeTask = Task {
            try? await Task.sleep(for: .seconds(3))
            if !Task.isCancelled { notice = "" }
        }
    }
}

/// What an import replaces, for the player to confirm: the archive's licences, each part and what it
/// does here, and what to know before going online with it.
private struct ImportSheet: View {
    let pending: ProfileStore.PendingImport
    let confirm: () -> Void
    let cancel: () -> Void

    private var contents: ProfileTransfer.Contents { pending.contents }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    Text(pending.fileName).font(.callout).foregroundStyle(.secondary)
                    if !contents.licenses.isEmpty {
                        GroupBox("Licences") {
                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(contents.licenses, id: \.slot) { license in
                                    HStack {
                                        Text(ProfileStore.displayName(license)).bold()
                                        Spacer()
                                        Text(license.friendCode.isEmpty ? "Never online" : license.friendCode)
                                            .monospacedDigit()
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    GroupBox("What it puts on this headset") {
                        VStack(alignment: .leading, spacing: 10) {
                            ForEach(contents.parts, id: \.self) { part in
                                Label(describe(part), systemImage: "checkmark.circle")
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    ForEach(warnings, id: \.self) { warning in
                        Label(warning, systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    }
                    Text("Before anything changes, what this headset has now is saved to Backups in the WiiCompiled folder; importing that file brings it back.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .padding(24)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .navigationTitle("Import this profile?")
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel", action: cancel) }
                ToolbarItem(placement: .confirmationAction) { Button("Import", action: confirm) }
            }
        }
        .frame(minWidth: 600, minHeight: 680)
    }

    private func describe(_ part: ProfileTransfer.Part) -> String {
        switch part {
        case .retroRewindSave:
            return "Retro Rewind save: its licences, friends and progress take the place of this headset's."
        case .gameSave:
            return "Mario Kart Wii save, for the unmodded game."
        case .miis:
            return (contents.miiCount == 1 ? "Miis: its Mii joins yours" : "Miis: its \(contents.miiCount) Miis join yours")
                + "; one with the same ID is replaced."
        case .consoleIdentity:
            let console = contents.console ?? "?"
            if console == pending.currentConsole { return "Console identity: the same console as this headset's (\(console))." }
            return "Console identity: this headset becomes console \(console), the one the profile played online from"
                + (pending.currentConsole.map { ", in place of \($0)." } ?? ".")
        case .wifiLogin:
            return "Wi-Fi login (DWC_AUTHDATA): the console's Nintendo WFC user ID."
        case .deviceKeys:
            return "Device keys (keys.bin): the console's certificate."
        case .retroRewindData:
            return "Retro Rewind's ratings, settings and ghosts; ghosts only this headset has are kept."
        }
    }

    private var warnings: [String] {
        var warnings: [String] = []
        let online = contents.licenses.contains { !$0.friendCode.isEmpty }
        if online, !contents.parts.contains(.consoleIdentity) {
            warnings.append("It carries no console identity (setting.txt). Retro WFC checks the console at login and may refuse these licences from this headset (error 22005).")
        }
        if contents.parts.contains(.consoleIdentity), contents.console != pending.currentConsole {
            warnings.append("Play online with this profile on one device at a time: the device it came from goes online as the same console.")
        }
        return warnings
    }
}

/// One of WheelWizard's badges (Views/Components/Badge.axaml), as the Quest's BadgeView draws it: a
/// role badge is two discs with an icon, a tournament medal a medal in gold, silver or bronze. Its
/// tip shows when the eyes rest on it.
struct BadgeView: View {
    let badge: RetroWfc.Badge

    var body: some View {
        GeometryReader { geometry in
            let size = min(geometry.size.width, geometry.size.height)
            content(size)
                .frame(width: size, height: size)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .help(badge.tip)
        .accessibilityLabel(badge.tip)
    }

    @ViewBuilder private func content(_ size: CGFloat) -> some View {
        switch badge {
        case .whWzDev:
            role(size, outer: rgb(0xC7FFF0), inner: rgb(0x01CBA5), icon: "chevron.left.forwardslash.chevron.right", fill: rgb(0x0C554A))
        case .rrDev:
            role(size, outer: rgb(0xFFFDC1), inner: rgb(0xFFC800), icon: "chevron.left.forwardslash.chevron.right", fill: rgb(0x89510A))
        case .translator:
            role(size, outer: rgb(0xE9C7FF), inner: rgb(0x8A01CB), icon: "character.bubble", fill: rgb(0x400C55))
        case .translatorLead:
            ZStack {
                discs(size, outer: rgb(0xFFC7FB), inner: rgb(0xCB01AE))
                Image(systemName: "character.bubble").resizable().scaledToFit()
                    .foregroundStyle(rgb(0x550C48))
                    .frame(width: size * 9 / 14, height: size * 9 / 14)
                    .offset(x: size / 14, y: -size / 28)
                Image(systemName: "star.fill").resizable().scaledToFit()
                    .foregroundStyle(rgb(0x550C48))
                    .frame(width: size * 4 / 14, height: size * 3 / 14)
                    .offset(x: -size * 3.5 / 14, y: 0)
            }
        case .heart:
            ZStack {
                discs(size, outer: rgb(0xA7FCFF), inner: rgb(0xF0F0EC))
                Image(systemName: "heart.fill").resizable().scaledToFit()
                    .foregroundStyle(rgb(0x72F2FF))
                    .frame(width: size / 2, height: size / 2)
                    .offset(y: size * 0.05)
            }
        case .firestarterGoldWinner, .summitShowdownGoldWinner, .leafstruckGoldWinner:
            medal(size, disc: rgb(0xFFFDC1), ribbon: rgb(0xD19200))
        case .firestarterSilverWinner, .summitShowdownSilverWinner, .leafstruckSilverWinner:
            medal(size, disc: rgb(0xF6F7F9), ribbon: rgb(0x8B91A5))
        case .firestarterBronzeWinner, .summitShowdownBronzeWinner, .leafstruckBronzeWinner:
            medal(size, disc: rgb(0xFFD19D), ribbon: rgb(0xEC5616))
        }
    }

    /// A 14-unit grid: the outer disc fills it, the inner one leaves a unit, the icon two.
    private func role(_ size: CGFloat, outer: Color, inner: Color, icon: String, fill: Color) -> some View {
        ZStack {
            discs(size, outer: outer, inner: inner)
            Image(systemName: icon).resizable().scaledToFit()
                .foregroundStyle(fill)
                .frame(width: size * 9 / 14, height: size * 9 / 14)
        }
    }

    private func discs(_ size: CGFloat, outer: Color, inner: Color) -> some View {
        ZStack {
            Circle().fill(outer)
            Circle().fill(inner).frame(width: size * 12 / 14, height: size * 12 / 14)
        }
    }

    /// A disc behind the award ribbon, turned 21 degrees.
    private func medal(_ size: CGFloat, disc: Color, ribbon: Color) -> some View {
        ZStack {
            Circle().fill(disc).frame(width: size / 2, height: size / 2).offset(y: -size / 10)
            Image(systemName: "rosette").resizable().scaledToFit()
                .foregroundStyle(ribbon)
                .rotationEffect(.degrees(21))
        }
    }

    private func rgb(_ value: UInt32) -> Color {
        Color(.sRGB, red: Double(value >> 16 & 0xFF) / 255, green: Double(value >> 8 & 0xFF) / 255, blue: Double(value & 0xFF) / 255)
    }
}

extension Color {
    /// WheelWizard's primary colour (primary_400), which marks a licence online and draws its VR.
    static let wheelWizard = Color(.sRGB, red: 0x01 / 255, green: 0xCB / 255, blue: 0xA5 / 255)
}

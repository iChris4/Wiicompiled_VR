// SPDX-License-Identifier: GPL-3.0-or-later

import Charts
import SwiftUI

/// WheelWizard's VrHistoryGraph (Views/Patterns/VrHistoryGraph), as the Quest launcher's
/// VrHistoryPanel.kt shows it: a player's VR over the period chosen, with its start, end and
/// change, drawn by time or by match, from Retro WFC's history of the friend code.
struct VrHistoryView: View {
    let friendCode: String
    /// Changes when the save is read again: VrHistoryGraph reloads each time it is shown, so races since count.
    let generation: Int

    @State private var days = VrHistoryView.defaultDays
    @State private var byMatch = false
    @State private var state = LoadState.loading

    private enum LoadState {
        case loading
        case loaded(RetroWfc.History)
        /// VrHistoryGraph's empty and error states, which share one look.
        case message(String)
    }

    private static let defaultDays = 30
    /// What VrHistoryGraph asks for "Lifetime".
    private static let lifetimeDays = 999
    private static let dayOptions: [(Int, String)] = [
        (1, "Last 24 hours"), (7, "Last 7 days"), (30, "Last 30 days"), (60, "Last 60 days"), (lifetimeDays, "Lifetime"),
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 24) {
                total("Start VR", history.map { Self.grouped($0.starting) } ?? "0")
                total("End VR", history.map { Self.grouped($0.ending) } ?? "0")
                total("Net Change", history.map { $0.totalChange > 0 ? "+\(Self.grouped($0.totalChange))" : Self.grouped($0.totalChange) } ?? "0",
                      color: history.map { $0.totalChange > 0 ? Color.wheelWizard : ($0.totalChange < 0 ? .red : .primary) } ?? .primary)
            }
            HStack(spacing: 16) {
                Picker("Period", selection: $days) {
                    ForEach(Self.dayOptions, id: \.0) { Text($0.1).tag($0.0) }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .fixedSize()
                Toggle("Matches", isOn: $byMatch)
                    .fixedSize()
                if case .loading = state {
                    ProgressView()
                }
            }
            Text(range).font(.callout).foregroundStyle(.secondary)
            plot
        }
        .task(id: "\(friendCode):\(days):\(generation)") { await load() }
    }

    private var history: RetroWfc.History? {
        if case .loaded(let history) = state { return history }
        return nil
    }

    private func load() async {
        // SetNoFriendCodeState: a licence never taken online has nothing to show.
        guard friendCode.contains(where: { ("1"..."9").contains($0) }) else {
            state = .message("Friend code is required to load VR history.")
            return
        }
        state = .loading
        do {
            let loaded = try await RetroWfc.history(friendCode, days: days)
            state = .loaded(loaded)
        } catch is CancellationError {
        } catch {
            if !Task.isCancelled { state = .message(error.localizedDescription) }
        }
    }

    private var range: String {
        guard let history else { return "" }
        if days == Self.lifetimeDays { return "Lifetime history" }
        let calendar = Calendar.current
        let fromFormat = calendar.component(.year, from: history.from) != calendar.component(.year, from: history.to) ? "MMM d, yyyy" : "MMM d"
        return "Past \(days) days (\(Self.date(history.from, fromFormat)) to \(Self.date(history.to, "MMM d")))"
    }

    // MARK: The plot

    private struct Point: Identifiable {
        let id: Int
        let x: Double
        let total: Int
    }

    @ViewBuilder private var plot: some View {
        if let history, !history.entries.isEmpty {
            let entries = history.entries
            let first = entries[0].time
            let span = max(1, entries[entries.count - 1].time.timeIntervalSince(first))
            let lowest = entries.map(\.total).min() ?? 0
            let highest = entries.map(\.total).max() ?? 0
            let points = entries.enumerated().map { index, entry in
                Point(id: index,
                      x: byMatch ? (entries.count > 1 ? Double(index) / Double(entries.count - 1) : 0)
                                 : entry.time.timeIntervalSince(first) / span,
                      total: entry.total)
            }
            let labels = byMatch
                ? [1, entries.count / 2 + 1, entries.count].map { "Match \($0)" }
                : [first, first.addingTimeInterval(span / 2), entries[entries.count - 1].time].map { Self.date($0, days >= 7 ? "MMM d" : "MMM d HH:mm") }
            VStack(alignment: .leading, spacing: 4) {
                HStack(alignment: .top, spacing: 8) {
                    VStack(alignment: .trailing) {
                        Text(Self.grouped(highest))
                        Spacer()
                        Text(Self.grouped(lowest))
                    }
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                    chart(points, lowest: lowest, highest: highest)
                }
                .frame(height: 220)
                HStack {
                    Text(labels[0])
                    Spacer()
                    Text(labels[1])
                    Spacer()
                    Text(labels[2])
                }
                .font(.caption)
                .foregroundStyle(.secondary)
                .padding(.leading, 48)
            }
        } else if case .message(let message) = state {
            emptyState(message)
        } else if history != nil {
            emptyState("No VR history found for this range yet.")
        } else {
            Color.clear.frame(height: 220)
        }
    }

    /// The area under the VR line in a fading gradient, and the line, over three lines across and
    /// three down, as VrHistoryChart draws them.
    private func chart(_ points: [Point], lowest: Int, highest: Int) -> some View {
        // VrHistoryGraph spreads the lowest to the highest VR over the whole height.
        let bottom = Double(lowest)
        let top = Double(max(highest, lowest + 1))
        return Chart(points) { point in
            AreaMark(x: .value("Time", point.x), yStart: .value("Lowest", bottom), yEnd: .value("VR", Double(point.total)))
                // primary_300 fading to primary_700 at 30%, under a primary_400 line, as VrHistoryChart paints them.
                .foregroundStyle(LinearGradient(colors: [Color(.sRGB, red: 0x34 / 255, green: 0xEA / 255, blue: 0xC5 / 255),
                                                         Color(.sRGB, red: 0x03 / 255, green: 0x82 / 255, blue: 0x6E / 255)],
                                                startPoint: .top, endPoint: .bottom).opacity(0.3))
            LineMark(x: .value("Time", point.x), y: .value("VR", Double(point.total)))
                .foregroundStyle(Color.wheelWizard)
                .lineStyle(StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
        }
        .chartXScale(domain: 0.0...1.0)
        .chartYScale(domain: bottom...top)
        .chartXAxis { AxisMarks(values: [0.0, 0.5, 1.0]) { _ in AxisGridLine() } }
        .chartYAxis { AxisMarks(values: [bottom, (bottom + top) / 2, top]) { _ in AxisGridLine() } }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.black.opacity(0.25)))
    }

    private func emptyState(_ message: String) -> some View {
        VStack(spacing: 8) {
            Image(systemName: "chart.xyaxis.line").font(.largeTitle).foregroundStyle(.secondary)
            Text("No VR history").font(.headline)
            Text(message).font(.callout).foregroundStyle(.secondary).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity, minHeight: 220)
        .background(RoundedRectangle(cornerRadius: 12).fill(Color.black.opacity(0.25)))
    }

    private func total(_ title: String, _ value: String, color: Color = .primary) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Text(value).font(.title2.bold().monospacedDigit()).foregroundStyle(color)
        }
    }

    private static func grouped(_ value: Int) -> String {
        NumberFormatter.localizedString(from: NSNumber(value: value), number: .decimal)
    }

    private static func date(_ date: Date, _ template: String) -> String {
        let format = DateFormatter()
        format.locale = .current
        format.dateFormat = template
        return format.string(from: date)
    }
}

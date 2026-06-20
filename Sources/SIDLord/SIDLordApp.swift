import SwiftUI
import AppKit
import AVFoundation
import CoreText
import UniformTypeIdentifiers
import SIDCore

enum C64Theme {
    static let background = Color(nsColor: NSColor(srgbRed: 0.22, green: 0.20, blue: 0.55, alpha: 1.0))
    static let border = Color(nsColor: NSColor(srgbRed: 0.44, green: 0.40, blue: 0.77, alpha: 1.0))
    static let text = Color(nsColor: NSColor(srgbRed: 0.63, green: 0.70, blue: 1.00, alpha: 1.0))
    static let secondary = Color(nsColor: NSColor(srgbRed: 0.52, green: 0.58, blue: 0.92, alpha: 1.0))
    static let fontName = "PetMe64"
    static let uiFontSize: CGFloat = 13
}

func c64Font(_ size: CGFloat) -> Font {
    .custom(C64Theme.fontName, size: size)
}

func uiFont(_ weight: Font.Weight = .regular) -> Font {
    .system(size: C64Theme.uiFontSize, weight: weight, design: .monospaced)
}

enum SIDChipModel: Int, CaseIterable, Identifiable {
    case mos6581 = 6581
    case mos8580 = 8580

    var id: Int { rawValue }
    var title: String { self == .mos6581 ? "MOS6581" : "MOS8580" }
}

struct TableRowData: Identifiable {
    let id: Int
    var left: UInt8
    var right: UInt8
}

struct VerticalSlider: NSViewRepresentable {
    @Binding var value: Double
    let range: ClosedRange<Double>
    let step: Double

    @MainActor
    final class Coordinator: NSObject {
        var parent: VerticalSlider

        init(parent: VerticalSlider) {
            self.parent = parent
        }

        @objc @MainActor func valueChanged(_ sender: NSSlider) {
            var next = sender.doubleValue
            if parent.step > 0 {
                next = (next / parent.step).rounded() * parent.step
            }
            if next < parent.range.lowerBound { next = parent.range.lowerBound }
            if next > parent.range.upperBound { next = parent.range.upperBound }
            parent.value = next
            sender.doubleValue = next
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator(parent: self)
    }

    func makeNSView(context: Context) -> NSSlider {
        let slider = NSSlider(value: value, minValue: range.lowerBound, maxValue: range.upperBound, target: context.coordinator, action: #selector(Coordinator.valueChanged(_:)))
        slider.isVertical = true
        slider.controlSize = .small
        return slider
    }

    func updateNSView(_ nsView: NSSlider, context: Context) {
        nsView.minValue = range.lowerBound
        nsView.maxValue = range.upperBound
        if abs(nsView.doubleValue - value) > .ulpOfOne {
            nsView.doubleValue = value
        }
    }
}

final class AudioEngineController {
    private let engine = AVAudioEngine()
    private var sourceNode: AVAudioSourceNode?

    func start() throws {
        let outputFormat = engine.outputNode.inputFormat(forBus: 0)
        sidcore_set_sample_rate(Float(outputFormat.sampleRate))

        let sourceNode = AVAudioSourceNode(format: outputFormat) { _, _, frameCount, audioBufferList -> OSStatus in
            var mono = Array(repeating: Float(0), count: Int(frameCount))
            mono.withUnsafeMutableBufferPointer { monoBuffer in
                sidcore_render(monoBuffer.baseAddress, frameCount)
            }

            let buffers = UnsafeMutableAudioBufferListPointer(audioBufferList)
            for audioBuffer in buffers {
                guard let data = audioBuffer.mData else { continue }
                let channelData = data.bindMemory(to: Float.self, capacity: Int(frameCount))
                for frame in 0..<Int(frameCount) {
                    channelData[frame] = mono[frame]
                }
            }

            return noErr
        }

        engine.attach(sourceNode)
        engine.connect(sourceNode, to: engine.mainMixerNode, format: outputFormat)
        self.sourceNode = sourceNode
        try engine.start()
    }
}

@MainActor
final class InstrumentViewModel: ObservableObject {
    @Published var attackNibble: Double = 5
    @Published var decayNibble: Double = 2
    @Published var sustainNibble: Double = 12
    @Published var releaseNibble: Double = 5
    @Published var gateTimer: Double = 3
    @Published var vibDelay: Double = 0
    @Published var firstWave: UInt8 = 0x20
    @Published var selectedTableIndex: Int = 0
    @Published var tablePointerHex: String = "00"
    @Published var tableLengthHex: String = "00"
    @Published var selectedTableRowIndex: Int = 0
    @Published var selectedTableRowLeftHex: String = "00"
    @Published var selectedTableRowRightHex: String = "00"
    @Published var tableRowsByTable: [[TableRowData]] = Array(repeating: [], count: 4)
    @Published var tablePointerHexByTable: [String] = Array(repeating: "00", count: 4)
    @Published var tableLengthHexByTable: [String] = Array(repeating: "00", count: 4)
    @Published var loadedInstrumentName: String = "none"
    @Published var presetPosition: String = "0 / 0"
    @Published var selectedChipModel: SIDChipModel = .mos8580
    @Published var status: String = "Idle"
    @Published var audioStatus: String = "Audio: not started"
    @Published var midiStatus: String = "MIDI: not connected"

    private var instrument = SIDCoreInstrument()
    private let audioEngine = AudioEngineController()
    private var midiInput: MIDIInputController?
    private var suppressLiveApply = false
    private var gateTimerFlags: UInt8 = 0
    private var presetFiles: [URL] = []
    private var currentPresetIndex: Int?
    private let tableNames = ["WTBL", "PTBL", "FTBL", "STBL"]
    private let attackTimes: [Float] = [0.002, 0.008, 0.016, 0.024, 0.038, 0.056, 0.068, 0.080, 0.100, 0.250, 0.500, 0.800, 1.000, 3.000, 5.000, 8.000]
    private let decayReleaseTimes: [Float] = [0.006, 0.024, 0.048, 0.072, 0.114, 0.168, 0.204, 0.240, 0.300, 0.750, 1.500, 2.400, 3.000, 9.000, 15.000, 24.000]

    init() {
        sidcore_init_default_instrument(&instrument)
        selectedChipModel = SIDChipModel(rawValue: Int(sidcore_get_chip_model())) ?? .mos8580
        syncCoreInstrumentMeta()
        do {
            try audioEngine.start()
            audioStatus = "Audio: running"
        } catch {
            audioStatus = "Audio error: \(error.localizedDescription)"
        }

        midiInput = MIDIInputController(
            onMessage: { [weak self] status, data1, data2 in
                self?.handleMIDI(status: status, data1: data1, data2: data2)
            },
            onSourceCountChanged: { [weak self] sourceCount in
                DispatchQueue.main.async {
                    self?.midiStatus = sourceCount > 0
                        ? "MIDI: listening (\(sourceCount) source\(sourceCount == 1 ? "" : "s"))"
                        : "MIDI: no devices connected"
                }
            }
        )

        do {
            try midiInput?.start()
            let sourceCount = midiInput?.sourceCount ?? 0
            midiStatus = sourceCount > 0
                ? "MIDI: listening (\(sourceCount) source\(sourceCount == 1 ? "" : "s"))"
                : "MIDI: no devices connected"
        } catch {
            midiStatus = "MIDI error: \(error.localizedDescription)"
        }
        refreshTableEditor()
    }

    func setChipModel(_ model: SIDChipModel) {
        selectedChipModel = model
        sidcore_set_chip_model(Int32(model.rawValue))
        status = String(cString: sidcore_last_event())
    }

    func applyADSRLive() {
        if suppressLiveApply { return }
        syncCoreInstrumentMeta()
        sidcore_set_adsr(&instrument, attackValue, decayValue, sustainValue, releaseValue)
        status = String(cString: sidcore_last_event())
    }

    func noteOn() {
        syncCoreInstrumentMeta()
        sidcore_set_adsr(&instrument, attackValue, decayValue, sustainValue, releaseValue)
        sidcore_note_on(60, 100, &instrument)
        status = String(cString: sidcore_last_event())
    }

    func noteOff() {
        sidcore_note_off(60)
        status = String(cString: sidcore_last_event())
    }

    func saveInstrument() {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "ins") ?? .data]
        panel.canCreateDirectories = true
        panel.nameFieldStringValue = "instrument.ins"

        if panel.runModal() == .OK, let url = panel.url {
            syncCoreInstrumentMeta()
            sidcore_set_adsr(&instrument, attackValue, decayValue, sustainValue, releaseValue)
            let result = url.path.withCString { path in
                sidcore_save_ins(path, &instrument)
            }
            status = result != 0
                ? "Saved: \(url.lastPathComponent)"
                : String(cString: sidcore_last_event())
            if result != 0 {
                loadedInstrumentName = url.lastPathComponent
            }
        }
    }

    func loadInstrument() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "ins") ?? .data]
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false

        if panel.runModal() == .OK, let url = panel.url {
            loadInstrument(at: url, refreshPresetList: true)
        }
    }

    func loadPreviousPreset() {
        stepPreset(by: -1)
    }

    func loadNextPreset() {
        stepPreset(by: 1)
    }

    func refreshTableEditor() {
        guard selectedTableIndex >= 0 && selectedTableIndex < tableNames.count else { return }
        var nextRowsByTable: [[TableRowData]] = Array(repeating: [], count: tableNames.count)
        var nextPointerHexByTable: [String] = Array(repeating: "00", count: tableNames.count)
        var nextLengthHexByTable: [String] = Array(repeating: "00", count: tableNames.count)

        for tableIdx in 0..<tableNames.count {
            let table = Int32(tableIdx)
            let pointer = sidcore_table_pointer(table)
            let length = Int(sidcore_table_length(table))
            nextPointerHexByTable[tableIdx] = hexByte(pointer)
            nextLengthHexByTable[tableIdx] = hexByte(UInt8(max(0, min(255, length))))

            var rows: [TableRowData] = []
            if length > 0 {
                rows.reserveCapacity(length)
                for idx in 0..<length {
                    var left: UInt8 = 0
                    var right: UInt8 = 0
                    if sidcore_get_table_row(table, UInt8(idx), &left, &right) != 0 {
                        rows.append(TableRowData(id: idx, left: left, right: right))
                    } else {
                        rows.append(TableRowData(id: idx, left: 0, right: 0))
                    }
                }
            }
            nextRowsByTable[tableIdx] = rows
        }

        tableRowsByTable = nextRowsByTable
        tablePointerHexByTable = nextPointerHexByTable
        tableLengthHexByTable = nextLengthHexByTable
        tablePointerHex = tablePointerHexByTable[selectedTableIndex]
        tableLengthHex = tableLengthHexByTable[selectedTableIndex]

        if currentTableRows.isEmpty {
            selectedTableRowIndex = 0
            selectedTableRowLeftHex = "00"
            selectedTableRowRightHex = "00"
        } else {
            selectedTableRowIndex = max(0, min(selectedTableRowIndex, currentTableRows.count - 1))
            syncSelectedRowFields()
        }
    }

    func applyTableHeaderEdits() {
        let table = Int32(selectedTableIndex)
        guard let pointer = parseHexByte(tablePointerHex), let length = parseHexByte(tableLengthHex) else {
            status = "Table update failed: use hex bytes (00-FF)"
            return
        }
        let pointerOK = sidcore_set_table_pointer(table, pointer)
        let lengthOK = sidcore_set_table_length(table, length)
        refreshTableEditor()
        status = (pointerOK != 0 && lengthOK != 0)
            ? "Table \(tableName) updated: ptr=\(String(format: "%02X", pointer)) len=\(String(format: "%02X", length))"
            : "Table update failed"
    }

    func selectTableRow(_ index: Int) {
        guard !currentTableRows.isEmpty else { return }
        selectedTableRowIndex = max(0, min(index, currentTableRows.count - 1))
        syncSelectedRowFields()
    }

    func selectTableCell(table: Int, row: Int) {
        guard table >= 0 && table < tableNames.count else { return }
        selectedTableIndex = table
        tablePointerHex = tablePointerHexByTable[table]
        tableLengthHex = tableLengthHexByTable[table]
        selectTableRow(row)
    }

    func applySelectedTableRowEdit() {
        guard !currentTableRows.isEmpty else {
            status = "No rows to edit in \(tableName)"
            return
        }
        guard let left = parseHexByte(selectedTableRowLeftHex), let right = parseHexByte(selectedTableRowRightHex) else {
            status = "Row update failed: use hex bytes (00-FF)"
            return
        }
        let table = Int32(selectedTableIndex)
        let row = UInt8(selectedTableRowIndex)
        let result = sidcore_set_table_row(table, row, left, right)
        refreshTableEditor()
        status = result != 0
            ? "Row \(String(format: "%02X", row)) set: L=\(String(format: "%02X", left)) R=\(String(format: "%02X", right))"
            : "Row update failed"
    }

    func insertTableRowAfterSelection() {
        guard currentTableRows.count < 255 else {
            status = "Row insert failed: max 255 rows"
            return
        }
        let insertIndex = currentTableRows.isEmpty ? 0 : min(selectedTableRowIndex + 1, currentTableRows.count)
        var rows = currentTableRows
        rows.insert(TableRowData(id: 0, left: 0, right: 0), at: insertIndex)
        if writeTableRows(rows) {
            refreshTableEditor()
            selectTableRow(insertIndex)
            status = "Inserted row \(String(format: "%02X", insertIndex)) in \(tableName)"
        } else {
            status = "Row insert failed"
        }
    }

    func deleteSelectedTableRow() {
        guard !currentTableRows.isEmpty else {
            status = "No rows to delete in \(tableName)"
            return
        }
        var rows = currentTableRows
        let deleteIndex = selectedTableRowIndex
        rows.remove(at: deleteIndex)
        if writeTableRows(rows) {
            refreshTableEditor()
            if !currentTableRows.isEmpty {
                selectTableRow(min(deleteIndex, currentTableRows.count - 1))
            }
            status = "Deleted row \(String(format: "%02X", deleteIndex)) in \(tableName)"
        } else {
            status = "Row delete failed"
        }
    }

    func tableSelectionChanged() {
        tablePointerHex = tablePointerHexByTable[selectedTableIndex]
        tableLengthHex = tableLengthHexByTable[selectedTableIndex]
        selectedTableRowIndex = 0
        refreshTableEditor()
    }

    private func handleMIDI(status: UInt8, data1: UInt8, data2: UInt8) {
        DispatchQueue.main.async {
            let type = status & 0xF0
            if type == 0x90 && data2 > 0 {
                self.syncCoreInstrumentMeta()
                sidcore_set_adsr(&self.instrument, self.attackValue, self.decayValue, self.sustainValue, self.releaseValue)
                sidcore_note_on(data1, data2, &self.instrument)
                self.status = String(cString: sidcore_last_event())
            } else if type == 0x80 || (type == 0x90 && data2 == 0) {
                sidcore_note_off(data1)
                self.status = String(cString: sidcore_last_event())
            }
        }
    }

    private func syncCoreInstrumentMeta() {
        let gateTimerLow = UInt8(max(0, min(63, Int(gateTimer.rounded()))))
        instrument.gateTimer = gateTimerFlags | gateTimerLow
        instrument.vibDelay = UInt8(max(0, min(255, Int(vibDelay.rounded()))))
        instrument.firstWave = firstWave
    }

    private func closestIndex(for value: Float, in table: [Float]) -> Int {
        var bestIndex = 0
        var bestDiff = abs(table[0] - value)
        for (index, candidate) in table.enumerated().dropFirst() {
            let diff = abs(candidate - value)
            if diff < bestDiff {
                bestDiff = diff
                bestIndex = index
            }
        }
        return bestIndex
    }

    var attackValue: Float { attackTimes[Int(max(0, min(15, attackNibble.rounded())))] }
    var decayValue: Float { decayReleaseTimes[Int(max(0, min(15, decayNibble.rounded())))] }
    var sustainValue: Float { Float(max(0, min(15, sustainNibble.rounded()))) / 15.0 }
    var releaseValue: Float { decayReleaseTimes[Int(max(0, min(15, releaseNibble.rounded())))] }
    var canStepPreset: Bool { presetFiles.count > 1 }
    var tableName: String {
        guard selectedTableIndex >= 0 && selectedTableIndex < tableNames.count else { return "?" }
        return tableNames[selectedTableIndex]
    }
    var currentTableRows: [TableRowData] {
        guard selectedTableIndex >= 0 && selectedTableIndex < tableRowsByTable.count else { return [] }
        return tableRowsByTable[selectedTableIndex]
    }
    var selectedTableRowLabel: String {
        String(format: "%02X", selectedTableRowIndex)
    }
    func tableNameFor(index: Int) -> String {
        guard index >= 0 && index < tableNames.count else { return "?" }
        return tableNames[index]
    }
    func rowsForTable(index: Int) -> [TableRowData] {
        guard index >= 0 && index < tableRowsByTable.count else { return [] }
        return tableRowsByTable[index]
    }
    func isSelectedTableRow(table: Int, row: Int) -> Bool {
        selectedTableIndex == table && selectedTableRowIndex == row
    }
    var gateTimerDisplay: String {
        let low = Int(gateTimer.rounded()) & 0x3F
        if gateTimerFlags == 0 {
            return String(format: "%02X", low)
        }
        return String(format: "%02X (raw %02X)", low, Int(gateTimerFlags | UInt8(low)))
    }
    var vibDelaySeconds: Double {
        let ticks = max(0, Int(vibDelay.rounded()) - 1)
        return Double(ticks) / 50.0
    }

    private func loadInstrument(at url: URL, refreshPresetList: Bool) {
        var loaded = SIDCoreInstrument()
        let result = url.path.withCString { path in
            sidcore_load_ins(path, &loaded)
        }
        if result == 0 {
            status = String(cString: sidcore_last_event())
            return
        }

        if refreshPresetList {
            configurePresetList(for: url)
        } else {
            updatePresetSelection(for: url)
        }

        suppressLiveApply = true
        instrument = loaded
        attackNibble = Double(closestIndex(for: loaded.attack, in: attackTimes))
        decayNibble = Double(closestIndex(for: loaded.decay, in: decayReleaseTimes))
        sustainNibble = Double(max(0, min(15, Int((loaded.sustain * 15).rounded()))))
        releaseNibble = Double(closestIndex(for: loaded.release, in: decayReleaseTimes))
        gateTimerFlags = loaded.gateTimer & 0xC0
        gateTimer = Double(loaded.gateTimer & 0x3F)
        vibDelay = Double(loaded.vibDelay)
        firstWave = loaded.firstWave
        loadedInstrumentName = url.lastPathComponent
        refreshTableEditor()
        status = "Loaded: \(url.lastPathComponent) | \(String(cString: sidcore_last_event()))"
        DispatchQueue.main.async { [weak self] in
            self?.suppressLiveApply = false
        }
    }

    private func stepPreset(by offset: Int) {
        guard let currentPresetIndex, !presetFiles.isEmpty else {
            status = "Preset browser unavailable. Load a .ins file first."
            return
        }

        let count = presetFiles.count
        let nextIndex = (currentPresetIndex + offset + count) % count
        loadInstrument(at: presetFiles[nextIndex], refreshPresetList: false)
    }

    private func configurePresetList(for url: URL) {
        let directory = url.deletingLastPathComponent()
        do {
            let files = try FileManager.default.contentsOfDirectory(
                at: directory,
                includingPropertiesForKeys: nil,
                options: [.skipsHiddenFiles]
            )
            let presets = files
                .filter { $0.pathExtension.lowercased() == "ins" }
                .sorted { $0.lastPathComponent.localizedCaseInsensitiveCompare($1.lastPathComponent) == .orderedAscending }

            presetFiles = presets
            updatePresetSelection(for: url)
        } catch {
            presetFiles = []
            currentPresetIndex = nil
            presetPosition = "0 / 0"
            status = "Loaded: \(url.lastPathComponent) | preset list error: \(error.localizedDescription)"
        }
    }

    private func updatePresetSelection(for url: URL) {
        let targetPath = url.standardizedFileURL.path
        if let index = presetFiles.firstIndex(where: { $0.standardizedFileURL.path == targetPath }) {
            currentPresetIndex = index
        } else if !presetFiles.isEmpty {
            currentPresetIndex = 0
        } else {
            currentPresetIndex = nil
        }

        if let currentPresetIndex {
            presetPosition = "\(currentPresetIndex + 1) / \(presetFiles.count)"
        } else {
            presetPosition = "0 / 0"
        }
    }

    private func syncSelectedRowFields() {
        guard !currentTableRows.isEmpty else {
            selectedTableRowLeftHex = "00"
            selectedTableRowRightHex = "00"
            return
        }
        let row = currentTableRows[selectedTableRowIndex]
        selectedTableRowLeftHex = hexByte(row.left)
        selectedTableRowRightHex = hexByte(row.right)
    }

    private func writeTableRows(_ rows: [TableRowData]) -> Bool {
        let table = Int32(selectedTableIndex)
        if sidcore_set_table_length(table, UInt8(rows.count)) == 0 {
            return false
        }
        for (idx, row) in rows.enumerated() {
            if sidcore_set_table_row(table, UInt8(idx), row.left, row.right) == 0 {
                return false
            }
        }
        return true
    }

    private func hexByte(_ value: UInt8) -> String {
        String(format: "%02X", value)
    }

    private func parseHexByte(_ text: String) -> UInt8? {
        let trimmed = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty, trimmed.count <= 2 else { return nil }
        return UInt8(trimmed, radix: 16)
    }
}

struct ContentView: View {
    @ObservedObject var viewModel: InstrumentViewModel
    private let adsrPanelWidth: CGFloat = 414

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Text("SIDLord")
                    .font(uiFont(.medium))
                    .bold()

                HStack(spacing: 10) {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Patch")
                            .font(uiFont())
                        Text(patchDisplayName.uppercased())
                            .font(uiFont(.medium))
                            .lineLimit(2)
                            .truncationMode(.middle)
                        Text("[\(viewModel.presetPosition)]")
                            .font(uiFont())
                            .foregroundStyle(C64Theme.secondary)
                    }
                        .padding(.horizontal, 8)
                        .padding(.vertical, 5)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(
                            RoundedRectangle(cornerRadius: 6)
                                .stroke(C64Theme.border, lineWidth: 1)
                        )
                    VStack(spacing: 6) {
                        Button {
                            viewModel.loadPreviousPreset()
                        } label: {
                            Image(systemName: "chevron.left")
                                .font(uiFont(.bold))
                                .frame(width: 20, height: 14)
                        }
                        .buttonStyle(.bordered)
                        .disabled(!viewModel.canStepPreset)
                        Button {
                            viewModel.loadNextPreset()
                        } label: {
                            Image(systemName: "chevron.right")
                                .font(uiFont(.bold))
                                .frame(width: 20, height: 14)
                        }
                        .buttonStyle(.bordered)
                        .disabled(!viewModel.canStepPreset)
                    }
                }
                .frame(width: adsrPanelWidth, alignment: .leading)

            VStack(alignment: .leading, spacing: 8) {
                Text("ADSR")
                    .font(uiFont())
                HStack(spacing: 14) {
                    adsrVerticalSlider("A", value: $viewModel.attackNibble, display: String(format: "%02X (%.3fs)", Int(viewModel.attackNibble), viewModel.attackValue))
                        .onChange(of: viewModel.attackNibble) { _, _ in viewModel.applyADSRLive() }
                    adsrVerticalSlider("D", value: $viewModel.decayNibble, display: String(format: "%02X (%.3fs)", Int(viewModel.decayNibble), viewModel.decayValue))
                        .onChange(of: viewModel.decayNibble) { _, _ in viewModel.applyADSRLive() }
                    adsrVerticalSlider("S", value: $viewModel.sustainNibble, display: String(format: "%02X (%.2f)", Int(viewModel.sustainNibble), viewModel.sustainValue))
                        .onChange(of: viewModel.sustainNibble) { _, _ in viewModel.applyADSRLive() }
                    adsrVerticalSlider("R", value: $viewModel.releaseNibble, display: String(format: "%02X (%.3fs)", Int(viewModel.releaseNibble), viewModel.releaseValue))
                        .onChange(of: viewModel.releaseNibble) { _, _ in viewModel.applyADSRLive() }
                }
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(C64Theme.border, lineWidth: 1)
            )
            .frame(width: adsrPanelWidth, alignment: .leading)
            slider("Gate Timer", value: $viewModel.gateTimer, range: 0...63, step: 1, display: viewModel.gateTimerDisplay)
                .onChange(of: viewModel.gateTimer) { _, _ in viewModel.applyADSRLive() }
            slider("Vibrato Delay", value: $viewModel.vibDelay, range: 0...255, step: 1, display: String(format: "%02X (%.2fs)", Int(viewModel.vibDelay), viewModel.vibDelaySeconds))
                .onChange(of: viewModel.vibDelay) { _, _ in viewModel.applyADSRLive() }

            VStack(alignment: .leading, spacing: 8) {
                Text("Table Editor")
                    .font(uiFont())
                HStack(spacing: 12) {
                    ForEach(0..<4, id: \.self) { tableIdx in
                        VStack(alignment: .leading, spacing: 4) {
                            Text(viewModel.tableNameFor(index: tableIdx))
                                .font(uiFont())
                                .foregroundStyle(viewModel.selectedTableIndex == tableIdx ? C64Theme.text : C64Theme.secondary)
                            Text("ROW:LR")
                                .font(uiFont())
                                .foregroundStyle(C64Theme.secondary)
                            ScrollView {
                                VStack(alignment: .leading, spacing: 2) {
                                    ForEach(viewModel.rowsForTable(index: tableIdx)) { row in
                                        Button {
                                            viewModel.selectTableCell(table: tableIdx, row: row.id)
                                        } label: {
                                            Text(String(format: "%02X:%02X %02X", row.id + 1, row.left, row.right))
                                                .font(uiFont())
                                                .frame(maxWidth: .infinity, alignment: .leading)
                                                .padding(.horizontal, 4)
                                                .padding(.vertical, 1)
                                        }
                                        .buttonStyle(.plain)
                                        .background(
                                            RoundedRectangle(cornerRadius: 3)
                                                .fill(viewModel.isSelectedTableRow(table: tableIdx, row: row.id) ? C64Theme.border.opacity(0.55) : .clear)
                                        )
                                    }
                                }
                                .frame(maxWidth: .infinity, alignment: .leading)
                            }
                            .frame(height: 170)
                            .padding(4)
                            .background(
                                RoundedRectangle(cornerRadius: 4)
                                    .stroke(C64Theme.border, lineWidth: 1)
                            )
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }

                Divider()

                HStack(spacing: 8) {
                    Text("EDIT \(viewModel.tableName) ROW \(viewModel.selectedTableRowLabel)")
                        .font(uiFont())
                    Spacer()
                    Text("PTR")
                        .font(uiFont())
                    TextField("", text: $viewModel.tablePointerHex)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 42)
                    Text("LEN")
                        .font(uiFont())
                    TextField("", text: $viewModel.tableLengthHex)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 42)
                    Button("SET TBL") { viewModel.applyTableHeaderEdits() }
                }

                HStack(spacing: 8) {
                    Text("L")
                        .font(uiFont())
                    TextField("", text: $viewModel.selectedTableRowLeftHex)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 42)
                    Text("R")
                        .font(uiFont())
                    TextField("", text: $viewModel.selectedTableRowRightHex)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 42)
                    Button("SET ROW") { viewModel.applySelectedTableRowEdit() }
                    Button("+ ROW") { viewModel.insertTableRowAfterSelection() }
                    Button("- ROW") { viewModel.deleteSelectedTableRow() }
                }
            }
            .padding(10)
            .background(
                RoundedRectangle(cornerRadius: 6)
                    .stroke(C64Theme.border, lineWidth: 1)
            )

            Picker("First Wave", selection: $viewModel.firstWave) {
                Text("Triangle (10)").tag(UInt8(0x10))
                Text("Saw (20)").tag(UInt8(0x20))
                Text("Pulse (40)").tag(UInt8(0x40))
                Text("Noise (80)").tag(UInt8(0x80))
            }
            .pickerStyle(.segmented)
            .onChange(of: viewModel.firstWave) { _, _ in viewModel.applyADSRLive() }

            HStack(spacing: 12) {
                Button("Note On (C-4)") { viewModel.noteOn() }
                Button("Note Off") { viewModel.noteOff() }
            }

            Text(viewModel.audioStatus)
                .font(uiFont())

            Text(viewModel.midiStatus)
                .font(uiFont())

            VStack(alignment: .leading, spacing: 6) {
                Text("Status")
                    .font(uiFont())
                ScrollView {
                    Text(viewModel.status)
                        .font(uiFont())
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
                .frame(height: 130)
                .padding(8)
                .background(
                    RoundedRectangle(cornerRadius: 6)
                        .stroke(C64Theme.border, lineWidth: 1)
                )
            }
            .padding(.top, 6)
            }
            .padding(12)
            .frame(maxWidth: .infinity, alignment: .topLeading)
        }
        .frame(minWidth: 840, minHeight: 600)
        .font(uiFont())
        .foregroundStyle(C64Theme.text)
        .tint(C64Theme.text)
        .background(C64Theme.background.ignoresSafeArea())
    }

    private func slider(_ title: String, value: Binding<Double>, range: ClosedRange<Double>, step: Double = 0.01, display: String? = nil) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                    .font(uiFont())
                Spacer()
                Text(display ?? String(format: "%.2f", value.wrappedValue))
                    .font(uiFont())
            }
            Slider(value: value, in: range, step: step)
        }
    }

    private var patchDisplayName: String {
        let name = viewModel.loadedInstrumentName
        guard name.lowercased().hasSuffix(".ins") else { return name }
        return String(name.dropLast(4))
    }

    private func adsrVerticalSlider(_ title: String, value: Binding<Double>, display: String) -> some View {
        VStack(spacing: 6) {
            Text(title)
                .font(uiFont())
            VerticalSlider(value: value, range: 0...15, step: 1)
                .frame(width: 22, height: 86)
            Text(display)
                .font(uiFont())
                .lineLimit(1)
                .minimumScaleFactor(0.65)
                .frame(width: 88)
        }
    }
}

struct SettingsView: View {
    @ObservedObject var viewModel: InstrumentViewModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("SID chip", selection: $viewModel.selectedChipModel) {
                ForEach(SIDChipModel.allCases) { model in
                    Text(model.title).tag(model)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: viewModel.selectedChipModel) { _, newValue in
                viewModel.setChipModel(newValue)
            }
            Spacer()
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(C64Theme.background)
        .foregroundStyle(C64Theme.text)
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?
    private var settingsWindow: NSWindow?
    private let viewModel = InstrumentViewModel()

    func applicationDidFinishLaunching(_ notification: Notification) {
        registerC64Font()
        installMainMenu()
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 920, height: 640),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.center()
        window.title = "SIDLord"
        window.contentView = NSHostingView(rootView: ContentView(viewModel: viewModel))
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
    }

    private func registerC64Font() {
        guard let fontURL = Bundle.module.url(forResource: "PetMe64", withExtension: "ttf") else { return }
        CTFontManagerRegisterFontsForURL(fontURL as CFURL, .process, nil)
    }

    private func installMainMenu() {
        let mainMenu = NSMenu()
        let appName = "SIDLord"

        let appMenuItem = NSMenuItem(title: appName, action: nil, keyEquivalent: "")
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About \(appName)", action: #selector(showAboutPanel), keyEquivalent: "")
        appMenu.addItem(NSMenuItem.separator())
        let settingsItem = NSMenuItem(title: "Settings…", action: #selector(openSettings), keyEquivalent: ",")
        settingsItem.keyEquivalentModifierMask = [.command]
        appMenu.addItem(settingsItem)
        appMenu.addItem(NSMenuItem.separator())
        let quitItem = NSMenuItem(title: "Quit \(appName)", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        quitItem.keyEquivalentModifierMask = [.command]
        appMenu.addItem(quitItem)
        appMenuItem.submenu = appMenu
        mainMenu.addItem(appMenuItem)

        let fileMenuItem = NSMenuItem(title: "File", action: nil, keyEquivalent: "")
        let fileMenu = NSMenu(title: "File")
        let loadItem = NSMenuItem(title: "Load…", action: #selector(loadInstrumentMenu), keyEquivalent: "o")
        loadItem.keyEquivalentModifierMask = [.command]
        fileMenu.addItem(loadItem)
        let saveItem = NSMenuItem(title: "Save…", action: #selector(saveInstrumentMenu), keyEquivalent: "s")
        saveItem.keyEquivalentModifierMask = [.command]
        fileMenu.addItem(saveItem)
        fileMenuItem.submenu = fileMenu
        mainMenu.addItem(fileMenuItem)

        NSApp.mainMenu = mainMenu
    }

    @objc private func showAboutPanel() {
        NSApp.orderFrontStandardAboutPanel(options: [
            .applicationName: "SIDLord",
            .version: "Iteration 1: instrument core API + SwiftUI shell"
        ])
    }

    @objc private func openSettings() {
        if let settingsWindow {
            settingsWindow.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            return
        }

        let settingsWindow = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 460, height: 220),
            styleMask: [.titled, .closable, .miniaturizable],
            backing: .buffered,
            defer: false
        )
        settingsWindow.center()
        settingsWindow.title = "Settings"
        settingsWindow.contentView = NSHostingView(
            rootView: SettingsView(viewModel: viewModel)
        )
        settingsWindow.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.settingsWindow = settingsWindow
    }

    @objc private func loadInstrumentMenu() {
        viewModel.loadInstrument()
    }

    @objc private func saveInstrumentMenu() {
        viewModel.saveInstrument()
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        true
    }
}

@main
enum SIDLordMain {
    static func main() {
        let app = NSApplication.shared
        let delegate = AppDelegate()
        app.setActivationPolicy(.regular)
        app.delegate = delegate
        app.run()
    }
}

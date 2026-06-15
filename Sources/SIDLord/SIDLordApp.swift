import SwiftUI
import AppKit
import AVFoundation
import UniformTypeIdentifiers
import SIDCore

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
    @Published var status: String = "Idle"
    @Published var audioStatus: String = "Audio: not started"
    @Published var midiStatus: String = "MIDI: not connected"

    private var instrument = SIDCoreInstrument()
    private let audioEngine = AudioEngineController()
    private var midiInput: MIDIInputController?
    private let attackTimes: [Float] = [0.002, 0.008, 0.016, 0.024, 0.038, 0.056, 0.068, 0.080, 0.100, 0.250, 0.500, 0.800, 1.000, 3.000, 5.000, 8.000]
    private let decayReleaseTimes: [Float] = [0.006, 0.024, 0.048, 0.072, 0.114, 0.168, 0.204, 0.240, 0.300, 0.750, 1.500, 2.400, 3.000, 9.000, 15.000, 24.000]

    init() {
        sidcore_init_default_instrument(&instrument)
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
    }

    func applyADSRLive() {
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
        }
    }

    func loadInstrument() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "ins") ?? .data]
        panel.canChooseDirectories = false
        panel.allowsMultipleSelection = false

        if panel.runModal() == .OK, let url = panel.url {
            var loaded = SIDCoreInstrument()
            let result = url.path.withCString { path in
                sidcore_load_ins(path, &loaded)
            }
            if result != 0 {
                instrument = loaded
                attackNibble = Double(closestIndex(for: loaded.attack, in: attackTimes))
                decayNibble = Double(closestIndex(for: loaded.decay, in: decayReleaseTimes))
                sustainNibble = Double(max(0, min(15, Int((loaded.sustain * 15).rounded()))))
                releaseNibble = Double(closestIndex(for: loaded.release, in: decayReleaseTimes))
                gateTimer = Double(loaded.gateTimer)
                vibDelay = Double(loaded.vibDelay)
                firstWave = loaded.firstWave
                status = "Loaded: \(url.lastPathComponent)"
            } else {
                status = String(cString: sidcore_last_event())
            }
        }
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
        instrument.gateTimer = UInt8(max(0, min(63, Int(gateTimer.rounded()))))
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
}

struct ContentView: View {
    @StateObject private var viewModel = InstrumentViewModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text("SIDLord")
                .font(.largeTitle)
                .bold()

            Text("Iteration 1: instrument core API + SwiftUI shell")
                .foregroundStyle(.secondary)

            slider("Attack", value: $viewModel.attackNibble, range: 0...15, step: 1, display: String(format: "%02X (%.3fs)", Int(viewModel.attackNibble), viewModel.attackValue))
                .onChange(of: viewModel.attackNibble) { _, _ in viewModel.applyADSRLive() }
            slider("Decay", value: $viewModel.decayNibble, range: 0...15, step: 1, display: String(format: "%02X (%.3fs)", Int(viewModel.decayNibble), viewModel.decayValue))
                .onChange(of: viewModel.decayNibble) { _, _ in viewModel.applyADSRLive() }
            slider("Sustain", value: $viewModel.sustainNibble, range: 0...15, step: 1, display: String(format: "%02X (%.2f)", Int(viewModel.sustainNibble), viewModel.sustainValue))
                .onChange(of: viewModel.sustainNibble) { _, _ in viewModel.applyADSRLive() }
            slider("Release", value: $viewModel.releaseNibble, range: 0...15, step: 1, display: String(format: "%02X (%.3fs)", Int(viewModel.releaseNibble), viewModel.releaseValue))
                .onChange(of: viewModel.releaseNibble) { _, _ in viewModel.applyADSRLive() }
            slider("Gate Timer", value: $viewModel.gateTimer, range: 0...63, step: 1)
                .onChange(of: viewModel.gateTimer) { _, _ in viewModel.applyADSRLive() }
            slider("Vibrato Delay", value: $viewModel.vibDelay, range: 0...255, step: 1)
                .onChange(of: viewModel.vibDelay) { _, _ in viewModel.applyADSRLive() }

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
                Button("Load .ins") { viewModel.loadInstrument() }
                Button("Save .ins") { viewModel.saveInstrument() }
            }

            Text(viewModel.audioStatus)
                .font(.system(.body, design: .monospaced))

            Text(viewModel.midiStatus)
                .font(.system(.body, design: .monospaced))

            Text("Status: \(viewModel.status)")
                .font(.system(.body, design: .monospaced))
                .padding(.top, 6)
        }
        .padding(24)
        .frame(minWidth: 620, minHeight: 420)
    }

    private func slider(_ title: String, value: Binding<Double>, range: ClosedRange<Double>, step: Double = 0.01, display: String? = nil) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(display ?? String(format: "%.2f", value.wrappedValue))
                    .font(.system(.body, design: .monospaced))
            }
            Slider(value: value, in: range, step: step)
        }
    }
}

@MainActor
final class AppDelegate: NSObject, NSApplicationDelegate {
    private var window: NSWindow?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 720, height: 480),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.center()
        window.title = "SIDLord"
        window.contentView = NSHostingView(rootView: ContentView())
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        self.window = window
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

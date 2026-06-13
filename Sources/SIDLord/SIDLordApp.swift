import SwiftUI
import AppKit
import AVFoundation
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
    @Published var attack: Double = 0.01
    @Published var decay: Double = 0.20
    @Published var sustain: Double = 0.80
    @Published var release: Double = 0.30
    @Published var status: String = "Idle"
    @Published var audioStatus: String = "Audio: not started"
    @Published var midiStatus: String = "MIDI: not connected"

    private var instrument = SIDCoreInstrument()
    private let audioEngine = AudioEngineController()
    private var midiInput: MIDIInputController?

    init() {
        sidcore_init_default_instrument(&instrument)
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
        sidcore_set_adsr(&instrument, Float(attack), Float(decay), Float(sustain), Float(release))
    }

    func noteOn() {
        sidcore_set_adsr(&instrument, Float(attack), Float(decay), Float(sustain), Float(release))
        sidcore_note_on(60, 100, &instrument)
        status = String(cString: sidcore_last_event())
    }

    func noteOff() {
        sidcore_note_off(60)
        status = String(cString: sidcore_last_event())
    }

    private func handleMIDI(status: UInt8, data1: UInt8, data2: UInt8) {
        DispatchQueue.main.async {
            let type = status & 0xF0
            if type == 0x90 && data2 > 0 {
                sidcore_set_adsr(&self.instrument, Float(self.attack), Float(self.decay), Float(self.sustain), Float(self.release))
                sidcore_note_on(data1, data2, &self.instrument)
                self.status = String(cString: sidcore_last_event())
            } else if type == 0x80 || (type == 0x90 && data2 == 0) {
                sidcore_note_off(data1)
                self.status = String(cString: sidcore_last_event())
            }
        }
    }
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

            slider("Attack", value: $viewModel.attack, range: 0...2)
                .onChange(of: viewModel.attack) { _, _ in viewModel.applyADSRLive() }
            slider("Decay", value: $viewModel.decay, range: 0...2)
                .onChange(of: viewModel.decay) { _, _ in viewModel.applyADSRLive() }
            slider("Sustain", value: $viewModel.sustain, range: 0...1)
                .onChange(of: viewModel.sustain) { _, _ in viewModel.applyADSRLive() }
            slider("Release", value: $viewModel.release, range: 0...3)
                .onChange(of: viewModel.release) { _, _ in viewModel.applyADSRLive() }

            HStack(spacing: 12) {
                Button("Note On (C-4)") { viewModel.noteOn() }
                Button("Note Off") { viewModel.noteOff() }
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

    private func slider(_ title: String, value: Binding<Double>, range: ClosedRange<Double>) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title)
                Spacer()
                Text(String(format: "%.2f", value.wrappedValue))
                    .font(.system(.body, design: .monospaced))
            }
            Slider(value: value, in: range)
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

import Foundation
import Testing
import SIDCore

private let sidcoreTestLock = NSLock()

private func withSIDCoreLock<T>(_ body: () throws -> T) rethrows -> T {
    sidcoreTestLock.lock()
    defer { sidcoreTestLock.unlock() }
    return try body()
}

private struct ParsedInstrument {
    var ident: String
    var ad: UInt8
    var sr: UInt8
    var pointers: [UInt8]
    var vibDelay: UInt8
    var gateTimer: UInt8
    var firstWave: UInt8
    var lengths: [UInt8]
    var leftTables: [[UInt8]]
    var rightTables: [[UInt8]]
}

private struct FixtureSpec {
    var ident: String
    var ad: UInt8
    var sr: UInt8
    var pointers: [UInt8]
    var vibDelay: UInt8
    var gateTimer: UInt8
    var firstWave: UInt8
    var name: String
    var leftTables: [[UInt8]]
    var rightTables: [[UInt8]]
}

@Test
func loadSaveRoundtripAcrossGTI3GTI4GTI5() throws {
    try withSIDCoreLock {
        let specs: [FixtureSpec] = [
        FixtureSpec(
            ident: "GTI3",
            ad: 0x52,
            sr: 0xC5,
            pointers: [0x01, 0x01, 0x00, 0x00],
            vibDelay: 0x07,
            gateTimer: 0x03,
            firstWave: 0x21,
            name: "GTI3-Roundtrip",
            leftTables: [[0x41, 0xFF], [0x88], [], []],
            rightTables: [[0x80, 0x01], [0x00], [], []]
        ),
        FixtureSpec(
            ident: "GTI4",
            ad: 0x48,
            sr: 0x97,
            pointers: [0x02, 0x00, 0x01, 0x00],
            vibDelay: 0x00,
            gateTimer: 0x1A,
            firstWave: 0x09,
            name: "GTI4-Roundtrip",
            leftTables: [[0x0F, 0x81], [], [0x90], []],
            rightTables: [[0x00, 0x34], [], [0x0F], []]
        ),
        FixtureSpec(
            ident: "GTI5",
            ad: 0x00,
            sr: 0xE8,
            pointers: [0x13, 0x0B, 0x00, 0x00],
            vibDelay: 0x00,
            gateTimer: 0x02,
            firstWave: 0x09,
            name: "Snare-like",
            leftTables: [[0x81, 0x41, 0x41, 0x80, 0x80, 0x80, 0xFF], [0x88, 0xFF], [], []],
            rightTables: [[0xD4, 0xAA, 0xA6, 0xD8, 0xD0, 0xC8, 0x00], [0x00, 0x00], [], []]
        )
    ]

        for spec in specs {
        let tempDir = try uniqueTempDirectory()
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let inputURL = tempDir.appendingPathComponent("input.ins")
        let outputURL = tempDir.appendingPathComponent("output.ins")
        try Data(buildInstrumentBytes(spec: spec)).write(to: inputURL)

        var instrument = SIDCoreInstrument()
        let loadOK = inputURL.path.withCString { sidcore_load_ins($0, &instrument) }
        #expect(loadOK == 1, "\(spec.ident) load failed")

        let saveOK = outputURL.path.withCString { sidcore_save_ins($0, &instrument) }
        #expect(saveOK == 1, "\(spec.ident) save failed")

        let parsed = try parseInstrument(at: outputURL)
        #expect(parsed.ident == "GTI5")
        #expect(parsed.ad == spec.ad)
        #expect(parsed.sr == spec.sr)
        #expect(parsed.pointers == spec.pointers)
        #expect(parsed.vibDelay == spec.vibDelay)
        #expect(parsed.gateTimer == spec.gateTimer)
        #expect(parsed.firstWave == spec.firstWave)
        #expect(parsed.lengths == spec.leftTables.map { UInt8($0.count) })
        #expect(parsed.leftTables == spec.leftTables)
        #expect(parsed.rightTables == spec.rightTables)
        }
    }
}

@Test
func bulkCorpusLoadSaveRoundtrip() throws {
    try withSIDCoreLock {
    let corpusURL = try instrumentCorpusDirectory()
    let files = try FileManager.default.contentsOfDirectory(at: corpusURL, includingPropertiesForKeys: nil)
        .filter { $0.pathExtension.lowercased() == "ins" }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }

    #expect(!files.isEmpty, "No .ins files found in \(corpusURL.path)")
    #expect(files.count >= 100, "Expected a large corpus, got \(files.count)")

    let tempDir = try uniqueTempDirectory()
    defer { try? FileManager.default.removeItem(at: tempDir) }

    for file in files {
        let input = try parseInstrument(at: file)
        var instrument = SIDCoreInstrument()
        let loadOK = file.path.withCString { sidcore_load_ins($0, &instrument) }
        #expect(loadOK == 1, "Load failed for \(file.lastPathComponent)")

        let outputURL = tempDir.appendingPathComponent(file.lastPathComponent)
        let saveOK = outputURL.path.withCString { sidcore_save_ins($0, &instrument) }
        #expect(saveOK == 1, "Save failed for \(file.lastPathComponent)")

        let output = try parseInstrument(at: outputURL)
        #expect(output.ident == "GTI5", "Saved ident mismatch for \(file.lastPathComponent)")
        #expect(output.ad == input.ad, "AD mismatch for \(file.lastPathComponent)")
        #expect(output.sr == input.sr, "SR mismatch for \(file.lastPathComponent)")
        #expect(output.pointers == input.pointers, "Pointers mismatch for \(file.lastPathComponent)")
        #expect(output.vibDelay == input.vibDelay, "VibDelay mismatch for \(file.lastPathComponent)")
        #expect(output.gateTimer == input.gateTimer, "GateTimer mismatch for \(file.lastPathComponent)")
        #expect(output.firstWave == input.firstWave, "FirstWave mismatch for \(file.lastPathComponent)")
        #expect(output.lengths == input.lengths, "Table lengths mismatch for \(file.lastPathComponent)")
        #expect(output.leftTables == input.leftTables, "Left table mismatch for \(file.lastPathComponent)")
        #expect(output.rightTables == input.rightTables, "Right table mismatch for \(file.lastPathComponent)")
    }
    }
}

@Test
func gateTimerHighBitsRoundtripRegression() throws {
    try withSIDCoreLock {
    let corpus = try instrumentCorpusDirectory()
    let cases: [(name: String, expectedGateTimer: UInt8)] = [
        ("unleash-19-Tolkki-noHR.ins", 0x82),
        ("funktest-13-Sawtooth-legato.ins", 0x42)
    ]

    let tempDir = try uniqueTempDirectory()
    defer { try? FileManager.default.removeItem(at: tempDir) }

    for testCase in cases {
        let inputURL = corpus.appendingPathComponent(testCase.name)
        let input = try parseInstrument(at: inputURL)
        #expect(input.gateTimer == testCase.expectedGateTimer, "Unexpected fixture gateTimer in \(testCase.name)")

        var instrument = SIDCoreInstrument()
        let loadOK = inputURL.path.withCString { sidcore_load_ins($0, &instrument) }
        #expect(loadOK == 1, "Load failed for \(testCase.name)")

        let outputURL = tempDir.appendingPathComponent(testCase.name)
        let saveOK = outputURL.path.withCString { sidcore_save_ins($0, &instrument) }
        #expect(saveOK == 1, "Save failed for \(testCase.name)")

        let output = try parseInstrument(at: outputURL)
        #expect(output.gateTimer == testCase.expectedGateTimer, "GateTimer high bits lost for \(testCase.name)")
    }
    }
}

@Test
func corpusWavetableCommandCoverage() throws {
    let corpus = try instrumentCorpusDirectory()
    let files = try FileManager.default.contentsOfDirectory(at: corpus, includingPropertiesForKeys: nil)
        .filter { $0.pathExtension.lowercased() == "ins" }
        .sorted { $0.lastPathComponent < $1.lastPathComponent }

    let supportedCommands: Set<UInt8> = [0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x09, 0x0A, 0x0B, 0x0C, 0x0D]
    var unsupported: [String] = []

    for file in files {
        let parsed = try parseInstrument(at: file)
        let wavetable = parsed.leftTables[0]
        for byte in wavetable where byte >= 0xF0 && byte <= 0xFE {
            let command = byte & 0x0F
            if !supportedCommands.contains(command) {
                unsupported.append("\(file.lastPathComponent): \(String(format: "%02X", byte))")
            }
        }
    }

    #expect(unsupported.isEmpty, "Unsupported wavetable commands found: \(unsupported.joined(separator: ", "))")
}

@Test
func loadDiagnosticsWarnOnInvalidSpeedtableRefs() throws {
    try withSIDCoreLock {
    let corpus = try instrumentCorpusDirectory()
    let url = corpus.appendingPathComponent("wavecmdtest-01-instrument.ins")

    var instrument = SIDCoreInstrument()
    let loadOK = url.path.withCString { sidcore_load_ins($0, &instrument) }
    #expect(loadOK == 1)

    let event = String(cString: sidcore_last_event())
    #expect(event.contains("WARN_ST=04"), "Expected WARN_ST=04, got: \(event)")
    }
}

@Test
func loadDiagnosticsShowNoWarningsForValidSpeedRefs() throws {
    try withSIDCoreLock {
    let corpus = try instrumentCorpusDirectory()
    let url = corpus.appendingPathComponent("sixpack-13-bass---saw.ins")

    var instrument = SIDCoreInstrument()
    let loadOK = url.path.withCString { sidcore_load_ins($0, &instrument) }
    #expect(loadOK == 1)

    let event = String(cString: sidcore_last_event())
    #expect(event.contains("WARN_ST=00"), "Expected WARN_ST=00, got: \(event)")
    #expect(event.contains("WARN_CMD=00"), "Expected WARN_CMD=00, got: \(event)")
    #expect(event.contains("WARN_JW=00"), "Expected WARN_JW=00, got: \(event)")
    #expect(event.contains("WARN_JP=00"), "Expected WARN_JP=00, got: \(event)")
    #expect(event.contains("WARN_JF=00"), "Expected WARN_JF=00, got: \(event)")
    }
}

@Test
func loadDiagnosticsWarnOnInvalidWaveCommands() throws {
    try withSIDCoreLock {
        let tempDir = try uniqueTempDirectory()
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let invalidWaveSpec = FixtureSpec(
            ident: "GTI5",
            ad: 0x52,
            sr: 0xC5,
            pointers: [0x01, 0x00, 0x00, 0x00],
            vibDelay: 0x00,
            gateTimer: 0x03,
            firstWave: 0x20,
            name: "InvalidWaveCmd",
            leftTables: [[0xF8], [], [], []],
            rightTables: [[0x01], [], [], []]
        )

        let inputURL = tempDir.appendingPathComponent("invalid-wavecmd.ins")
        try Data(buildInstrumentBytes(spec: invalidWaveSpec)).write(to: inputURL)

        var instrument = SIDCoreInstrument()
        let loadOK = inputURL.path.withCString { sidcore_load_ins($0, &instrument) }
        #expect(loadOK == 1)

        let event = String(cString: sidcore_last_event())
        #expect(event.contains("WARN_ST=00"), "Expected WARN_ST=00, got: \(event)")
        #expect(event.contains("WARN_CMD=01"), "Expected WARN_CMD=01, got: \(event)")
    }
}

@Test
func loadDiagnosticsWarnOnInvalidTableJumps() throws {
    try withSIDCoreLock {
        let tempDir = try uniqueTempDirectory()
        defer { try? FileManager.default.removeItem(at: tempDir) }

        let invalidJumpSpec = FixtureSpec(
            ident: "GTI5",
            ad: 0x52,
            sr: 0xC5,
            pointers: [0x01, 0x01, 0x01, 0x00],
            vibDelay: 0x00,
            gateTimer: 0x03,
            firstWave: 0x20,
            name: "InvalidJumpCmd",
            leftTables: [[0xFF, 0xFF], [0xFF, 0xFF], [0xFF, 0xFF], []],
            rightTables: [[0x02, 0x01], [0x02, 0x01], [0x02, 0x01], []]
        )

        let inputURL = tempDir.appendingPathComponent("invalid-jumps.ins")
        try Data(buildInstrumentBytes(spec: invalidJumpSpec)).write(to: inputURL)

        var instrument = SIDCoreInstrument()
        let loadOK = inputURL.path.withCString { sidcore_load_ins($0, &instrument) }
        #expect(loadOK == 1)

        let event = String(cString: sidcore_last_event())
        #expect(event.contains("WARN_JW=02"), "Expected WARN_JW=02, got: \(event)")
        #expect(event.contains("WARN_JP=02"), "Expected WARN_JP=02, got: \(event)")
        #expect(event.contains("WARN_JF=02"), "Expected WARN_JF=02, got: \(event)")
    }
}

@Test
func tableEditingAPIRoundtrip() throws {
    try withSIDCoreLock {
        var instrument = SIDCoreInstrument()
        sidcore_init_default_instrument(&instrument)

        #expect(sidcore_table_count() == 4)
        #expect(sidcore_set_table_length(0, 2) == 1)
        #expect(sidcore_set_table_pointer(0, 1) == 1)
        #expect(sidcore_set_table_row(0, 0, 0x21, 0x00) == 1)
        #expect(sidcore_set_table_row(0, 1, 0xFF, 0x01) == 1)

        var left: UInt8 = 0
        var right: UInt8 = 0
        #expect(sidcore_get_table_row(0, 0, &left, &right) == 1)
        #expect(left == 0x21 && right == 0x00)
        #expect(sidcore_get_table_row(0, 1, &left, &right) == 1)
        #expect(left == 0xFF && right == 0x01)

        let tempDir = try uniqueTempDirectory()
        defer { try? FileManager.default.removeItem(at: tempDir) }
        let outputURL = tempDir.appendingPathComponent("table-api-roundtrip.ins")
        let saveOK = outputURL.path.withCString { sidcore_save_ins($0, &instrument) }
        #expect(saveOK == 1)

        let parsed = try parseInstrument(at: outputURL)
        #expect(parsed.pointers[0] == 0x01)
        #expect(parsed.lengths[0] == 0x02)
        #expect(parsed.leftTables[0] == [0x21, 0xFF])
        #expect(parsed.rightTables[0] == [0x00, 0x01])
    }
}

@Test
func sidChipModelSelectionAPI() {
    withSIDCoreLock {
        sidcore_set_chip_model(8580)
        #expect(sidcore_get_chip_model() == 8580)

        sidcore_set_chip_model(6581)
        #expect(sidcore_get_chip_model() == 6581)

        sidcore_set_chip_model(1234)
        #expect(sidcore_get_chip_model() == 6581)
    }
}

@Test
func sidChipModelDefaultsTo8580() {
    withSIDCoreLock {
        sidcore_set_chip_model(6581)
        #expect(sidcore_get_chip_model() == 6581)

        var instrument = SIDCoreInstrument()
        sidcore_init_default_instrument(&instrument)
        #expect(sidcore_get_chip_model() == 8580)
    }
}

private func uniqueTempDirectory() throws -> URL {
    let base = FileManager.default.temporaryDirectory
    let dir = base.appendingPathComponent("sidlord-tests-\(UUID().uuidString)", isDirectory: true)
    try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
}

private func instrumentCorpusDirectory() throws -> URL {
    struct CorpusError: LocalizedError {
        let message: String
        var errorDescription: String? { message }
    }

    let thisFile = URL(fileURLWithPath: #filePath)
    let repoRoot = thisFile
        .deletingLastPathComponent() // SIDCoreCompatibilityTests
        .deletingLastPathComponent() // Tests
        .deletingLastPathComponent() // repo root

    let candidates = [
        repoRoot.appendingPathComponent("../gt-instruments"),
        repoRoot.deletingLastPathComponent().appendingPathComponent("gt-instruments")
    ].map { $0.standardizedFileURL }

    for candidate in candidates {
        var isDirectory: ObjCBool = false
        if FileManager.default.fileExists(atPath: candidate.path, isDirectory: &isDirectory), isDirectory.boolValue {
            return candidate
        }
    }

    throw CorpusError(message: "Could not find instrument corpus directory (expected ../gt-instruments from repo root).")
}

private func buildInstrumentBytes(spec: FixtureSpec) -> [UInt8] {
    precondition(spec.ident.count == 4)
    precondition(spec.pointers.count == 4)
    precondition(spec.leftTables.count == 4 && spec.rightTables.count == 4)

    var bytes = Array(spec.ident.utf8)
    bytes.append(spec.ad)
    bytes.append(spec.sr)
    bytes.append(contentsOf: spec.pointers)
    bytes.append(spec.vibDelay)
    bytes.append(spec.gateTimer)
    bytes.append(spec.firstWave)

    var name = Array(spec.name.utf8.prefix(16))
    if name.count < 16 {
        name.append(contentsOf: repeatElement(0, count: 16 - name.count))
    }
    bytes.append(contentsOf: name)

    for index in 0..<4 {
        let left = spec.leftTables[index]
        let right = spec.rightTables[index]
        precondition(left.count == right.count)
        precondition(left.count <= 255)
        bytes.append(UInt8(left.count))
        bytes.append(contentsOf: left)
        bytes.append(contentsOf: right)
    }

    return bytes
}

private func parseInstrument(at url: URL) throws -> ParsedInstrument {
    let data = try Data(contentsOf: url)
    var idx = 0

    struct ParseError: Error {}

    func readByte(_ data: Data, _ idx: inout Int) throws -> UInt8 {
        guard idx < data.count else { throw ParseError() }
        defer { idx += 1 }
        return data[idx]
    }

    func readBytes(_ data: Data, _ idx: inout Int, count: Int) throws -> [UInt8] {
        guard idx + count <= data.count else { throw ParseError() }
        defer { idx += count }
        return Array(data[idx..<(idx + count)])
    }

    let ident = String(bytes: try readBytes(data, &idx, count: 4), encoding: .ascii) ?? "????"
    let ad = try readByte(data, &idx)
    let sr = try readByte(data, &idx)
    let pointers = try readBytes(data, &idx, count: 4)
    let vibDelay = try readByte(data, &idx)
    let gateTimer = try readByte(data, &idx)
    let firstWave = try readByte(data, &idx)
    _ = try readBytes(data, &idx, count: 16)

    var lengths = [UInt8]()
    var leftTables = [[UInt8]]()
    var rightTables = [[UInt8]]()
    for _ in 0..<4 {
        let len = try readByte(data, &idx)
        lengths.append(len)
        leftTables.append(try readBytes(data, &idx, count: Int(len)))
        rightTables.append(try readBytes(data, &idx, count: Int(len)))
    }

    return ParsedInstrument(
        ident: ident,
        ad: ad,
        sr: sr,
        pointers: pointers,
        vibDelay: vibDelay,
        gateTimer: gateTimer,
        firstWave: firstWave,
        lengths: lengths,
        leftTables: leftTables,
        rightTables: rightTables
    )
}

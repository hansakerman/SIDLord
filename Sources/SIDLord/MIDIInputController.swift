import Foundation
import CoreMIDI

final class MIDIInputController {
    private var client = MIDIClientRef()
    private var inputPort = MIDIPortRef()
    private var connectedSources = Set<MIDIEndpointRef>()
    private let onMessage: (UInt8, UInt8, UInt8) -> Void
    private let onSourceCountChanged: (Int) -> Void

    init(onMessage: @escaping (UInt8, UInt8, UInt8) -> Void,
         onSourceCountChanged: @escaping (Int) -> Void) {
        self.onMessage = onMessage
        self.onSourceCountChanged = onSourceCountChanged
    }

    deinit {
        stop()
    }

    func start() throws {
        var status = MIDIClientCreateWithBlock("SIDLord MIDI Client" as CFString, &client) { [weak self] _ in
            self?.refreshConnections()
        }
        guard status == noErr else { throw MIDIError.clientCreate(status) }

        status = MIDIInputPortCreateWithBlock(client, "SIDLord MIDI Input" as CFString, &inputPort) { [weak self] packetList, _ in
            self?.process(packetList: packetList)
        }
        guard status == noErr else { throw MIDIError.inputPortCreate(status) }

        refreshConnections()
    }

    func stop() {
        for source in connectedSources {
            MIDIPortDisconnectSource(inputPort, source)
        }
        connectedSources.removeAll()
        onSourceCountChanged(0)

        if inputPort != 0 {
            MIDIPortDispose(inputPort)
            inputPort = 0
        }

        if client != 0 {
            MIDIClientDispose(client)
            client = 0
        }
    }

    func refreshConnections() {
        var currentSources = Set<MIDIEndpointRef>()
        let sourceCount = MIDIGetNumberOfSources()
        if sourceCount > 0 {
            for index in 0..<sourceCount {
                let source = MIDIGetSource(index)
                if source == 0 { continue }
                currentSources.insert(source)
                if !connectedSources.contains(source) {
                    if MIDIPortConnectSource(inputPort, source, nil) == noErr {
                        connectedSources.insert(source)
                    }
                }
            }
        }

        for source in connectedSources where !currentSources.contains(source) {
            MIDIPortDisconnectSource(inputPort, source)
        }
        connectedSources = connectedSources.intersection(currentSources)
        onSourceCountChanged(connectedSources.count)
    }

    var sourceCount: Int {
        connectedSources.count
    }

    private func parseStatusByteCount(_ status: UInt8) -> Int {
        let type = status & 0xF0
        switch type {
        case 0x80, 0x90, 0xA0, 0xB0, 0xE0:
            return 3
        case 0xC0, 0xD0:
            return 2
        default:
            switch status {
            case 0xF1, 0xF3:
                return 2
            case 0xF2:
                return 3
            default:
                return 1
            }
        }
    }

    private func process(packetList: UnsafePointer<MIDIPacketList>) {
        var packet = packetList.pointee.packet

        for _ in 0..<packetList.pointee.numPackets {
            handle(packet: packet)
            packet = withUnsafePointer(to: &packet) { MIDIPacketNext($0).pointee }
        }
    }

    private func handle(packet: MIDIPacket) {
        let length = Int(packet.length)
        if length == 0 { return }

        withUnsafeBytes(of: packet.data) { rawBuffer in
            let bytes = rawBuffer.bindMemory(to: UInt8.self)
            var index = 0
            var runningStatus: UInt8 = 0

            while index < length {
                let raw = bytes[index]
                let status: UInt8
                let messageStart: Int

                if raw >= 0x80 {
                    status = raw
                    runningStatus = status
                    messageStart = index
                    index += 1
                } else if runningStatus >= 0x80 {
                    status = runningStatus
                    messageStart = index - 1
                } else {
                    index += 1
                    continue
                }

                let byteCount = parseStatusByteCount(status)
                let dataBytesNeeded = max(0, byteCount - 1)
                if index + dataBytesNeeded > length {
                    break
                }

                if (status & 0xF0 == 0x80 || status & 0xF0 == 0x90), dataBytesNeeded >= 2 {
                    onMessage(status, bytes[index], bytes[index + 1])
                }

                if raw >= 0x80 {
                    index += dataBytesNeeded
                } else {
                    index = messageStart + byteCount
                }
            }
        }
    }
}

enum MIDIError: Error {
    case clientCreate(OSStatus)
    case inputPortCreate(OSStatus)
}

# SIDLord

Standalone macOS-first instrument editor focused on GoatTracker-compatible workflow.

> **Status:** Very early **beta / lab** project. Expect rough edges, partial implementations, and breaking changes while core behavior is still being explored.

Project handover / decisions log: `WORKING_NOTES.md`

## Current iteration

This bootstrap iteration sets up:

- A native SwiftUI desktop shell (`Sources/SIDLord`)
- A separate C core module (`Sources/SIDCore`) with a stable API surface
- Live parameter editing (ADSR) and note trigger hooks (`noteOn`/`noteOff`)
- A low-latency audio output slice using a reSID backend for audible note playback
- CoreMIDI input listener for live note-on/note-off control (including hot-plug device refresh)
- GoatTracker instrument file load/save (`.ins`, GTI3/4/5 header support) with ADSR + first-wave/gate/vibrato metadata and table/pointer pass-through preservation

The core/UI split is intentional so future GUI/client surfaces can reuse the same engine.

## Build

```bash
swift build
swift run SIDLord
```

## Next steps

1. Improve reSID voice programming to better match GoatTracker instrument behavior.
2. Move MIDI routing from UI shell into a core-facing command layer.
3. Expand `.ins` compatibility from pass-through table/pointer preservation to editable table-aware workflows.

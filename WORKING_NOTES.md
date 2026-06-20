# SIDLord Working Notes

Last updated: 2026-06-15 (late evening)

This file is the persistent handover document for project goals, technical decisions, and current status.
Update in this file.

## Project goal

Build a standalone, macOS-first instrument editor for GoatTracker-compatible `.ins` workflows, with audible low-latency playback and a reusable core engine.

## Key decisions

1. **Architecture split:** SwiftUI app shell in `Sources/SIDLord`, reusable audio/instrument core in `Sources/SIDCore`.
2. **Audio backend:** Use reSID in `SIDCore` for playback/emulation.
3. **File support focus:** Prioritize GoatTracker `.ins` (GTI3/4/5) load/save compatibility first.
4. **Behavior fix implemented:** On `.ins` load/playback, `firstWave` is sanitized to waveform bits before writing SID voice control (prevents silent playback when non-wave bits are present).
5. **Delivery strategy:** Bootstrap functional playback first, then iterate toward tracker-accurate table/filter behavior.

## Current status

- App starts and plays audible notes via reSID.
- ADSR editing updates live.
- MIDI note on/off input works with hot-plug device refresh.
- UI controls are now quantized for deterministic editing:
  - ADSR uses SID nibble steps (0-15)
  - Gate timer and vibrato delay use integer steps
- `.ins` load/save works for ADSR + metadata (`firstWave`, `gateTimer`, `vibDelay`) and table/pointer pass-through preservation.
- `.ins` load now reports explicit diagnostics in status output (AD/SR, FW/GT/VD, table pointers, table lengths) for quick mapping inspection.
- Playback now avoids forcing saw when loaded `firstWave` has no waveform bits: note-on derives initial wave from wavetable first relevant step (fallback saw only if no usable wave is found).
- Added playback-time table pointer rebasing for `.ins` files where instrument pointers are stored as absolute table indices (e.g. WTBL pointer > local WTBL length), so wavetable/pulsetable playback starts from the intended local data.
- Increased default/minimum app window size and made status text explicitly multi-line, so long load diagnostics are visible in the UI.
- Fixed load-status overwrite in UI: programmatic control updates during `.ins` load no longer trigger live ADSR status spam, so load diagnostics remain visible after loading.
- Improved MIDI startup error reporting: CoreMIDI failures now surface explicit OSStatus details in UI instead of generic “operation could not be completed”, and startup now cleans up partial MIDI init state before retry.
- Added focused `.ins` compatibility roundtrip tests (GTI3/GTI4/GTI5) validating load->save preserves AD/SR, metadata fields, pointers, and table payloads (including absolute-pointer fixture).
- Wavetable stepping now handles absolute note bytes (`>= 0x80`) in GT style (`note & 0x7F`) instead of ignoring them, improving pitch behavior for instruments that use absolute wavetable notes.
- Added bulk corpus self-test for `.ins` compatibility: automated load->save roundtrip validation across the full local `../gt-instruments` set (229 files), checking AD/SR, metadata, pointers, and all table bytes.
- Implemented wavetable command vibrato (`CMD 0x04`) with speed-table (`STBL`) support and instrument `vibDelay` gating for note startup behavior closer to GoatTracker tick semantics.
- Added instrument-level speedtable vibrato tick effect (STBL pointer + vibDelay) for the common GoatTracker "default vibrato" path, and aligned ordering so normal tick effects are skipped on wavetable command ticks.
- UI usability tweak: show current preset/`.ins` filename in main view and display vibrato delay as both hex value and approximate seconds (`XX (Ys)`).
- Added wavetable command support for portamento up/down/toneporta (`CMD 0x01/0x02/0x03`) using speedtable (`STBL`) semantics, reducing pitch-motion gaps in command-heavy instruments.
- Added wavetable `CMD_SETWAVEPTR` (`0x08`) support so instruments that redirect wavetable flow from commands can continue stepping as expected.
- Added UI preset browser: preset name now appears in a boxed selector with up/down arrows that cycle and load `.ins` files from the current preset directory (alternative to file dialog load).
- Gate timer handling improved: UI now preserves full gateTimer byte on load/edit/save (high-bit flags no longer dropped) and playback applies gate-off pretrigger when gateTimer high-bit mode requests it.
- Added targeted regression test for gateTimer high-bit persistence (`0x82` and `0x42`) to prevent future compatibility regressions.
- Added gateTimer no-hardrestart handling on note-on: when gate flags request hardrestart, SIDCore now applies keyoff pretrigger and temporary hardrestart AD/SR before instrument ADSR.
- Applied C64-themed UI styling (BASIC-like blue palette) and bundled `PetMe64` font as an app resource, with runtime font registration for consistent C64 visual style.
- Increased default/minimum window size and moved status output into a fixed-height scrollable panel to prevent layout/window “jumping” when status text grows.
- Added corpus-level wavetable command coverage test to fail fast if new/unsupported `F*` command bytes appear in instrument data.
- Corrected wavetable `F8` handling to match GoatTracker command semantics in wavetable context (treated as invalid/non-flow command rather than pointer redirect).
- Tightened invalid wavetable command handling: `F0/F8/FE` command nibbles now terminate wavetable execution in SIDCore to avoid undefined behavior from non-wavetable-valid commands.
- Added STBL-reference robustness pass: speedtable-dependent wavetable effects now verify STBL entry validity before applying, and load diagnostics now include `WARN_ST=xx` for invalid WTBL->STBL references.
- Added diagnostics regression tests (`WARN_ST=04` for known edge-case file and `WARN_ST=00` for a valid baseline) and serialized SIDCore-interacting tests with a shared lock to prevent parallel-state flakiness.
- Extended load diagnostics with `WARN_CMD=xx` for invalid wavetable command nibbles and added regression coverage for this warning (`WARN_CMD=01` crafted fixture, `WARN_CMD=00` valid fixture).
- Aligned wavetable `CMD_SETWAVE` behavior with tracker semantics by writing full control-byte value (except external gate bit handling), rather than waveform-only masking.
- Added jump-target safety pass for WTBL/PTBL/FTBL: jump-into-jump and invalid jump targets now terminate table stepping deterministically at runtime, and load diagnostics report per-table jump warnings (`WARN_JW/WARN_JP/WARN_JF`).
- Added regression coverage for invalid table jumps using crafted fixtures; compatibility suite expanded accordingly.
- Added SIDCore table-editing API infrastructure for future preset editor UI: table count, pointer get/set, length get/set, and row get/set for WTBL/PTBL/FTBL/STBL.
- Added regression test coverage for table-editing API persistence via save/load roundtrip.
- Added native macOS menu integration: app menu with About/Settings/Quit and File menu with Load/Save (standard keyboard shortcuts), and removed in-view load/save buttons.
- Moved the in-view iteration/status tagline into the About panel metadata to keep the main editor UI cleaner.
- ADSR UI changed to compact vertical synth-style sliders with numeric readouts positioned below each slider.
- UI readability/layout tweak: reduced UI font sizes, made patch label more explicit, and start window now opens to full screen-visible frame.
- Patch chooser visibility pass: patch name now rendered as uppercase monospaced system text in the box; preset navigation uses clear `chevron.left` / `chevron.right` bordered icon buttons.
- UI compacting pass: unified all text to the same monospaced system font/size as the patch filename, tightened spacing, reduced ADSR control footprint, and switched to smaller default/minimum app window sizes.
- Settings menu now includes SID chip model selection (MOS6581/MOS8580), wired to new SIDCore chip-model API so changing it updates live emulation behavior.
- SID chip default changed to MOS8580 (core and UI fallback); `sidcore_init_default_instrument` now resets model to 8580 to guarantee startup default.
- Table editor UI replaced with a more GoatTracker-like workflow: four side-by-side table columns (WTBL/PTBL/FTBL/STBL) with hex row listing (`NN:LL RR`), selectable row focus, and direct hex editing for pointer/length + row values, plus row insert/delete.
- SIDLord can be launched and run locally from this repo (`swift run SIDLord`).
- Full GoatTracker playback semantics are **not complete** yet:
  - Table execution behavior is partial/in-progress.
  - Filter workflow control from instrument/table data is not fully tracker-accurate.
  - Not all GoatTracker command/table edge cases are implemented.

## Discussion log (2026-06-15)

1. Confirmed project direction: SIDLord is an instrument-focused editor with GoatTracker-compatible workflow, not full tracker playback yet.
2. Investigated silent playback after loading some `.ins` files: ADSR moved, but audio could be silent due to invalid/unusable waveform bits in loaded `firstWave`.
3. Implemented fix in `SIDCore`: playback now sanitizes `firstWave` to waveform bits before writing voice control (with safe fallback waveform).
4. Confirmed overall playback status: bootstrap playback works, but full emulation/command behavior (tables + filters + edge cases) is still incomplete.
5. Agreed implementation direction: use local reference code in `../goattracker-vibed-out` as behavior source when aligning table/filter playback semantics.
6. Verified live use quality: low latency felt good and MIDI hot-plug worked in practice.
7. Confirmed `.ins` roundtrip behavior by saving, changing ADSR, then loading back and observing restoration.
8. Added first-wave (incl. saw), gate timer, and vibrato delay handling in UI/core and `.ins` mapping.
9. Added quantized UI controls so ADSR and timing-related values are no longer free-running floats.
10. Product direction note: long-term idea is a more modern sequencer UX, but phase focus remains `.ins` compatibility first.
11. Added explicit `.ins` load diagnostics output from `SIDCore` and surfaced it in the app status text for fast field-level inspection.
12. Fixed first-wave startup behavior for instruments (e.g., snare/noise cases): if `firstWave` lacks waveform bits, playback now probes wavetable for initial wave instead of defaulting directly to saw.
13. Investigated `unleash-05-Snare-only.ins` still sounding saw: root cause was absolute table pointers (`WTBL=0x13`, `PTBL=0x0B`) with short local table blobs; implemented table-pointer base rebasing for playback and extended diagnostics with `BASE[...]`.
14. UX follow-up: enlarged the main window defaults and status layout to better display long diagnostics strings without feeling cramped.
15. Fixed noisy status race during load by suppressing live ADSR apply while populating loaded values; this keeps the `Loaded: ... | diagnostics` message intact.
16. MIDI reliability/diagnostics pass: added better error strings for client/port creation failures and ensured failed port creation disposes the client to avoid stale startup state.
17. Added `SIDCoreCompatibilityTests` using Swift Testing to exercise `.ins` compatibility roundtrips for GTI3/4/5 fixtures, including a snare-like absolute-pointer case.
18. Table semantics increment: fixed wavetable note execution to support both relative (`<0x80`) and absolute (`>=0x80`) note values, and update the active note/frequency accordingly.
19. Added large-scale programmatic self-test (`bulkCorpusLoadSaveRoundtrip`) so regressions in `.ins` parsing/mapping are caught across the entire 229-file local instrument corpus.
20. Tracker-semantics progress: added core wavetable vibrato execution path (including note-step mode via STBL high-bit compare and per-note vibrato state reset).
21. Vibrato follow-up fix: `vibDelay` now gates the STBL-driven tick effect (CMD_DONOTHING-style path) rather than wavetable CMD vibrato itself, matching GoatTracker behavior more closely.
22. Added clearer UI context for testing: persistent preset label plus vibrato delay time hint in seconds.
23. Tracker command coverage expanded with wavetable portamento commands; toneporta now converges toward played base note with STBL-derived speed.
24. Table-flow coverage update: wavetable pointer redirection command (`0x08`) is now executed in `SIDCore`.
25. UX feature added: in-app preset stepping (`↑`/`↓`) with folder-based `.ins` list and position indicator.
26. Compatibility fix: stopped truncating gateTimer to 0..63 internally; low bits remain user-editable while loaded high bits are preserved and reflected in raw gate display.
27. Test coverage update: explicit high-bit gateTimer roundtrip test added alongside corpus and fixture compatibility tests.
28. Gate semantics increment: implemented hardrestart branch for gateTimer flags (`0x40`/`0x80`) in live note trigger path.
29. Visual direction update: switched UI typography/colors toward Commodore 64 aesthetic with bundled bitmap-style font and themed controls.
30. Added proactive compatibility guardrail test (`corpusWavetableCommandCoverage`) for wavetable command-byte support drift.
31. Corrected prior assumption around wavetable `F8`: command is now neutralized in wavetable execution path to avoid non-GT behavior.
32. Extended invalid-command guard to cover all known non-wavetable-valid command nibbles (`0x00`, `0x08`, `0x0E`) with deterministic stop behavior.
33. Long-session phase complete: STBL validation + warning diagnostics + regression coverage integrated and passing in full compatibility suite.
34. Additional sprint pass: invalid wavetable-command warnings and set-wave semantics correction implemented with full compatibility suite passing (7 tests).
35. Additional sprint pass: table-jump safety + warning diagnostics integrated; compatibility suite now passing with 8 tests.
36. Preset-editing infrastructure sprint: exposed low-level table mutation/read APIs in C bridge and validated with dedicated roundtrip test; compatibility suite now passing with 9 tests.
37. Desktop UX pass: migrated file actions into standard OS menu bar, with keyboard shortcuts and placeholder settings window.
38. UI cleanup: moved “Iteration 1…” text from main view into About info.
39. UI tweak: converted ADSR controls to vertical mini-faders with values under each control.
40. UI pass: smaller typography + clearer patch header + full-size launch frame.
41. Patch selector refinement: ensured patch label/control visibility with monospaced uppercase filename and explicit left/right icon navigation buttons.
42. UI compacting pass: switched all UI text to the patch-name monospaced font at one shared size, tightened layout spacing, reduced slider footprint, and stopped auto-maximizing the main window.
43. Settings pass: replaced placeholder settings content with a real SID chip selector (6581/8580) backed by SIDCore set/get chip-model API, plus targeted regression coverage.
44. Chip-model default pass: switched startup/default SID model to 8580 and added targeted test coverage for default + selection API behavior.
45. GT-style table-editor pass: replaced slider-based table editing with side-by-side hex table views and direct per-row/per-table value editing operations.
30. Window/layout stability pass: enlarged app frame and constrained dynamic status area to a reserved viewport.

## Next stage (active roadmap)

1. ✅ Add explicit load diagnostics/inspection output for `.ins` fields to validate mapping correctness quickly.
2. ✅ Build a focused `.ins` compatibility test set and verify save->load and load->save roundtrips.
3. Implement/finish tracker-style table stepping in `SIDCore` (wavetable, pulse table, filter table).
4. Ensure per-tick behavior aligns with GoatTracker timing semantics and filter register behavior.
5. Document unsupported commands/edge cases explicitly as they are found.
6. Add instrument-editor tempo workflow review: evaluate how editor-visible tempo/tick settings should influence table effects (e.g. arpeggio rate), and align behavior with GoatTracker expectations.
   - Current corpus command bytes observed in WTBL: `F1`, `F2`, `F6` (all implemented in SIDCore).
   - Edge case observed: `wavecmdtest-01-instrument.ins` uses STBL param `02` with STBL length `01` (out-of-range in standalone `.ins` context).

## Session recovery rule

When context is lost, restore from this file first, then continue from the first unchecked item in **Next stage**.

## Operator shorthand

- `c` = continue
- `sl` = SIDLord
- When launching `sl`, always stop any already-running SIDLord process first, then start a fresh instance.

## Sources

- Local GoatTracker reference source: `../goattracker-vibed-out`
- Local extracted instrument set: `../gt-instruments` (229 `.ins` files; all files currently have valid `GTI3/GTI4/GTI5` headers)

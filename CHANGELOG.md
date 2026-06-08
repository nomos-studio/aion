# Changelog

All notable changes to aion are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [Unreleased]

---

## [0.1.0] — 2026-06-07

### Added

#### Core session substrate

- **Ableton Link peer** — joins the Link beat-sync session at startup;
  `link.beat_at_time(link.now())` drives all 24 PPQN timing.
- **MIDI I/O** — RtMidi output (note-on/off, CC, SysEx, pitch-bend,
  channel-pressure, MIDI clock 0xF8) and hardware MIDI input routed to
  `hw_midi_in_queue`.
- **OSC server** — UDP OSC listener (`--osc-port`, default 9002); note
  events routed to `osc_in_queue`.
- **Audio device** — RtAudio backend opened at a small buffer size
  (`--buffer-frames`, default 64) purely for hardware-interrupt clock
  discipline; no audio processing (output zeroed).
- **Event scheduler** — `nomos::rt::event_scheduler` for beat-accurate
  multi-event dispatch (`MSG-SCHEDULE-BUNDLE`).
- **RT modulator engine** — `nomos::rt::modulator_engine`; autonomous
  modulators (slope, slew, segment, shift-register, fractal, stochastic,
  graph) initiated via IPC and run at event-loop tick rate without further
  network round-trips.

#### Routing matrix

- **`RoutingMatrix`** — runtime-configurable routing layer replacing the
  fixed pass-through.  Templated on a `Sink` type for testability (no
  virtual dispatch in production).
  - **`MidiRoute`** — maps events from a source (`:ipc`, `:midi-hw`, `:osc`)
    to MIDI output with optional channel override and semitone transpose.
    Source-channel filtering (`src-ch -1` = any).
  - **`ModRoute`** — maps a named modulator output field (`:cv`, `:aux`,
    `:gate`, `:gate2`, `:out0`–`:out15`) to a MIDI CC with scale and
    offset.  CC values dirty-tracked (no redundant sends).
  - `apply_edn(string_view)` — replaces the full routing table from an EDN
    payload; clears dirty-tracking cache on update.
- **`aion_control_thread`** — thin `rt_control_thread` subclass that
  overrides `dispatch_extension` to handle `MSG-ROUTE-SET` (0x52).

#### IPC (via nomos-rt)

- Unix-domain socket (`--socket`, default `/tmp/aion.sock`); accepts
  one connection at a time.
- Handled: session lifecycle, TX-LOG, param-set, note events, MIDI CC/PB/
  SysEx/MTS, modulator start/stop/update, schedule-bundle, Link transport.
- `MSG-TICK` (0x50) pushed to the connected client at 24 PPQN with
  `{:beat D :tick-n N :mods {:id {:cv F :aux F :gate B :gate2 B} ...}}`.
- `MSG-ROUTE-SET` (0x52) — replaces the routing matrix; dispatched by
  `aion_control_thread::dispatch_extension`.
- `MSG-MIDI-EVENT` (0x51) — pushed on hardware MIDI input (planned).

#### Test suite

- Catch2 v3.7.1; 19 tests in `tests/test_routing_matrix.cpp`.
- `CaptureSink` stub — records all MIDI output calls without hardware;
  enables full routing-logic coverage.
- Covers: `apply_edn` parsing, `route_event` (source filter, channel
  override, transpose, clamp, note-off), `route_modulator` (CV→CC,
  dirty tracking, gate field, scale/offset, cache clear).

### Changed

- Migrated from kairos FetchContent to `nomos::rt` types throughout.
- nomos-rt dependency pinned to `0d7ef76` for reproducible builds.

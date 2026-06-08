# aion

Lightweight nomos-rt session substrate for embedded and remote deployment.
Provides Link beat-sync, MIDI I/O, OSC, RT modulators, and a configurable
routing matrix — without the CLAP plugin layer that kairos requires.

Designed to run on a Raspberry Pi Zero 2W or similar single-board computer
as an always-on session peer that [nous](https://github.com/nomos-studio/nous)
can connect to over a Unix socket.

```
nous (Clojure REPL, any host)
    ↕  EDN over Unix socket (nomos-rt IPC)
aion (this — Pi Zero 2W or laptop)
    ↕
MIDI hardware · OSC peers · Ableton Link session
```

## Features

- **Ableton Link peer** — joins the Link session as a beat-sync authority;
  small buffer sizes + a real audio device give hardware-clock discipline.
- **MIDI I/O** — note/CC/SysEx output and hardware MIDI input via RtMidi.
- **OSC server** — UDP OSC note events routed into the event queue.
- **RT modulator engine** — slope, slew, segment, shift-register, fractal,
  stochastic, and graph modulators; output pushed as `:mods` in MSG-TICK.
- **Routing matrix** — runtime-configurable MIDI event routing (source →
  channel remap, transpose) and modulator → MIDI CC routing.
- **24 PPQN tick push** — MSG-TICK frames sent to the connected nous client
  on every beat tick; includes modulator outputs and current beat position.

## Build

```bash
cmake -B build
cmake --build build

# With a local nomos-rt checkout (avoids network fetch)
cmake -B build -DNOMOS_RT_DIR=/path/to/nomos-rt \
               -DEDN_CPP_DIR=/path/to/edn-cpp
cmake --build build
```

**Requirements:** cmake ≥ 3.20, C++20, network access for FetchContent
(nomos-rt, RtMidi, RtAudio, Ableton Link, edn-cpp, txlog).

## Usage

```bash
./build/aion \
  --socket /tmp/aion.sock \    # IPC socket (default: /tmp/aion.sock)
  --bpm 120 \                  # Initial Link tempo
  --midi-port 1 \              # RtMidi output port index (-1 = none)
  --midi-in-port 0 \           # RtMidi input port index (-1 = none)
  --osc-port 9002 \            # UDP OSC listen port (default: 9002)
  --audio-device 0 \           # RtAudio device id (0 = default)
  --buffer-frames 64 \         # Buffer size for clock discipline
  --no-audio                   # Skip audio (software timer only)
```

List available audio devices:

```bash
./build/aion --list-audio-devices
```

From the nous REPL — identical API to kairos:

```clojure
(require '[nous.kairos :as kairos])
(kairos/start-kairos! :binary "/path/to/aion"
                      :socket-path "/tmp/aion.sock")

;; Configure routing matrix
(kairos/send-route-set!
  {:midi-routes [{:src :ipc     :src-ch -1 :dst-ch 0}
                 {:src :midi-hw :src-ch -1 :dst-ch 0 :xpose 12}]
   :mod-routes  [{:id "lfo-1" :field :cv :ch 0 :cc 74}]})

;; Start an RT modulator
(kairos/start-modulator! :lfo-1 :slope {:rate 0.25 :shape -0.3})
```

## Routing matrix

The routing matrix replaces the fixed pass-through dispatch with configurable
rules.  Updated at runtime via MSG-ROUTE-SET (0x52).

**MIDI routes** (`{:src :ipc|:midi-hw|:osc :src-ch N :dst-ch N :xpose N}`):
- `:src` — event source: `:ipc` (from nous), `:midi-hw` (hardware MIDI), `:osc`
- `:src-ch` — filter by source channel (0–15); -1 = any
- `:dst-ch` — output channel override (0–15); -1 = passthrough
- `:xpose` — semitone transpose (-127–127)

**Mod routes** (`{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0 :offset 0.0}`):
- `:field` — modulator output: `:cv`, `:aux`, `:gate`, `:gate2`, `:out0`–`:out15`
- `:scale` and `:offset` — map CV [0,1] to CC [0,127] with linear scaling

An empty `{}` resets to default pass-through (all sources → MIDI out).

## License

GPL-2.0-or-later — see [LICENSE](LICENSE).  Binaries link Ableton Link
(GPL-2.0-or-later); the resulting binary is GPL-2.0-or-later.

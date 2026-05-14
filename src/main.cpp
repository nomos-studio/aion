// SPDX-License-Identifier: GPL-2.0-or-later

#include <nomos/rt/event_scheduler.hpp>
#include <nomos/rt/input_event.hpp>
#include <nomos/rt/rt_control_thread.hpp>
#include <nomos/rt/spsc_queue.hpp>

#include "modulator_engine.hpp"

#include "audio_device.hpp"
#include "link_peer.hpp"
#include "midi_io.hpp"
#include "osc_server.hpp"

#include <clap/events.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace aion {
std::string_view version() noexcept;
}

namespace {
std::atomic<bool> g_running{true};
void on_signal(int) { g_running.store(false, std::memory_order_relaxed); }
} // namespace

int main(int argc, char *argv[]) {
  std::string socket_path = "/tmp/aion.sock";
  std::string db_path = "aion.db";
  int midi_port = -1;
  int midi_in_port = -1;
  uint16_t osc_port = 9002;
  double initial_bpm = 120.0;
  double sample_rate = 48000.0;
  uint32_t buffer_frames =
      64; // small: hardware interrupt cadence, not processing
  bool no_audio = false;
  bool list_audio_devs = false;
  unsigned int audio_device_id = 0;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg{argv[i]};
    if (arg == "--socket" && i + 1 < argc)
      socket_path = argv[++i];
    else if (arg == "--db" && i + 1 < argc)
      db_path = argv[++i];
    else if (arg == "--midi-port" && i + 1 < argc)
      midi_port = std::atoi(argv[++i]);
    else if (arg == "--midi-in-port" && i + 1 < argc)
      midi_in_port = std::atoi(argv[++i]);
    else if (arg == "--osc-port" && i + 1 < argc)
      osc_port = static_cast<uint16_t>(std::atoi(argv[++i]));
    else if (arg == "--bpm" && i + 1 < argc)
      initial_bpm = std::atof(argv[++i]);
    else if (arg == "--buffer-frames" && i + 1 < argc)
      buffer_frames = static_cast<uint32_t>(std::atoi(argv[++i]));
    else if (arg == "--audio-device" && i + 1 < argc)
      audio_device_id = static_cast<unsigned int>(std::atoi(argv[++i]));
    else if (arg == "--no-audio")
      no_audio = true;
    else if (arg == "--list-audio-devices")
      list_audio_devs = true;
    else if (arg == "--version") {
      std::cout << "aion v" << aion::version() << "\n";
      return EXIT_SUCCESS;
    } else if (arg == "--help") {
      std::cout
          << "Usage: aion [options]\n"
             "  --socket <path>         Unix domain socket (default: "
             "/tmp/aion.sock)\n"
             "  --db <path>             txlog database (default: aion.db)\n"
             "  --bpm <bpm>             Initial Link tempo (default: 120)\n"
             "  --midi-port <n>         MIDI output port index\n"
             "  --midi-in-port <n>      MIDI input port index\n"
             "  --osc-port <n>          UDP OSC listen port (default: 9002)\n"
             "  --buffer-frames <n>     Audio buffer size for clock discipline "
             "(default: 64)\n"
             "  --audio-device <id>     RtAudio device id (0=default, see "
             "--list-audio-devices)\n"
             "  --no-audio              Skip audio device (no hardware clock "
             "discipline)\n"
             "  --list-audio-devices    Print available audio devices and "
             "exit\n"
             "  --version               Print version and exit\n";
      return EXIT_SUCCESS;
    }
  }

  if (list_audio_devs) {
    nomos::rt::audio_device::list_devices();
    return EXIT_SUCCESS;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  // Shared queues.
  // param_queue: written by rt_control_thread on msg_param_set; no audio
  // consumer
  //              in aion — drained and discarded by the event thread.
  // ipc_in_queue: note-on / note-off / midi-in from IPC; dispatched to MIDI hw.
  // hw_midi_in_queue: hardware MIDI input translated to clap events by midi_io.
  // osc_in_queue: OSC-driven note events from the osc_server.
  nomos::rt::param_queue param_queue;
  nomos::rt::input_event_queue ipc_in_queue;
  nomos::rt::input_event_queue hw_midi_in_queue;
  nomos::rt::input_event_queue osc_in_queue;

  // Beat scheduler — control thread pushes to staging; event thread ticks.
  nomos::rt::event_scheduler scheduler;

  // RT modulator engine — autonomous modulators initiated via IPC, run at
  // event-loop tick rate.
  nomos::rt::modulator_engine mod_engine;

  // Control thread — IPC socket, session, and common cljseq-rt message
  // dispatch.
  nomos::rt::rt_control_thread ctrl{nomos::rt::rt_control_thread::config{
                                     .socket_path = socket_path,
                                     .db_path = db_path,
                                     .sched_staging = &scheduler.staging(),
                                     .mod_engine = &mod_engine,
                                 },
                                 param_queue, ipc_in_queue};
  ctrl.start();

  // Link peer — beat-sync authority.
  nomos::rt::link_peer link{initial_bpm};
  link.enable(true);

  // MIDI I/O.
  nomos::rt::midi_io midi;
  nomos::rt::midi_io::list_ports();
  if (midi_port >= 0)
    midi.open_port(static_cast<unsigned int>(midi_port));
  if (midi_in_port >= 0)
    midi.open_input_port(static_cast<unsigned int>(midi_in_port),
                         hw_midi_in_queue);

  // OSC server.
  nomos::rt::osc_server osc{osc_port, osc_in_queue};
  osc.start();

  // Audio device — opened at a small buffer size purely for the hardware
  // interrupt cadence that disciplines our clock.  When a wordclock-capable
  // interface is present (e.g. ES-9, Scarlett) this becomes the most stable
  // timestamp source in the room and aion can assert Link authority.  The
  // callback does no audio processing — it just clocks the hardware interrupt.
  nomos::rt::audio_device audio_dev;
  if (!no_audio) {
    nomos::rt::audio_device_config audio_cfg{
        .device_id = audio_device_id,
        .out_channels = 2,
        .in_channels = 0,
        .sample_rate = sample_rate,
        .buffer_frames = buffer_frames,
    };
    const bool opened = audio_dev.open(
        audio_cfg, [](float **out, const float *const *, uint32_t out_ch,
                      uint32_t, uint32_t nframes, double) {
          // Zero output — no DSP processing, just hardware clock discipline.
          for (uint32_t c = 0; c < out_ch; ++c)
            if (out && out[c])
              std::fill_n(out[c], nframes, 0.0f);
        });
    if (opened) {
      if (!audio_dev.start())
        std::cerr << "[aion] audio device failed to start; no hardware clock "
                     "discipline\n";
    } else {
      std::cerr << "[aion] audio device failed to open; no hardware clock "
                   "discipline\n";
    }
  }

  // Event dispatch thread — translates queued CLAP input events to MIDI output.
  //
  // Drains three queues:
  //   ipc_in_queue     — note/MIDI events from the cljseq IPC connection
  //   hw_midi_in_queue — hardware MIDI input (pass-through / merge)
  //   osc_in_queue     — OSC-driven note events
  //
  // param_queue is also drained here; aion has no audio consumer for params
  // (a future routing layer will act on them).
  //
  // Sleep 250µs when all queues are idle — low latency without busy-spinning.
  std::thread event_thread{[&]() {
    auto dispatch_event = [&](const nomos::rt::clap_event_union &ev) {
      switch (ev.header.type) {
      case CLAP_EVENT_NOTE_ON: {
        const auto vel = static_cast<uint8_t>(
            std::clamp(ev.note.velocity * 127.0, 0.0, 127.0));
        midi.note_on(static_cast<uint8_t>(ev.note.channel + 1),
                     static_cast<uint8_t>(ev.note.key), vel);
        break;
      }
      case CLAP_EVENT_NOTE_OFF:
        midi.note_off(static_cast<uint8_t>(ev.note.channel + 1),
                      static_cast<uint8_t>(ev.note.key));
        break;
      case CLAP_EVENT_MIDI:
        midi.send({ev.midi.data[0], ev.midi.data[1], ev.midi.data[2]});
        break;
      default:
        break;
      }
    };

    int64_t last_tick_n = -1;

    while (g_running.load(std::memory_order_relaxed)) {
      bool did_work = false;

      // Tick beat-scheduled events into ipc_in_queue before draining it.
      const double  beat   = link.beat_at_time(link.now());
      const int64_t tick_n = static_cast<int64_t>(std::floor(beat * 24.0));
      if (tick_n > last_tick_n) {
        last_tick_n             = tick_n;
        const std::string frame = "{:beat " + std::to_string(beat) +
                                  " :tick-n " + std::to_string(tick_n) + "}";
        ctrl.push_frame(nomos::rt::ipc::msg_tick, frame);
        midi.realtime(0xF8);
        did_work = true;
      }
      scheduler.tick(beat, [&](const nomos::rt::clap_event_union &ev) {
        ipc_in_queue.push(ev);
        did_work = true;
      });

      // Tick RT modulators at block rate.  Discard output for now — routing
      // to CLAP params or MIDI CC is a future layer.
      const float tick_rate_hz = static_cast<float>(sample_rate) / static_cast<float>(buffer_frames);
      mod_engine.tick(beat, tick_rate_hz, nullptr);

      while (auto ev = ipc_in_queue.pop()) {
        dispatch_event(*ev);
        did_work = true;
      }
      while (auto ev = hw_midi_in_queue.pop()) {
        dispatch_event(*ev);
        did_work = true;
      }
      while (auto ev = osc_in_queue.pop()) {
        dispatch_event(*ev);
        did_work = true;
      }
      // Drain param events — no consumer in aion yet.
      while (param_queue.pop()) {
        did_work = true;
      }

      if (!did_work)
        std::this_thread::sleep_for(std::chrono::microseconds(250));
    }
  }};

  std::cerr << "[aion]  v" << aion::version() << "\n";
  std::cerr << "[aion]  socket=" << socket_path << " db=" << db_path << "\n";
  std::cerr << "[link]  enabled, bpm=" << initial_bpm << "\n";
  std::cerr << "[osc]   listening on port " << osc_port << "\n";
  if (audio_dev.is_running())
    std::cerr << "[audio] hardware clock discipline active"
              << " sample_rate=" << sample_rate
              << " buffer_frames=" << buffer_frames << "\n";
  else
    std::cerr << "[audio] no hardware clock discipline (software timer only)\n";

  while (g_running.load(std::memory_order_relaxed))
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

  std::cerr << "[aion] shutting down\n";
  audio_dev.stop();
  audio_dev.close();
  osc.stop();
  ctrl.stop();
  event_thread.join();
  midi.close();
  link.enable(false);

  return EXIT_SUCCESS;
}

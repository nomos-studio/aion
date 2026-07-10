// SPDX-License-Identifier: GPL-2.0-or-later

#include <nomos/rt/event_scheduler.hpp>
#include <nomos/rt/input_event.hpp>
#include <nomos/rt/rt_control_thread.hpp>
#include <nomos/rt/spsc_queue.hpp>

#include <nomos/rt/modulator_engine.hpp>

#include "aion_control_thread.hpp"
#include "audio_device.hpp"
#include "link_peer.hpp"
#include "midi_io.hpp"
#include "osc_server.hpp"
#include "routing_matrix.hpp"

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

#include <pthread.h>
#include <signal.h>

namespace aion {
std::string_view version() noexcept;
}

namespace {
std::atomic<bool> g_running{true};
void              on_signal(int) noexcept {
    g_running.store(false, std::memory_order_relaxed);
}

// Install signal handlers via sigaction (not std::signal).
// SA_RESTART is intentionally absent: blocking syscalls (accept, read) return
// EINTR on signal delivery so threads can re-check g_running and exit cleanly.
void install_signal_handlers() noexcept {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // SIGPIPE: write to a closed client returns EPIPE rather than killing the process.
    struct sigaction sa_pipe {};
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    sigaction(SIGPIPE, &sa_pipe, nullptr);
}

// Block all signals on the calling thread.  Call from RT threads (audio
// callback, event dispatch) so that SIGTERM/SIGINT are only delivered to
// the main thread, which owns the g_running flag and the shutdown sequence.
void block_signals_on_this_thread() noexcept {
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, nullptr);
}
} // namespace

int main(int argc, char* argv[]) {
    std::string  socket_path     = "/tmp/aion.sock";
    std::string  db_path         = "aion.db";
    int          midi_port       = -1;
    int          midi_in_port    = -1;
    uint16_t     osc_port        = 9002;
    double       initial_bpm     = 120.0;
    double       sample_rate     = 48000.0;
    uint32_t     buffer_frames   = 64;
    bool         no_audio        = false;
    bool         list_audio_devs = false;
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
            std::cout << "Usage: aion [options]\n"
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

    install_signal_handlers();

    // Shared queues.
    nomos::rt::param_queue       param_queue;
    nomos::rt::input_event_queue ipc_in_queue;
    nomos::rt::input_event_queue hw_midi_in_queue;
    nomos::rt::input_event_queue osc_in_queue;

    // Beat scheduler.
    nomos::rt::event_scheduler scheduler;

    // RT modulator engine.
    nomos::rt::modulator_engine mod_engine;

    // Routing matrix — controls how events flow from sources to MIDI output.
    // Default (empty): all events pass through on their original channel.
    // Updated at runtime via MSG-ROUTE-SET IPC frames.
    aion::RoutingMatrix routing;

    // MIDI I/O — created before the control thread so we can pass &midi into
    // the config for direct SysEx/MTS/CC output from the control thread.
    nomos::rt::midi_io midi;
    nomos::rt::midi_io::list_ports();
    if (midi_port >= 0)
        midi.open_port(static_cast<unsigned int>(midi_port));
    if (midi_in_port >= 0)
        midi.open_input_port(static_cast<unsigned int>(midi_in_port), hw_midi_in_queue);

    // Control thread — IPC socket, session, and common nomos-rt message dispatch.
    // aion_control_thread extends the base to handle msg_route_set.
    aion::aion_control_thread ctrl{nomos::rt::rt_control_thread::config{
                                       .socket_path   = socket_path,
                                       .db_path       = db_path,
                                       .sched_staging = &scheduler.staging(),
                                       .mod_engine    = &mod_engine,
                                       .midi          = &midi,
                                   },
                                   param_queue, ipc_in_queue, routing};
    ctrl.start();

    // Diagnostic tap: push a MSG-MIDI-DIAG frame back to nous for every MIDI
    // send, before the bytes reach RtMidi.  Fires even when no port is open —
    // no hardware needed for CI verification.  Set here, before the event
    // thread starts, so there is no data race on send_cb_.
    midi.set_send_callback([&](const std::vector<uint8_t>& bytes) {
        std::string payload = "{:bytes [";
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            if (i > 0)
                payload += ' ';
            payload += std::to_string(static_cast<unsigned>(bytes[i]));
        }
        payload += "]}";
        ctrl.push_frame(nomos::rt::ipc::msg_midi_diag, payload);
    });

    // Link peer.
    nomos::rt::link_peer link{initial_bpm};
    link.enable(true);

    // OSC server.
    nomos::rt::osc_server osc{osc_port, osc_in_queue};
    osc.start();

    // Audio device — hardware interrupt cadence for clock discipline.
    nomos::rt::audio_device audio_dev;
    if (!no_audio) {
        nomos::rt::audio_device_config audio_cfg{
            .device_id     = audio_device_id,
            .out_channels  = 2,
            .in_channels   = 0,
            .sample_rate   = sample_rate,
            .buffer_frames = buffer_frames,
        };
        const bool opened =
            audio_dev.open(audio_cfg, [](float** out, const float* const*, uint32_t out_ch,
                                         uint32_t, uint32_t nframes, double) {
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

    // Event dispatch thread.
    //
    // Sources routed through the RoutingMatrix:
    //   ipc_in_queue     — note/MIDI events from the IPC connection
    //   hw_midi_in_queue — hardware MIDI input
    //   osc_in_queue     — OSC-driven note events
    //
    // Modulator outputs are routed to MIDI CC each tick (when routes are defined)
    // and also pushed to the connected client as MSG-TICK :mods.
    std::thread event_thread{[&]() {
        block_signals_on_this_thread(); // SIGTERM handled by main thread only
        int64_t last_tick_n = -1;
        mod_engine.pre_warm_reader();

        while (g_running.load(std::memory_order_relaxed)) {
            bool did_work = false;

            // Tick beat-scheduled events.
            const double  beat   = link.beat_at_time(link.now());
            const int64_t tick_n = static_cast<int64_t>(std::floor(beat * 24.0));
            const float   tick_rate_hz =
                static_cast<float>(sample_rate) / static_cast<float>(buffer_frames);

            // Tick RT modulators; capture outputs for MSG-TICK and mod routing.
            std::string mods_edn;
            mod_engine.tick(beat, tick_rate_hz,
                            [&](const std::string& id, const nomos::rt::modulator_output& out) {
                                // Modulator → MIDI CC routing.
                                routing.route_modulator(id, out, midi);

                                // Accumulate for MSG-TICK payload.
                                mods_edn += " :" + id + " {:cv " + std::to_string(out.cv) +
                                            " :aux " + std::to_string(out.aux) + " :gate " +
                                            (out.gate ? "true" : "false") + " :gate2 " +
                                            (out.gate2 ? "true" : "false") + "}";
                            });

            if (tick_n > last_tick_n) {
                last_tick_n = tick_n;
                std::string frame =
                    "{:beat " + std::to_string(beat) + " :tick-n " + std::to_string(tick_n);
                if (!mods_edn.empty())
                    frame += " :mods {" + mods_edn + "}";
                frame += "}";
                ctrl.push_frame(nomos::rt::ipc::msg_tick, frame);
                midi.realtime(0xF8);
                did_work = true;
            }

            scheduler.tick(beat, [&](const nomos::rt::clap_event_union& ev) {
                ipc_in_queue.push(ev);
                did_work = true;
            });

            while (auto ev = ipc_in_queue.pop()) {
                routing.route_event(*ev, aion::MidiRoute::Source::ipc, midi);
                did_work = true;
            }
            while (auto ev = hw_midi_in_queue.pop()) {
                routing.route_event(*ev, aion::MidiRoute::Source::midi_hw, midi);
                did_work = true;
            }
            while (auto ev = osc_in_queue.pop()) {
                routing.route_event(*ev, aion::MidiRoute::Source::osc, midi);
                did_work = true;
            }

            // Drain param events — no param consumer in aion (routed via mod engine).
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
                  << " sample_rate=" << sample_rate << " buffer_frames=" << buffer_frames << "\n";
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

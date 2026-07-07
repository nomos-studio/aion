// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// aion routing matrix — connects event sources to MIDI output sinks.
//
// Two route kinds:
//
//   MidiRoute — maps CLAP note/MIDI events from one source (ipc, midi_hw, osc)
//               to MIDI hardware output, with optional channel override and
//               semitone transpose.
//
//   ModRoute  — maps a named modulator output field (cv, aux, gate, outputs[N])
//               to a MIDI CC message.  Emitted each tick when the CC value
//               changes by at least 1 (dirty tracking).
//
// EDN wire format for msg_route_set:
//   {:midi-routes [{:src     :ipc|:midi-hw|:osc  ; source
//                   :src-ch  -1                   ; -1 = any channel
//                   :dst-ch  -1                   ; -1 = passthrough
//                   :xpose   0}                   ; semitone transpose
//                  ...]
//    :mod-routes  [{:id      "lfo-1"              ; modulator keyword name
//                   :field   :cv|:aux|:gate|:gate2|:out0..15
//                   :ch      0                    ; MIDI channel 0-15
//                   :cc      74                   ; CC number 0-127
//                   :scale   1.0                  ; CV [0,1] → CC [0, scale*127]
//                   :offset  0.0}                 ; offset applied before scale
//                  ...]}
//
// Default (empty) matrix: all events from all sources pass through to MIDI out
// on their original channel, no transpose, no modulator CC routing.

#include <nomos/rt/abstract_modulator.hpp>
#include <nomos/rt/input_event.hpp>

#include <edn/builtins.hpp>
#include <edn/parser.hpp>
#include <edn/value.hpp>

#include <clap/events.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace aion {

struct MidiRoute {
    enum class Source : uint8_t { ipc = 0, midi_hw = 1, osc = 2 };
    Source src{Source::ipc};
    int    src_ch{-1}; // -1 = any
    int    dst_ch{-1}; // -1 = passthrough
    int    xpose{0};   // semitone transpose
};

struct ModRoute {
    std::string id;       // modulator keyword name (without leading ':')
    int         field{0}; // 0=cv, 1=aux, 2=gate, 3=gate2, 4+= outputs[N-4]
    uint8_t     ch{0};    // MIDI channel 0-15
    uint8_t     cc{0};    // CC number 0-127
    float       scale{1.0f};
    float       offset{0.0f};
};

// Concept satisfied by nomos::rt::midi_io and by test stubs:
//   sink.note_on(uint8_t ch, uint8_t note, uint8_t vel)
//   sink.note_off(uint8_t ch, uint8_t note)
//   sink.cc(uint8_t ch, uint8_t cc_num, uint8_t val)
//   sink.send(const std::vector<uint8_t>&)

class RoutingMatrix {
  public:
    // Route a CLAP event from src through the matrix to Sink.
    // Called from the event thread — reads midi_routes_ under lock.
    // Sink must satisfy the midi sink concept above.
    template <typename Sink>
    void route_event(const nomos::rt::clap_event_union& ev, MidiRoute::Source src,
                     Sink& sink) const {
        std::lock_guard<std::mutex> lock(mutex_);

        if (midi_routes_.empty()) {
            // Default: pass everything through.
            dispatch(ev, -1, 0, sink);
            return;
        }

        for (const auto& r : midi_routes_) {
            if (r.src != src)
                continue;
            const int ev_ch = note_channel(ev);
            if (r.src_ch != -1 && ev_ch != -1 && r.src_ch != ev_ch)
                continue;
            dispatch(ev, r.dst_ch, r.xpose, sink);
        }
    }

    // Route modulator outputs to MIDI CC.
    // Called from the event thread each tick.
    // Sink must satisfy the midi sink concept above.
    template <typename Sink>
    void route_modulator(const std::string& id, const nomos::rt::modulator_output& out,
                         Sink& sink) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& r : mod_routes_) {
            if (r.id != id)
                continue;
            const float raw = field_value(out, r.field);
            const float cv  = std::clamp(raw * r.scale + r.offset, 0.0f, 1.0f);
            const auto  val = static_cast<uint8_t>(std::round(cv * 127.0f));

            const uint32_t key = (static_cast<uint32_t>(r.ch) << 8) | r.cc;
            auto           it  = last_cc_.find(key);
            if (it != last_cc_.end() && it->second == val)
                continue;
            last_cc_[key] = val;

            sink.cc(static_cast<uint8_t>(r.ch + 1), r.cc, val);
        }
    }

    // Replace the routing table from an EDN payload.
    // Called from the control thread (dispatch_extension); lock is held internally.
    void apply_edn(std::string_view edn) {
        auto parsed = edn::parse(std::string(edn));
        if (!parsed || !parsed->is<edn::map>())
            return;

        const auto& m = parsed->get<edn::map>();

        std::vector<MidiRoute> new_midi;
        std::vector<ModRoute>  new_mod;

        if (const auto* mr = m.find_kw("midi-routes"); mr && mr->is<edn::vector>()) {
            for (const auto& entry : mr->get<edn::vector>().items) {
                if (!entry.is<edn::map>())
                    continue;
                const auto& em = entry.get<edn::map>();
                MidiRoute   r;
                r.src    = parse_source(em);
                r.src_ch = int_field(em, "src-ch", -1);
                r.dst_ch = int_field(em, "dst-ch", -1);
                r.xpose  = int_field(em, "xpose", 0);
                new_midi.push_back(r);
            }
        }

        if (const auto* mr = m.find_kw("mod-routes"); mr && mr->is<edn::vector>()) {
            for (const auto& entry : mr->get<edn::vector>().items) {
                if (!entry.is<edn::map>())
                    continue;
                const auto& em   = entry.get<edn::map>();
                const auto* id_v = em.find_kw("id");
                if (!id_v || !id_v->is<std::string>())
                    continue;
                ModRoute r;
                r.id     = id_v->get<std::string>();
                r.field  = parse_field(em);
                r.ch     = static_cast<uint8_t>(std::clamp(int_field(em, "ch", 0), 0, 15));
                r.cc     = static_cast<uint8_t>(std::clamp(int_field(em, "cc", 0), 0, 127));
                r.scale  = float_field(em, "scale", 1.0f);
                r.offset = float_field(em, "offset", 0.0f);
                new_mod.push_back(r);
            }
        }

        std::lock_guard<std::mutex> lock(mutex_);
        midi_routes_ = std::move(new_midi);
        mod_routes_  = std::move(new_mod);
        last_cc_.clear();
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return midi_routes_.empty() && mod_routes_.empty();
    }

  private:
    // ---------------------------------------------------------------------------
    // Dispatch helpers
    // ---------------------------------------------------------------------------

    static int note_channel(const nomos::rt::clap_event_union& ev) {
        switch (ev.header.type) {
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
            return ev.note.channel;
        case CLAP_EVENT_MIDI:
            return ev.midi.data[0] & 0x0F;
        default:
            return -1;
        }
    }

    template <typename Sink>
    static void dispatch(const nomos::rt::clap_event_union& ev, int dst_ch, int xpose, Sink& sink) {
        switch (ev.header.type) {
        case CLAP_EVENT_NOTE_ON: {
            const int  ch  = dst_ch >= 0 ? dst_ch : ev.note.channel;
            const int  key = std::clamp(ev.note.key + xpose, 0, 127);
            const auto vel = static_cast<uint8_t>(std::clamp(ev.note.velocity * 127.0, 0.0, 127.0));
            sink.note_on(static_cast<uint8_t>(ch + 1), static_cast<uint8_t>(key), vel);
            break;
        }
        case CLAP_EVENT_NOTE_OFF: {
            const int ch  = dst_ch >= 0 ? dst_ch : ev.note.channel;
            const int key = std::clamp(ev.note.key + xpose, 0, 127);
            sink.note_off(static_cast<uint8_t>(ch + 1), static_cast<uint8_t>(key));
            break;
        }
        case CLAP_EVENT_MIDI:
            sink.send({ev.midi.data[0], ev.midi.data[1], ev.midi.data[2]});
            break;
        default:
            break;
        }
    }

    static float field_value(const nomos::rt::modulator_output& out, int field) {
        switch (field) {
        case 0:
            return out.cv;
        case 1:
            return out.aux;
        case 2:
            return out.gate ? 1.0f : 0.0f;
        case 3:
            return out.gate2 ? 1.0f : 0.0f;
        default: {
            const int i = field - 4;
            if (i >= 0 && i < nomos::rt::modulator_output::kMaxOutputs)
                return out.outputs[i];
            return 0.0f;
        }
        }
    }

    // ---------------------------------------------------------------------------
    // EDN parse helpers
    // ---------------------------------------------------------------------------

    static MidiRoute::Source parse_source(const edn::map& m) {
        const auto* v = m.find_kw("src");
        if (!v || !v->is<edn::keyword>())
            return MidiRoute::Source::ipc;
        const auto& kw = v->get<edn::keyword>();
        if (kw == edn::keyword{"midi-hw"})
            return MidiRoute::Source::midi_hw;
        if (kw == edn::keyword{"osc"})
            return MidiRoute::Source::osc;
        return MidiRoute::Source::ipc;
    }

    static int parse_field(const edn::map& m) {
        const auto* v = m.find_kw("field");
        if (!v)
            return 0;
        if (v->is<edn::keyword>()) {
            const auto& kw = v->get<edn::keyword>();
            if (kw == edn::keyword{"cv"})
                return 0;
            if (kw == edn::keyword{"aux"})
                return 1;
            if (kw == edn::keyword{"gate"})
                return 2;
            if (kw == edn::keyword{"gate2"})
                return 3;
            // :out0 .. :out15 — keyword name is e.g. "out0", "out7"
            if (kw.name.size() > 3 && kw.name.substr(0, 3) == "out") {
                try {
                    return 4 + std::stoi(std::string(kw.name.substr(3)));
                } catch (...) {
                }
            }
        }
        return 0;
    }

    static int int_field(const edn::map& m, const char* key, int def) {
        const auto* v = m.find_kw(key);
        if (!v)
            return def;
        if (v->is<int64_t>())
            return static_cast<int>(v->get<int64_t>());
        return def;
    }

    static float float_field(const edn::map& m, const char* key, float def) {
        const auto* v = m.find_kw(key);
        if (!v)
            return def;
        if (v->is<double>())
            return static_cast<float>(v->get<double>());
        if (v->is<int64_t>())
            return static_cast<float>(v->get<int64_t>());
        return def;
    }

    mutable std::mutex                    mutex_;
    std::vector<MidiRoute>                midi_routes_;
    std::vector<ModRoute>                 mod_routes_;
    std::unordered_map<uint32_t, uint8_t> last_cc_; // (ch<<8)|cc → last value
};

} // namespace aion

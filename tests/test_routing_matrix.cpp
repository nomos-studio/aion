// SPDX-License-Identifier: GPL-2.0-or-later
//
// RoutingMatrix unit tests.
//
// Uses a CaptureSink stub instead of nomos::rt::midi_io so tests run
// without a MIDI device or audio hardware.

#include "routing_matrix.hpp"

#include <catch2/catch_test_macros.hpp>

#include <clap/events.h>

#include <cstdint>
#include <vector>

using namespace aion;

// ---------------------------------------------------------------------------
// Test stub — records all MIDI output calls for inspection
// ---------------------------------------------------------------------------

struct CaptureSink {
    struct Event {
        enum class Kind { NoteOn, NoteOff, CC, Raw };
        Kind    kind;
        uint8_t a{0}, b{0}, c{0};
    };
    std::vector<Event> events;

    void note_on(uint8_t ch, uint8_t note, uint8_t vel) {
        events.push_back({Event::Kind::NoteOn, ch, note, vel});
    }
    void note_off(uint8_t ch, uint8_t note) {
        events.push_back({Event::Kind::NoteOff, ch, note, 0});
    }
    void cc(uint8_t ch, uint8_t cc_num, uint8_t val) {
        events.push_back({Event::Kind::CC, ch, cc_num, val});
    }
    void send(const std::vector<uint8_t>& bytes) {
        if (bytes.size() >= 3)
            events.push_back({Event::Kind::Raw, bytes[0], bytes[1], bytes[2]});
    }

    void clear() { events.clear(); }
    bool empty() const { return events.empty(); }
};

// ---------------------------------------------------------------------------
// CLAP event helpers
// ---------------------------------------------------------------------------

static nomos::rt::clap_event_union make_note_on(int ch, int key, double vel) {
    nomos::rt::clap_event_union ev{};
    ev.header.type     = CLAP_EVENT_NOTE_ON;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.note.channel    = static_cast<int16_t>(ch);
    ev.note.key        = static_cast<int16_t>(key);
    ev.note.velocity   = vel;
    return ev;
}

static nomos::rt::clap_event_union make_note_off(int ch, int key) {
    nomos::rt::clap_event_union ev{};
    ev.header.type     = CLAP_EVENT_NOTE_OFF;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.note.channel    = static_cast<int16_t>(ch);
    ev.note.key        = static_cast<int16_t>(key);
    ev.note.velocity   = 0.0;
    return ev;
}

// ---------------------------------------------------------------------------
// apply_edn — parsing
// ---------------------------------------------------------------------------

TEST_CASE("RoutingMatrix: empty by default", "[routing]") {
    RoutingMatrix rm;
    REQUIRE(rm.empty());
}

TEST_CASE("RoutingMatrix: apply_edn populates midi routes", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch 0 :xpose 0}
                                   {:src :midi-hw :src-ch 2 :dst-ch 3 :xpose 12}]})");
    REQUIRE_FALSE(rm.empty());
}

TEST_CASE("RoutingMatrix: apply_edn populates mod routes", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0}]})");
    REQUIRE_FALSE(rm.empty());
}

TEST_CASE("RoutingMatrix: apply_edn with empty map resets routes", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch 0}]})");
    REQUIRE_FALSE(rm.empty());
    rm.apply_edn("{}");
    REQUIRE(rm.empty());
}

TEST_CASE("RoutingMatrix: apply_edn ignores malformed EDN gracefully", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn("this is not edn");
    REQUIRE(rm.empty());     // malformed → no change
    rm.apply_edn("[1 2 3]"); // not a map
    REQUIRE(rm.empty());
}

// ---------------------------------------------------------------------------
// route_event — MIDI dispatch
// ---------------------------------------------------------------------------

TEST_CASE("RoutingMatrix: empty matrix passes all sources through", "[routing]") {
    RoutingMatrix rm;
    CaptureSink   sink;

    rm.route_event(make_note_on(0, 60, 0.8), MidiRoute::Source::ipc, sink);
    rm.route_event(make_note_on(0, 62, 0.5), MidiRoute::Source::midi_hw, sink);
    rm.route_event(make_note_on(0, 64, 0.6), MidiRoute::Source::osc, sink);

    REQUIRE(sink.events.size() == 3);
    for (const auto& e : sink.events)
        REQUIRE(e.kind == CaptureSink::Event::Kind::NoteOn);
}

TEST_CASE("RoutingMatrix: route filters by source", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch -1 :xpose 0}]})");
    CaptureSink sink;

    rm.route_event(make_note_on(0, 60, 0.8), MidiRoute::Source::ipc, sink);
    rm.route_event(make_note_on(0, 60, 0.8), MidiRoute::Source::midi_hw, sink);
    rm.route_event(make_note_on(0, 60, 0.8), MidiRoute::Source::osc, sink);

    // Only the ipc event should pass through
    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].kind == CaptureSink::Event::Kind::NoteOn);
}

TEST_CASE("RoutingMatrix: route applies channel override", "[routing]") {
    RoutingMatrix rm;
    // ipc events → output channel 3 (0-indexed; note_on receives ch+1 = 4)
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch 3 :xpose 0}]})");
    CaptureSink sink;

    rm.route_event(make_note_on(0, 60, 1.0), MidiRoute::Source::ipc, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].kind == CaptureSink::Event::Kind::NoteOn);
    REQUIRE(sink.events[0].a == 4); // ch+1
}

TEST_CASE("RoutingMatrix: route applies semitone transpose", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch -1 :xpose 12}]})");
    CaptureSink sink;

    rm.route_event(make_note_on(0, 60, 1.0), MidiRoute::Source::ipc, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].b == 72); // key 60 + 12
}

TEST_CASE("RoutingMatrix: route clamps transposed key to [0,127]", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :ipc :src-ch -1 :dst-ch -1 :xpose 100}]})");
    CaptureSink sink;

    rm.route_event(make_note_on(0, 120, 1.0), MidiRoute::Source::ipc, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].b == 127); // clamped
}

TEST_CASE("RoutingMatrix: route filters by source channel", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:midi-routes [{:src :midi-hw :src-ch 2 :dst-ch -1 :xpose 0}]})");
    CaptureSink sink;

    rm.route_event(make_note_on(0, 60, 0.8), MidiRoute::Source::midi_hw, sink);
    rm.route_event(make_note_on(2, 60, 0.8), MidiRoute::Source::midi_hw, sink);
    rm.route_event(make_note_on(5, 60, 0.8), MidiRoute::Source::midi_hw, sink);

    // Only channel 2 matches
    REQUIRE(sink.events.size() == 1);
}

TEST_CASE("RoutingMatrix: note-off is dispatched correctly", "[routing]") {
    RoutingMatrix rm;
    CaptureSink   sink;

    rm.route_event(make_note_off(0, 60), MidiRoute::Source::ipc, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].kind == CaptureSink::Event::Kind::NoteOff);
    REQUIRE(sink.events[0].b == 60);
}

// ---------------------------------------------------------------------------
// route_modulator — CC mapping
// ---------------------------------------------------------------------------

TEST_CASE("RoutingMatrix: mod route maps cv to CC", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0 :offset 0.0}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.5f;
    rm.route_modulator("lfo-1", out, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].kind == CaptureSink::Event::Kind::CC);
    REQUIRE(sink.events[0].a == 1);  // ch 0 → cc channel 1
    REQUIRE(sink.events[0].b == 74); // cc number
    REQUIRE(sink.events[0].c == 64); // round(0.5 * 127) = round(63.5) = 64
}

TEST_CASE("RoutingMatrix: mod route dirty-tracks CC — no redundant sends", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.5f;

    rm.route_modulator("lfo-1", out, sink);
    rm.route_modulator("lfo-1", out, sink); // same value
    rm.route_modulator("lfo-1", out, sink); // same value

    REQUIRE(sink.events.size() == 1); // only one CC sent
}

TEST_CASE("RoutingMatrix: mod route sends again when CV changes", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.5f;
    rm.route_modulator("lfo-1", out, sink);

    out.cv = 0.75f;
    rm.route_modulator("lfo-1", out, sink);

    REQUIRE(sink.events.size() == 2);
    REQUIRE(sink.events[1].c == 95); // round(0.75 * 127)
}

TEST_CASE("RoutingMatrix: mod route ignored for unknown modulator id", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.5f;
    rm.route_modulator("lfo-2", out, sink); // wrong id

    REQUIRE(sink.empty());
}

TEST_CASE("RoutingMatrix: mod route maps gate field", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "env" :field :gate :ch 0 :cc 64 :scale 1.0}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.gate = true;
    rm.route_modulator("env", out, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].c == 127); // gate=true → cv=1.0 → 127
}

TEST_CASE("RoutingMatrix: mod route applies scale and offset", "[routing]") {
    RoutingMatrix rm;
    // Map [0,1] cv → [64,127]: offset=0.5, scale=0.5 → clamp to [0,1] → * 127
    rm.apply_edn(R"({:mod-routes [{:id "m" :field :cv :ch 0 :cc 1 :scale 0.5 :offset 0.5}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.0f;
    rm.route_modulator("m", out, sink);

    REQUIRE(sink.events.size() == 1);
    REQUIRE(sink.events[0].c == 64); // clamp(0.0*0.5+0.5,0,1)*127 = round(63.5) = 64
}

TEST_CASE("RoutingMatrix: apply_edn clears dirty CC cache", "[routing]") {
    RoutingMatrix rm;
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0}]})");
    CaptureSink sink;

    nomos::rt::modulator_output out{};
    out.cv = 0.5f;
    rm.route_modulator("lfo-1", out, sink); // first send

    // Re-apply same routes — cache should be cleared
    rm.apply_edn(R"({:mod-routes [{:id "lfo-1" :field :cv :ch 0 :cc 74 :scale 1.0}]})");
    rm.route_modulator("lfo-1", out, sink); // should send again

    REQUIRE(sink.events.size() == 2);
}

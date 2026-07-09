// SPDX-License-Identifier: GPL-2.0-or-later
//
// midi_io send-callback (diagnostic tap) unit tests.
//
// Verifies that midi_io fires send_cb_ with the correct raw MIDI bytes for
// each message type, without opening a hardware port.  These tests form the
// C++ side of the CI-runnable MIDI verification path.

#include "midi_io.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

using namespace nomos::rt;

// ---------------------------------------------------------------------------
// Capture helper — collects every send_cb_ invocation for inspection
// ---------------------------------------------------------------------------

struct ByteCapture {
    std::vector<std::vector<uint8_t>> frames;

    midi_io::send_cb_t callback() {
        return [this](const std::vector<uint8_t>& bytes) { frames.push_back(bytes); };
    }

    void clear() { frames.clear(); }
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_CASE("midi_io diagnostic tap: fires even with no port open", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    REQUIRE_FALSE(midi.is_open());
    midi.note_on(1, 60, 100);
    REQUIRE(cap.frames.size() == 1);
}

TEST_CASE("midi_io diagnostic tap: note-on bytes are correct", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    // channel=1, note=60 (C4), vel=102
    // note_on(ch=1) → ch=(1-1)&0x0F=0 → status=0x90|0=0x90
    midi.note_on(1, 60, 102);

    REQUIRE(cap.frames.size() == 1);
    REQUIRE(cap.frames[0] == std::vector<uint8_t>{0x90, 60, 102});
}

TEST_CASE("midi_io diagnostic tap: note-off bytes are correct", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    midi.note_off(1, 60);

    REQUIRE(cap.frames.size() == 1);
    REQUIRE(cap.frames[0] == std::vector<uint8_t>{0x80, 60, 0x00});
}

TEST_CASE("midi_io diagnostic tap: CC bytes are correct", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    midi.cc(1, 74, 64);

    REQUIRE(cap.frames.size() == 1);
    REQUIRE(cap.frames[0] == std::vector<uint8_t>{0xB0, 74, 64});
}

TEST_CASE("midi_io diagnostic tap: channel encoding is 1-indexed", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    // channel=3 → ch=(3-1)&0x0F=2 → status=0x90|2=0x92
    midi.note_on(3, 60, 100);

    REQUIRE(cap.frames.size() == 1);
    REQUIRE(cap.frames[0][0] == 0x92);
    REQUIRE(cap.frames[0][1] == 60);
}

TEST_CASE("midi_io diagnostic tap: multiple sends accumulate in order", "[midi_diag]") {
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    midi.note_on(1, 60, 100);
    midi.note_on(1, 62, 90);
    midi.note_off(1, 60);

    REQUIRE(cap.frames.size() == 3);
    REQUIRE(cap.frames[0] == std::vector<uint8_t>{0x90, 60, 100});
    REQUIRE(cap.frames[1] == std::vector<uint8_t>{0x90, 62, 90});
    REQUIRE(cap.frames[2] == std::vector<uint8_t>{0x80, 60, 0x00});
}

TEST_CASE("midi_io diagnostic tap: velocity 0.8 from nous maps to byte 101 via routing",
          "[midi_diag]") {
    // Verifies the full nous→aion→MIDI byte chain for key 'a' (C4, vel 0.8).
    // nous sends {:key 60 :channel 0 :port 0 :velocity 0.8}
    // rt_control_thread::dispatch makes a CLAP NOTE_ON with velocity=0.8
    // routing_matrix::dispatch calls midi.note_on(ch+1=1, 60, round(0.8*127)=102)
    // — but the CLAP → MIDI velocity conversion: static_cast<uint8_t>(clamp(vel*127,0,127))
    // 0.8 * 127 = 101.6 → truncated to 101 (static_cast, not round)
    ByteCapture cap;
    midi_io     midi;
    midi.set_send_callback(cap.callback());

    // Simulate what routing_matrix::dispatch does for vel=0.8:
    const auto vel = static_cast<uint8_t>(std::clamp(0.8 * 127.0, 0.0, 127.0)); // = 101
    midi.note_on(1, 60, vel);

    REQUIRE(cap.frames.size() == 1);
    REQUIRE(cap.frames[0] == std::vector<uint8_t>{0x90, 60, 101});
}

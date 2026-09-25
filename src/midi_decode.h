// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

// midi_decode.h - Interpretation of raw MIDI bytes into human-readable form.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace midi {

enum class Category {
    NoteOn,
    NoteOff,
    PolyAftertouch,
    ControlChange,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    SysEx,
    MtcQuarterFrame,
    SongPosition,
    SongSelect,
    TuneRequest,
    Clock,
    Start,
    Continue,
    Stop,
    ActiveSensing,
    SystemReset,
    Unknown
};

// One bit per filter checkbox in the UI.
enum FilterBit : unsigned {
    F_NOTE       = 1u << 0,
    F_CC         = 1u << 1,
    F_PITCHBEND  = 1u << 2,
    F_AFTERTOUCH = 1u << 3,
    F_PROGRAM    = 1u << 4,
    F_SYSEX      = 1u << 5,
    F_CLOCK      = 1u << 6,
    F_TRANSPORT  = 1u << 7,
    F_SENSING    = 1u << 8,
    F_OTHER      = 1u << 9,
    F_ALL        = (1u << 10) - 1
};

// Clock and active sensing arrive hundreds of times a second and drown out
// everything else, so they start hidden.
constexpr unsigned kDefaultFilter = F_ALL & ~(F_CLOCK | F_SENSING);

Category       categorize(uint8_t status, uint8_t d1);
unsigned       filterBitFor(Category c);
const wchar_t* categoryName(Category c);

// Number of data bytes that follow a status byte (0, 1 or 2).
int dataByteCount(uint8_t status);

// True for 0x80..0xEF, where the low nibble is a channel number.
bool isChannelMessage(uint8_t status);

const wchar_t* ccName(uint8_t cc);
const wchar_t* gmProgramName(uint8_t program);
std::wstring   noteName(uint8_t note);        // 60 -> "C4"

// Column text for the live feed.
std::wstring data1Text(uint8_t status, uint8_t d1);
std::wstring data2Text(uint8_t status, uint8_t d1, uint8_t d2);
std::wstring describe(uint8_t status, uint8_t d1, uint8_t d2,
                      const uint8_t* sysex, size_t sysexLen);

// SysEx helpers.
std::wstring   manufacturerName(const uint8_t* data, size_t len);
std::wstring   hexBytes(const uint8_t* data, size_t len, size_t maxBytes = 32);

// Pitch bend as a signed value around centre (0 = centred, range -8192..+8191).
int pitchBendValue(uint8_t d1, uint8_t d2);

// Estimates tempo from the timing clock, which MIDI defines as 24 messages per
// quarter note. Averaging the most recent gaps smooths out transport jitter;
// ten of them settles quickly without lagging a tempo change by much.
class ClockTracker {
public:
    static constexpr size_t kDefaultWindow = 10;

    explicit ClockTracker(size_t window = kDefaultWindow) : window_(window ? window : 1) {}

    // Feeds one clock's timestamp and returns the current estimate, or 0 while
    // there is not yet enough to go on.
    double push(double timeMs);

    double   bpm() const   { return bpm_; }
    uint64_t count() const { return count_; }
    void     reset();

private:
    size_t             window_;
    double             lastMs_ = -1.0;
    std::deque<double> intervals_;
    uint64_t           count_  = 0;
    double             bpm_    = 0.0;
};

} // namespace midi

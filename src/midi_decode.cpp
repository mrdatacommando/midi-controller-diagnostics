// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

#include "midi_decode.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace midi {
namespace {

// Standard MIDI CC assignments (MMA). Entries marked "Undefined" genuinely are
// undefined in the spec, so a controller sending one is free to mean anything.
const wchar_t* const kCcNames[128] = {
    L"Bank Select (MSB)",        L"Modulation Wheel",         L"Breath Controller",         L"Undefined",
    L"Foot Controller",          L"Portamento Time",          L"Data Entry (MSB)",          L"Channel Volume",
    L"Balance",                  L"Undefined",                L"Pan",                       L"Expression",
    L"Effect Control 1",         L"Effect Control 2",         L"Undefined",                 L"Undefined",
    L"General Purpose 1",        L"General Purpose 2",        L"General Purpose 3",         L"General Purpose 4",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Bank Select (LSB)",        L"Modulation Wheel (LSB)",   L"Breath Controller (LSB)",   L"Undefined (LSB)",
    L"Foot Controller (LSB)",    L"Portamento Time (LSB)",    L"Data Entry (LSB)",          L"Channel Volume (LSB)",
    L"Balance (LSB)",            L"Undefined (LSB)",          L"Pan (LSB)",                 L"Expression (LSB)",
    L"Effect Control 1 (LSB)",   L"Effect Control 2 (LSB)",   L"Undefined",                 L"Undefined",
    L"General Purpose 1 (LSB)",  L"General Purpose 2 (LSB)",  L"General Purpose 3 (LSB)",   L"General Purpose 4 (LSB)",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Sustain Pedal",            L"Portamento On/Off",        L"Sostenuto Pedal",           L"Soft Pedal",
    L"Legato Footswitch",        L"Hold 2",                   L"Sound Ctrl 1 (Variation)",  L"Sound Ctrl 2 (Timbre)",
    L"Sound Ctrl 3 (Release)",   L"Sound Ctrl 4 (Attack)",    L"Sound Ctrl 5 (Brightness)", L"Sound Ctrl 6 (Decay)",
    L"Sound Ctrl 7 (Vib Rate)",  L"Sound Ctrl 8 (Vib Depth)", L"Sound Ctrl 9 (Vib Delay)",  L"Sound Ctrl 10",
    L"General Purpose 5",        L"General Purpose 6",        L"General Purpose 7",         L"General Purpose 8",
    L"Portamento Control",       L"Undefined",                L"Undefined",                 L"Undefined",
    L"High Resolution Velocity", L"Undefined",                L"Undefined",                 L"Reverb Send",
    L"Tremolo Depth",            L"Chorus Send",              L"Detune Depth",              L"Phaser Depth",
    L"Data Increment",           L"Data Decrement",           L"NRPN (LSB)",                L"NRPN (MSB)",
    L"RPN (LSB)",                L"RPN (MSB)",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"Undefined",                L"Undefined",                L"Undefined",                 L"Undefined",
    L"All Sound Off",            L"Reset All Controllers",    L"Local Control On/Off",      L"All Notes Off",
    L"Omni Mode Off",            L"Omni Mode On",             L"Mono Mode On",              L"Poly Mode On"
};

const wchar_t* const kGmNames[128] = {
    L"Acoustic Grand Piano",     L"Bright Acoustic Piano",    L"Electric Grand Piano",      L"Honky-tonk Piano",
    L"Electric Piano 1",         L"Electric Piano 2",         L"Harpsichord",               L"Clavi",
    L"Celesta",                  L"Glockenspiel",             L"Music Box",                 L"Vibraphone",
    L"Marimba",                  L"Xylophone",                L"Tubular Bells",             L"Dulcimer",
    L"Drawbar Organ",            L"Percussive Organ",         L"Rock Organ",                L"Church Organ",
    L"Reed Organ",               L"Accordion",                L"Harmonica",                 L"Tango Accordion",
    L"Acoustic Guitar (nylon)",  L"Acoustic Guitar (steel)",  L"Electric Guitar (jazz)",    L"Electric Guitar (clean)",
    L"Electric Guitar (muted)",  L"Overdriven Guitar",        L"Distortion Guitar",         L"Guitar Harmonics",
    L"Acoustic Bass",            L"Electric Bass (finger)",   L"Electric Bass (pick)",      L"Fretless Bass",
    L"Slap Bass 1",              L"Slap Bass 2",              L"Synth Bass 1",              L"Synth Bass 2",
    L"Violin",                   L"Viola",                    L"Cello",                     L"Contrabass",
    L"Tremolo Strings",          L"Pizzicato Strings",        L"Orchestral Harp",           L"Timpani",
    L"String Ensemble 1",        L"String Ensemble 2",        L"Synth Strings 1",           L"Synth Strings 2",
    L"Choir Aahs",               L"Voice Oohs",               L"Synth Voice",               L"Orchestra Hit",
    L"Trumpet",                  L"Trombone",                 L"Tuba",                      L"Muted Trumpet",
    L"French Horn",              L"Brass Section",            L"Synth Brass 1",             L"Synth Brass 2",
    L"Soprano Sax",              L"Alto Sax",                 L"Tenor Sax",                 L"Baritone Sax",
    L"Oboe",                     L"English Horn",             L"Bassoon",                   L"Clarinet",
    L"Piccolo",                  L"Flute",                    L"Recorder",                  L"Pan Flute",
    L"Blown Bottle",             L"Shakuhachi",               L"Whistle",                   L"Ocarina",
    L"Lead 1 (square)",          L"Lead 2 (sawtooth)",        L"Lead 3 (calliope)",         L"Lead 4 (chiff)",
    L"Lead 5 (charang)",         L"Lead 6 (voice)",           L"Lead 7 (fifths)",           L"Lead 8 (bass+lead)",
    L"Pad 1 (new age)",          L"Pad 2 (warm)",             L"Pad 3 (polysynth)",         L"Pad 4 (choir)",
    L"Pad 5 (bowed)",            L"Pad 6 (metallic)",         L"Pad 7 (halo)",              L"Pad 8 (sweep)",
    L"FX 1 (rain)",              L"FX 2 (soundtrack)",        L"FX 3 (crystal)",            L"FX 4 (atmosphere)",
    L"FX 5 (brightness)",        L"FX 6 (goblins)",           L"FX 7 (echoes)",             L"FX 8 (sci-fi)",
    L"Sitar",                    L"Banjo",                    L"Shamisen",                  L"Koto",
    L"Kalimba",                  L"Bag pipe",                 L"Fiddle",                    L"Shanai",
    L"Tinkle Bell",              L"Agogo",                    L"Steel Drums",               L"Woodblock",
    L"Taiko Drum",               L"Melodic Tom",              L"Synth Drum",                L"Reverse Cymbal",
    L"Guitar Fret Noise",        L"Breath Noise",             L"Seashore",                  L"Bird Tweet",
    L"Telephone Ring",           L"Helicopter",               L"Applause",                  L"Gunshot"
};

const wchar_t* const kNoteLetters[12] = {
    L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"
};

struct Manufacturer {
    uint32_t       id;   // one-byte id, or the 16-bit tail of an 00 xx yy id
    const wchar_t* name;
};

// Deliberately limited to IDs worth trusting. Anything not listed falls back to
// printing the raw ID, which beats naming the wrong company in a diagnostic.
const Manufacturer kManufacturers1[] = {
    {0x01, L"Sequential Circuits"}, {0x04, L"Moog"},      {0x05, L"Passport Designs"},
    {0x06, L"Lexicon"},             {0x07, L"Kurzweil"},  {0x0E, L"Alesis"},
    {0x10, L"Oberheim"},            {0x15, L"JL Cooper"}, {0x18, L"E-mu"},
    {0x1C, L"Eventide"},            {0x3E, L"Waldorf"},   {0x40, L"Kawai"},
    {0x41, L"Roland"},              {0x42, L"Korg"},      {0x43, L"Yamaha"},
    {0x44, L"Casio"},               {0x47, L"Akai"},      {0x4C, L"Sony"},
    {0x52, L"Zoom"},
    {0x7D, L"Non-commercial / educational"},
    {0x7E, L"Universal Non-Real Time"},
    {0x7F, L"Universal Real Time"},
};

const Manufacturer kManufacturers3[] = {
    {0x003B, L"MOTU"},     {0x010C, L"Line 6"},   {0x2029, L"Focusrite / Novation"},
    {0x2032, L"Behringer"},{0x206B, L"Arturia"},  {0x2109, L"Native Instruments"},
};

std::wstring formatf(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

} // namespace

bool isChannelMessage(uint8_t status) {
    return status >= 0x80 && status <= 0xEF;
}

int dataByteCount(uint8_t status) {
    if (isChannelMessage(status)) {
        const uint8_t type = status & 0xF0;
        return (type == 0xC0 || type == 0xD0) ? 1 : 2;
    }
    switch (status) {
        case 0xF1: return 1;   // MTC quarter frame
        case 0xF2: return 2;   // Song position pointer
        case 0xF3: return 1;   // Song select
        default:   return 0;
    }
}

Category categorize(uint8_t status, uint8_t d1) {
    (void)d1;
    if (isChannelMessage(status)) {
        switch (status & 0xF0) {
            case 0x80: return Category::NoteOff;
            case 0x90: return Category::NoteOn;   // velocity 0 is flagged in describe()
            case 0xA0: return Category::PolyAftertouch;
            case 0xB0: return Category::ControlChange;
            case 0xC0: return Category::ProgramChange;
            case 0xD0: return Category::ChannelAftertouch;
            case 0xE0: return Category::PitchBend;
            default:   return Category::Unknown;
        }
    }
    switch (status) {
        case 0xF0: case 0xF7: return Category::SysEx;
        case 0xF1: return Category::MtcQuarterFrame;
        case 0xF2: return Category::SongPosition;
        case 0xF3: return Category::SongSelect;
        case 0xF6: return Category::TuneRequest;
        case 0xF8: return Category::Clock;
        case 0xFA: return Category::Start;
        case 0xFB: return Category::Continue;
        case 0xFC: return Category::Stop;
        case 0xFE: return Category::ActiveSensing;
        case 0xFF: return Category::SystemReset;
        default:   return Category::Unknown;
    }
}

unsigned filterBitFor(Category c) {
    switch (c) {
        case Category::NoteOn:
        case Category::NoteOff:            return F_NOTE;
        case Category::ControlChange:      return F_CC;
        case Category::PitchBend:          return F_PITCHBEND;
        case Category::PolyAftertouch:
        case Category::ChannelAftertouch:  return F_AFTERTOUCH;
        case Category::ProgramChange:      return F_PROGRAM;
        case Category::SysEx:              return F_SYSEX;
        case Category::Clock:              return F_CLOCK;
        case Category::Start:
        case Category::Continue:
        case Category::Stop:
        case Category::SongPosition:
        case Category::SongSelect:         return F_TRANSPORT;
        case Category::ActiveSensing:      return F_SENSING;
        default:                           return F_OTHER;
    }
}

const wchar_t* categoryName(Category c) {
    switch (c) {
        case Category::NoteOn:            return L"Note On";
        case Category::NoteOff:           return L"Note Off";
        case Category::PolyAftertouch:    return L"Poly Aftertouch";
        case Category::ControlChange:     return L"Control Change";
        case Category::ProgramChange:     return L"Program Change";
        case Category::ChannelAftertouch: return L"Ch Aftertouch";
        case Category::PitchBend:         return L"Pitch Bend";
        case Category::SysEx:             return L"SysEx";
        case Category::MtcQuarterFrame:   return L"MTC Quarter Frame";
        case Category::SongPosition:      return L"Song Position";
        case Category::SongSelect:        return L"Song Select";
        case Category::TuneRequest:       return L"Tune Request";
        case Category::Clock:             return L"Clock";
        case Category::Start:             return L"Start";
        case Category::Continue:          return L"Continue";
        case Category::Stop:              return L"Stop";
        case Category::ActiveSensing:     return L"Active Sensing";
        case Category::SystemReset:       return L"System Reset";
        default:                          return L"Unknown";
    }
}

const wchar_t* ccName(uint8_t cc) {
    return cc < 128 ? kCcNames[cc] : L"Invalid";
}

const wchar_t* gmProgramName(uint8_t program) {
    return program < 128 ? kGmNames[program] : L"Invalid";
}

std::wstring noteName(uint8_t note) {
    if (note > 127) return L"?";
    // Scientific pitch notation, middle C (note 60) written as C4.
    const int octave = static_cast<int>(note) / 12 - 1;
    return formatf(L"%s%d", kNoteLetters[note % 12], octave);
}

int pitchBendValue(uint8_t d1, uint8_t d2) {
    const int raw = (static_cast<int>(d2 & 0x7F) << 7) | (d1 & 0x7F);
    return raw - 8192;
}

std::wstring data1Text(uint8_t status, uint8_t d1) {
    if (isChannelMessage(status)) {
        switch (status & 0xF0) {
            case 0x80: case 0x90: case 0xA0:
                return formatf(L"%s (%u)", noteName(d1).c_str(), d1);
            case 0xB0:
                return formatf(L"CC %u \x2013 %s", d1, ccName(d1));
            case 0xC0:
                return formatf(L"%u \x2013 %s", d1, gmProgramName(d1));
            case 0xD0:
                return formatf(L"%u", d1);
            case 0xE0:
                return formatf(L"LSB %u", d1);
            default:
                break;
        }
    }
    return dataByteCount(status) >= 1 ? formatf(L"%u", d1) : std::wstring();
}

std::wstring data2Text(uint8_t status, uint8_t d1, uint8_t d2) {
    (void)d1;
    if (dataByteCount(status) < 2) return std::wstring();
    if (isChannelMessage(status) && (status & 0xF0) == 0xE0) {
        return formatf(L"MSB %u", d2);
    }
    return formatf(L"%u", d2);
}

std::wstring manufacturerName(const uint8_t* data, size_t len) {
    if (!data || len < 2 || data[0] != 0xF0) return std::wstring();

    if (data[1] == 0x00) {
        if (len < 4) return std::wstring();
        const uint32_t id = (static_cast<uint32_t>(data[2]) << 8) | data[3];
        for (const Manufacturer& m : kManufacturers3) {
            if (m.id == id) return m.name;
        }
        return formatf(L"ID 00 %02X %02X", data[2], data[3]);
    }

    for (const Manufacturer& m : kManufacturers1) {
        if (m.id == data[1]) return m.name;
    }
    return formatf(L"ID %02X", data[1]);
}

std::wstring hexBytes(const uint8_t* data, size_t len, size_t maxBytes) {
    std::wstring out;
    if (!data) return out;
    const size_t shown = len < maxBytes ? len : maxBytes;
    out.reserve(shown * 3 + 24);
    for (size_t i = 0; i < shown; ++i) {
        if (i) out.push_back(L' ');
        wchar_t b[4];
        _snwprintf_s(b, _countof(b), _TRUNCATE, L"%02X", data[i]);
        out.append(b);
    }
    if (len > shown) {
        out.append(formatf(L" ... (%zu bytes total)", len));
    }
    return out;
}

std::wstring describe(uint8_t status, uint8_t d1, uint8_t d2,
                      const uint8_t* sysex, size_t sysexLen) {
    if (isChannelMessage(status)) {
        switch (status & 0xF0) {
            case 0x90:
                if (d2 == 0) {
                    return formatf(L"%s released (note on, velocity 0)", noteName(d1).c_str());
                }
                return formatf(L"%s struck, velocity %u", noteName(d1).c_str(), d2);
            case 0x80:
                return formatf(L"%s released, velocity %u", noteName(d1).c_str(), d2);
            case 0xA0:
                return formatf(L"Pressure %u on %s", d2, noteName(d1).c_str());
            case 0xB0: {
                std::wstring text = formatf(L"%s = %u", ccName(d1), d2);
                if (d1 >= 64 && d1 <= 69) {
                    // Pedals and footswitches split at 64.
                    text += (d2 >= 64) ? L"  [on]" : L"  [off]";
                } else if (d1 >= 120) {
                    text += L"  [channel mode message]";
                }
                return text;
            }
            case 0xC0:
                return formatf(L"Patch %u \x2013 %s", d1, gmProgramName(d1));
            case 0xD0:
                return formatf(L"Channel pressure %u", d1);
            case 0xE0: {
                const int v = pitchBendValue(d1, d2);
                if (v == 0) return L"Centred (0)";
                const double pct = (v >= 0 ? v / 8191.0 : v / 8192.0) * 100.0;
                return formatf(L"%+d  (%.1f%% %s)", v, pct < 0 ? -pct : pct,
                               v > 0 ? L"up" : L"down");
            }
            default:
                break;
        }
    }

    switch (status) {
        case 0xF0: case 0xF7: {
            std::wstring maker = manufacturerName(sysex, sysexLen);
            if (maker.empty()) maker = L"unknown manufacturer";
            return formatf(L"%zu bytes from %s", sysexLen, maker.c_str());
        }
        case 0xF1:
            return formatf(L"MTC piece %d, value %d", (d1 >> 4) & 0x07, d1 & 0x0F);
        case 0xF2: {
            const int beats = (static_cast<int>(d2 & 0x7F) << 7) | (d1 & 0x7F);
            return formatf(L"%d MIDI beats (bar %d at 4/4)", beats, beats / 16 + 1);
        }
        case 0xF3: return formatf(L"Song %u", d1);
        case 0xF6: return L"Tune request";
        case 0xF8: return L"Timing clock (24 per quarter note)";
        case 0xFA: return L"Transport: start from the top";
        case 0xFB: return L"Transport: continue from current position";
        case 0xFC: return L"Transport: stop";
        case 0xFE: return L"Keep-alive ping";
        case 0xFF: return L"System reset";
        default:   return formatf(L"Unrecognised status byte 0x%02X", status);
    }
}

// ------------------------------------------------------------ ClockTracker ---

void ClockTracker::reset() {
    lastMs_ = -1.0;
    intervals_.clear();
    count_ = 0;
    bpm_   = 0.0;
}

double ClockTracker::push(double timeMs) {
    ++count_;
    if (lastMs_ >= 0.0) {
        const double gap = timeMs - lastMs_;
        // 0.2-500 ms per clock spans roughly 5-1250 BPM. Anything outside that
        // is a stopped or restarted transport rather than a tempo, so the
        // running average is discarded instead of being poisoned by it.
        if (gap >= 0.2 && gap <= 500.0) {
            intervals_.push_back(gap);
            while (intervals_.size() > window_) intervals_.pop_front();
            double sum = 0.0;
            for (double v : intervals_) sum += v;
            const double avg = sum / static_cast<double>(intervals_.size());
            bpm_ = (avg > 0.0) ? 60000.0 / (avg * 24.0) : 0.0;
        } else {
            intervals_.clear();
            bpm_ = 0.0;
        }
    }
    lastMs_ = timeMs;
    return bpm_;
}

} // namespace midi

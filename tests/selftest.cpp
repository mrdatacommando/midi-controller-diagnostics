// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

// selftest.cpp - Console harness that exercises everything except the window.
//
// It is deliberately passive: it opens MIDI inputs and listens, and the thru
// test wires up routes without ever switching forwarding on, so running it
// cannot make your hardware or a software synth produce a sound.

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/log_writer.h"
#include "../src/midi_decode.h"
#include "../src/midi_engine.h"

namespace {

int g_failures = 0;

void out(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    fwrite(s.data(), 1, s.size(), stdout);
}

void line(const std::wstring& w) { out(w); out(L"\n"); }

void check(bool ok, const std::wstring& what, const std::wstring& got) {
    if (ok) {
        line(L"  ok    " + what);
    } else {
        ++g_failures;
        line(L"  FAIL  " + what + L"   -> got: " + got);
    }
}

void expect(const std::wstring& actual, const std::wstring& wanted, const std::wstring& what) {
    check(actual == wanted, what + L" == \"" + wanted + L"\"", actual);
}

// A Source backed by plain vectors, standing in for the app.
class TestSource : public logio::Source {
public:
    std::vector<std::wstring>        names;
    std::vector<std::vector<uint8_t>> blobs;

    std::wstring deviceName(int slot) const override {
        if (slot == -2) return L"\x2192 sent";
        if (slot >= 0 && static_cast<size_t>(slot) < names.size()) return names[slot];
        return L"(unknown)";
    }
    const std::vector<uint8_t>* sysex(int64_t index) const override {
        if (index < 0 || static_cast<size_t>(index) >= blobs.size()) return nullptr;
        return &blobs[static_cast<size_t>(index)];
    }
    std::vector<std::wstring> activeDeviceNames() const override { return names; }
};

void testDecoder() {
    line(L"\n=== Decoder ===");

    expect(midi::noteName(60), L"C4", L"note 60");
    expect(midi::noteName(50), L"D3", L"note 50");
    expect(midi::noteName(0),  L"C-1", L"note 0");
    expect(midi::noteName(127), L"G9", L"note 127");

    check(midi::dataByteCount(0x90) == 2, L"Note On has 2 data bytes",
          std::to_wstring(midi::dataByteCount(0x90)));
    check(midi::dataByteCount(0xC0) == 1, L"Program Change has 1 data byte",
          std::to_wstring(midi::dataByteCount(0xC0)));
    check(midi::dataByteCount(0xF8) == 0, L"Clock has 0 data bytes",
          std::to_wstring(midi::dataByteCount(0xF8)));

    check(midi::pitchBendValue(0x00, 0x40) == 0, L"pitch bend centre is 0",
          std::to_wstring(midi::pitchBendValue(0x00, 0x40)));
    check(midi::pitchBendValue(0x00, 0x00) == -8192, L"pitch bend minimum",
          std::to_wstring(midi::pitchBendValue(0x00, 0x00)));
    check(midi::pitchBendValue(0x7F, 0x7F) == 8191, L"pitch bend maximum",
          std::to_wstring(midi::pitchBendValue(0x7F, 0x7F)));

    expect(midi::ccName(1), L"Modulation Wheel", L"CC 1 name");
    expect(midi::ccName(64), L"Sustain Pedal", L"CC 64 name");
    expect(midi::ccName(123), L"All Notes Off", L"CC 123 name");
    expect(midi::gmProgramName(48), L"String Ensemble 1", L"program 48 name");
    expect(midi::gmProgramName(0), L"Acoustic Grand Piano", L"program 0 name");

    check(midi::categorize(0x90, 60) == midi::Category::NoteOn, L"0x90 is Note On", L"");
    check(midi::categorize(0xB0, 1)  == midi::Category::ControlChange, L"0xB0 is CC", L"");
    check(midi::categorize(0xF8, 0)  == midi::Category::Clock, L"0xF8 is Clock", L"");
    check(midi::isChannelMessage(0x9F) && !midi::isChannelMessage(0xF0),
          L"channel-message range", L"");

    line(L"\n  --- how messages read in the window ---");
    struct Sample { uint8_t s, d1, d2; const wchar_t* note; };
    const Sample samples[] = {
        {0x90, 60, 100, L"note on"},
        {0x90, 60, 0,   L"note on, velocity 0"},
        {0x80, 60, 64,  L"note off"},
        {0xB0, 1,  64,  L"mod wheel"},
        {0xB0, 64, 127, L"sustain down"},
        {0xB0, 64, 0,   L"sustain up"},
        {0xB0, 123, 0,  L"all notes off"},
        {0xC0, 48, 0,   L"program change"},
        {0xD0, 85, 0,   L"channel aftertouch"},
        {0xA0, 60, 32,  L"poly aftertouch"},
        {0xE0, 0,  0x40, L"pitch bend centred"},
        {0xE0, 0,  0x60, L"pitch bend up"},
        {0xF8, 0,  0,   L"clock"},
        {0xFA, 0,  0,   L"start"},
        {0xFE, 0,  0,   L"active sensing"},
        {0xF2, 0x10, 0x02, L"song position"},
    };
    for (const Sample& s : samples) {
        const midi::Category c = midi::categorize(s.s, s.d1);
        wchar_t buf[512];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"  %02X %02X %02X  %-18.18s | %-26.26s | %s",
                     s.s, s.d1, s.d2,
                     midi::categoryName(c),
                     midi::data1Text(s.s, s.d1).c_str(),
                     midi::describe(s.s, s.d1, s.d2, nullptr, 0).c_str());
        line(buf);
    }

    line(L"\n  --- SysEx ---");
    const uint8_t yamaha[]   = {0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7};
    const uint8_t novation[] = {0xF0, 0x00, 0x20, 0x29, 0x02, 0x0D, 0x01, 0xF7};
    const uint8_t unknown[]  = {0xF0, 0x66, 0x01, 0x02, 0xF7};
    expect(midi::manufacturerName(yamaha, sizeof(yamaha)), L"Yamaha", L"1-byte id");
    expect(midi::manufacturerName(novation, sizeof(novation)),
           L"Focusrite / Novation", L"3-byte id");
    expect(midi::manufacturerName(unknown, sizeof(unknown)), L"ID 66", L"unlisted id");
    line(L"  " + midi::describe(0xF0, 0, 0, yamaha, sizeof(yamaha)));
    line(L"  " + midi::hexBytes(yamaha, sizeof(yamaha)));
}

void testClockTempo() {
    line(L"\n=== Tempo from timing clock ===");

    // 24 clocks per quarter note: at B BPM a clock arrives every
    // 60000 / (B * 24) ms.
    auto msPerClock = [](double bpm) { return 60000.0 / (bpm * 24.0); };

    for (double wanted : {60.0, 120.0, 128.0, 174.0}) {
        midi::ClockTracker t;
        double now = 0.0;
        for (int i = 0; i < 40; ++i) {
            t.push(now);
            now += msPerClock(wanted);
        }
        const double got = t.bpm();
        wchar_t buf[128];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"%.1f BPM stream reads %.2f", wanted, got);
        check(got > wanted - 0.05 && got < wanted + 0.05, buf, std::to_wstring(got));
    }

    // Convergence after a tempo change. The window holds ten gaps, and the
    // first push at the new tempo still measures the last gap of the old one,
    // so it takes eleven pushes to flush the window completely.
    {
        midi::ClockTracker t;
        double now = 0.0;
        for (int i = 0; i < 30; ++i) { t.push(now); now += msPerClock(100.0); }
        const double before = t.bpm();

        for (int i = 0; i < 10; ++i) { t.push(now); now += msPerClock(140.0); }
        const double partway = t.bpm();

        t.push(now);
        const double settled = t.bpm();

        wchar_t buf[200];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                     L"100 -> 140 BPM: %.1f, then %.1f after 10 clocks, %.1f after 11",
                     before, partway, settled);
        check(before > 99.9 && before < 100.1 &&
                  partway > 130.0 && partway < 140.0 &&
                  settled > 139.9 && settled < 140.1,
              buf, std::to_wstring(settled));
    }

    // Jitter should average out rather than make the readout jump about.
    {
        midi::ClockTracker t;
        double now = 0.0;
        const double step = msPerClock(120.0);
        for (int i = 0; i < 60; ++i) {
            t.push(now);
            now += step + ((i % 2) ? 0.4 : -0.4);   // +/- 0.4 ms of jitter
        }
        wchar_t buf[128];
        _snwprintf_s(buf, _countof(buf), _TRUNCATE, L"jittery 120 BPM stays near 120 (%.2f)",
                     t.bpm());
        check(t.bpm() > 119.0 && t.bpm() < 121.0, buf, std::to_wstring(t.bpm()));
    }

    // A stopped transport must not be averaged in as though it were a tempo.
    {
        midi::ClockTracker t;
        double now = 0.0;
        for (int i = 0; i < 20; ++i) { t.push(now); now += msPerClock(120.0); }
        t.push(now + 4000.0);   // four seconds of silence, then it resumes
        check(t.bpm() == 0.0, L"a long gap clears the estimate rather than skewing it",
              std::to_wstring(t.bpm()));
    }

    check(midi::ClockTracker::kDefaultWindow == 10, L"averaging window is 10 clocks",
          std::to_wstring(midi::ClockTracker::kDefaultWindow));
}

void testLogWriter() {
    line(L"\n=== Log writer ===");

    TestSource src;
    src.names = {L"Akai Network - MIDI", L"Some, Device \"quoted\""};
    src.blobs.push_back({0xF0, 0x43, 0x10, 0x4C, 0xF7});

    FILETIME start;
    GetSystemTimeAsFileTime(&start);

    std::vector<midi::Event> events;
    auto add = [&](int slot, double ms, uint8_t s, uint8_t d1, uint8_t d2, int64_t sx = -1) {
        midi::Event e;
        e.seq        = events.size();
        e.timeMs     = ms;
        e.deviceSlot = slot;
        e.status     = s;
        e.d1         = d1;
        e.d2         = d2;
        e.sysexIndex = sx;
        events.push_back(e);
    };
    add(0, 0.0,     0x90, 60, 100);
    add(0, 123.456, 0x80, 60, 0);
    add(1, 200.0,   0xB0, 1,  64);
    add(0, 250.0,   0xE0, 0,  0x60);
    add(0, 300.0,   0xF0, 0,  0, 0);
    {   // a timing clock carrying a tempo estimate
        midi::Event c;
        c.seq = events.size(); c.timeMs = 320.0; c.deviceSlot = 0;
        c.status = 0xF8; c.bpm = 128.0f;
        events.push_back(c);
    }
    add(-2, 350.0,  0x90, 64, 100);   // an echoed "sent" event

    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);

    struct Case { logio::Format fmt; const wchar_t* ext; };
    const Case cases[] = {
        {logio::Format::Csv,   L"csv"},
        {logio::Format::Jsonl, L"jsonl"},
        {logio::Format::Text,  L"txt"},
    };

    for (const Case& c : cases) {
        std::wstring path = std::wstring(dir) + L"midiscope-selftest." + c.ext;
        std::wstring error;
        const bool ok = logio::writeLog(path, c.fmt, events, src, start, error);
        check(ok, std::wstring(L"wrote .") + c.ext, error);
        if (!ok) continue;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { check(false, L"reopened file", L""); continue; }
        char buf[8192] = {};
        DWORD read = 0;
        ReadFile(h, buf, sizeof(buf) - 1, &read, nullptr);
        CloseHandle(h);
        check(read > 0, std::wstring(L".") + c.ext + L" is not empty",
              std::to_wstring(read) + L" bytes");

        line(L"\n  --- " + std::wstring(c.ext) + L" (" + std::to_wstring(read) + L" bytes) ---");
        fwrite(buf, 1, read < 1400 ? read : 1400, stdout);
        if (read >= 1400) out(L"\n  ...(truncated)\n");
    }

    // Streaming should produce the same shape, written incrementally.
    std::wstring streamPath = std::wstring(dir) + L"midiscope-selftest-stream.csv";
    logio::StreamLogger stream;
    std::wstring error;
    check(stream.open(streamPath, logio::Format::Csv, error), L"opened stream log", error);
    stream.append(events, src, start);
    stream.append(events, src, start);
    stream.close();

    HANDLE h = CreateFileW(streamPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    LARGE_INTEGER size = {};
    if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &size); CloseHandle(h); }
    check(size.QuadPart > 0, L"stream log has content",
          std::to_wstring(size.QuadPart) + L" bytes");
}

void testThruRouting() {
    line(L"\n=== MIDI thru ===");

    const auto inputs  = midi::Engine::enumerateInputs();
    const auto outputs = midi::Engine::enumerateOutputs();
    if (inputs.empty() || outputs.empty()) {
        line(L"  (needs at least one input and one output - skipped)");
        return;
    }

    // Forwarding is never switched on here. Opening the route handles is the
    // part worth testing; leaving thru disabled means nothing can be passed
    // through to your gear even if you happen to touch it mid-test.
    line(L"  routing " + inputs[0].name + L"  ->  " + outputs[0].name +
         L"  (forwarding stays off)");

    midi::Engine engine;
    std::vector<std::wstring> errors;

    std::vector<midi::ThruRoute> routes;
    routes.push_back({inputs[0].id, outputs[0].id});
    engine.setThruRoutes(routes, errors);
    check(engine.thruActiveCount() == 0,
          L"a route set while stopped stays dormant",
          std::to_wstring(engine.thruActiveCount()));
    check(!engine.thruEnabled(), L"thru starts switched off", L"");

    const std::vector<UINT> ids{inputs[0].id};

    // Several cycles, to shake out handle leaks in the open/close paths.
    for (int cycle = 1; cycle <= 3; ++cycle) {
        errors.clear();
        if (!engine.start(ids, errors)) {
            line(L"  (could not open " + inputs[0].name + L" - skipping the rest)");
            for (const auto& e : errors) line(L"      note: " + e);
            return;
        }
        check(engine.thruActiveCount() == 1,
              L"cycle " + std::to_wstring(cycle) + L": route connects on start",
              std::to_wstring(engine.thruActiveCount()));

        // A second output on the same input should fan out, not replace.
        if (outputs.size() > 1) {
            std::vector<midi::ThruRoute> two = routes;
            two.push_back({inputs[0].id, outputs[1].id});
            errors.clear();
            engine.setThruRoutes(two, errors);
            check(engine.thruActiveCount() == 2,
                  L"cycle " + std::to_wstring(cycle) + L": one input fans out to two outputs",
                  std::to_wstring(engine.thruActiveCount()));
            for (const auto& e : errors) line(L"      note: " + e);

            errors.clear();
            engine.setThruRoutes(routes, errors);
            check(engine.thruActiveCount() == 1,
                  L"cycle " + std::to_wstring(cycle) + L": removing a route drops its output",
                  std::to_wstring(engine.thruActiveCount()));
        }

        engine.stop();
        check(engine.thruActiveCount() == 0,
              L"cycle " + std::to_wstring(cycle) + L": routes torn down on stop",
              std::to_wstring(engine.thruActiveCount()));
    }

    // A route naming a device that is not open must simply be ignored.
    errors.clear();
    std::vector<midi::ThruRoute> bogus;
    bogus.push_back({9999, outputs[0].id});
    engine.setThruRoutes(bogus, errors);
    if (engine.start(ids, errors)) {
        check(engine.thruActiveCount() == 0,
              L"a route for a device that is not open is ignored",
              std::to_wstring(engine.thruActiveCount()));
        engine.stop();
    }
}

void testEngineLifecycle() {
    line(L"\n=== Engine ===");

    const auto inputs  = midi::Engine::enumerateInputs();
    const auto outputs = midi::Engine::enumerateOutputs();

    line(L"  MIDI inputs (" + std::to_wstring(inputs.size()) + L"):");
    for (const auto& d : inputs) {
        line(L"    [" + std::to_wstring(d.id) + L"] " + d.name);
    }
    line(L"  MIDI outputs (" + std::to_wstring(outputs.size()) + L"):");
    for (const auto& d : outputs) {
        line(L"    [" + std::to_wstring(d.id) + L"] " + d.name);
    }

    if (inputs.empty()) {
        line(L"  (no inputs to open - skipping the capture test)");
        return;
    }

    // Open every input, listen briefly, shut down. This is the part most
    // likely to hang or leak, so run it twice to prove restart works.
    for (int pass = 1; pass <= 2; ++pass) {
        std::vector<UINT> ids;
        for (const auto& d : inputs) ids.push_back(d.id);

        midi::Engine engine;
        std::vector<std::wstring> errors;
        const DWORD t0 = GetTickCount();
        const bool ok = engine.start(ids, errors);
        line(L"  pass " + std::to_wstring(pass) + L": start -> " +
             (ok ? L"opened " + std::to_wstring(engine.openDeviceNames().size()) +
                       L" device(s)"
                 : L"opened nothing"));
        for (const auto& e : errors) line(L"      note: " + e);

        if (ok) {
            std::vector<midi::Event> drained;
            size_t total = 0;
            for (int i = 0; i < 20; ++i) {
                Sleep(50);
                engine.drain(drained);
                total += drained.size();
                for (const midi::Event& e : drained) {
                    wchar_t buf[256];
                    _snwprintf_s(buf, _countof(buf), _TRUNCATE,
                                 L"      %8.1f ms  %-20.20s  %s",
                                 e.timeMs,
                                 midi::categoryName(midi::categorize(e.status, e.d1)),
                                 midi::describe(e.status, e.d1, e.d2, nullptr, 0).c_str());
                    line(buf);
                }
            }
            line(L"      listened 1s, received " + std::to_wstring(total) +
                 L" message(s), dropped " + std::to_wstring(engine.droppedCount()));
        }

        engine.stop();
        const DWORD elapsed = GetTickCount() - t0;
        check(elapsed < 5000, L"pass " + std::to_wstring(pass) + L" start/stop completed",
              std::to_wstring(elapsed) + L" ms");
    }
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    line(L"MidiScope self-test");
    line(L"===================");

    testDecoder();
    testClockTempo();
    testLogWriter();
    testThruRouting();
    testEngineLifecycle();

    line(L"");
    if (g_failures == 0) {
        line(L"All checks passed.");
        return 0;
    }
    line(std::to_wstring(g_failures) + L" check(s) FAILED.");
    return 1;
}

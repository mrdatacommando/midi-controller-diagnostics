// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

// log_writer.h - Turning captured events into files on disk.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "midi_engine.h"

namespace logio {

enum class Format { Csv, Jsonl, Text };

// Supplies the context an event needs to be written out. The owner of the
// capture implements this, so device names and SysEx payloads stay correct
// even after capture has been stopped and restarted with a different device
// selection.
class Source {
public:
    virtual ~Source() = default;
    virtual std::wstring deviceName(int slot) const = 0;
    virtual const std::vector<uint8_t>* sysex(int64_t index) const = 0;
    virtual std::vector<std::wstring> activeDeviceNames() const = 0;
};

// Wall-clock rendering of an event. `withDate` switches between
// "2026-09-22 15:30:12.345" and the "15:30:12.345" the live view uses.
std::wstring wallClock(FILETIME captureStart, double offsetMs, bool withDate);

// Writes every event in one go. Returns false and fills `error` on failure.
bool writeLog(const std::wstring&             path,
              Format                          format,
              const std::vector<midi::Event>& events,
              const Source&                   source,
              FILETIME                        captureStart,
              std::wstring&                   error);

// Appends events to an open file as they arrive, so a long capture survives a
// crash or a power cut.
class StreamLogger {
public:
    ~StreamLogger();

    bool open(const std::wstring& path, Format format, std::wstring& error);
    void close();
    bool isOpen() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }

    const std::wstring& path() const { return path_; }

    void append(const std::vector<midi::Event>& events,
                const Source&                   source,
                FILETIME                        captureStart);

private:
    HANDLE       handle_ = nullptr;
    Format       format_ = Format::Csv;
    std::wstring path_;
    double       lastTimeMs_ = -1.0;
};

} // namespace logio

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

#include "log_writer.h"

#include <cstdio>

#include "midi_decode.h"

namespace logio {
namespace {

std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring csvEscape(const std::wstring& in) {
    bool needsQuotes = false;
    for (wchar_t c : in) {
        if (c == L',' || c == L'"' || c == L'\n' || c == L'\r') { needsQuotes = true; break; }
    }
    if (!needsQuotes) return in;

    std::wstring out;
    out.reserve(in.size() + 8);
    out.push_back(L'"');
    for (wchar_t c : in) {
        if (c == L'"') out.push_back(L'"');
        out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

std::wstring jsonEscape(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size() + 8);
    for (wchar_t c : in) {
        switch (c) {
            case L'"':  out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n";  break;
            case L'\r': out += L"\\r";  break;
            case L'\t': out += L"\\t";  break;
            default:
                if (c < 0x20) {
                    wchar_t esc[8];
                    _snwprintf_s(esc, _countof(esc), _TRUNCATE, L"\\u%04X", c);
                    out += esc;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::wstring formatf(const wchar_t* fmt, ...) {
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    const int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(buf, n > 0 ? static_cast<size_t>(n) : 0);
}

int channelOf(uint8_t status) {
    return midi::isChannelMessage(status) ? (status & 0x0F) + 1 : 0;
}

std::wstring deviceName(const Source& source, int slot) {
    return source.deviceName(slot);
}

std::wstring rawHex(const midi::Event& e, const Source& source, size_t maxBytes) {
    if (e.sysexIndex >= 0) {
        const std::vector<uint8_t>* blob = source.sysex(e.sysexIndex);
        if (!blob) return L"(SysEx data expired)";
        return midi::hexBytes(blob->data(), blob->size(), maxBytes);
    }
    uint8_t bytes[3] = {e.status, e.d1, e.d2};
    const int n = 1 + midi::dataByteCount(e.status);
    return midi::hexBytes(bytes, static_cast<size_t>(n), maxBytes);
}

void writeAll(HANDLE h, const std::string& s) {
    if (s.empty()) return;
    DWORD written = 0;
    WriteFile(h, s.data(), static_cast<DWORD>(s.size()), &written, nullptr);
}

std::string headerFor(Format format) {
    switch (format) {
        case Format::Csv:
            return "seq,timestamp,offset_ms,delta_ms,device,channel,type,"
                   "status_hex,data1,data2,data1_text,data2_text,description,raw,bpm\r\n";
        case Format::Text:
            return "";
        case Format::Jsonl:
        default:
            return "";
    }
}

std::string lineFor(Format                  format,
                    const midi::Event&      e,
                    const Source&           source,
                    FILETIME                captureStart,
                    double                  deltaMs) {
    const midi::Category cat = midi::categorize(e.status, e.d1);
    const int            ch  = channelOf(e.status);

    const std::wstring stamp = wallClock(captureStart, e.timeMs, true);
    const std::wstring dev   = deviceName(source, e.deviceSlot);
    const std::wstring type  = midi::categoryName(cat);
    const std::wstring d1t   = midi::data1Text(e.status, e.d1);
    const std::wstring d2t   = midi::data2Text(e.status, e.d1, e.d2);

    const std::vector<uint8_t>* blob =
        (e.sysexIndex >= 0) ? source.sysex(e.sysexIndex) : nullptr;
    // Timing clock carries a tempo estimate the pure decoder cannot know.
    const std::wstring desc =
        (e.status == 0xF8 && e.bpm > 0.0f)
            ? formatf(L"Timing clock \x2013 %.1f BPM from %s", e.bpm, dev.c_str())
            : midi::describe(e.status, e.d1, e.d2,
                             blob ? blob->data() : nullptr,
                             blob ? blob->size() : 0);
    const std::wstring bpmText =
        (e.status == 0xF8 && e.bpm > 0.0f) ? formatf(L"%.2f", e.bpm) : std::wstring();

    switch (format) {
        case Format::Csv: {
            // SysEx dumps can be long; keep the whole thing in the file.
            const std::wstring raw = rawHex(e, source, 4096);
            std::wstring line = formatf(
                L"%llu,%s,%.3f,%.3f,%s,%s,%s,%02X,%u,%u,%s,%s,%s,%s,%s\r\n",
                static_cast<unsigned long long>(e.seq),
                stamp.c_str(),
                e.timeMs,
                deltaMs,
                csvEscape(dev).c_str(),
                ch ? std::to_wstring(ch).c_str() : L"",
                csvEscape(type).c_str(),
                e.status,
                e.d1,
                e.d2,
                csvEscape(d1t).c_str(),
                csvEscape(d2t).c_str(),
                csvEscape(desc).c_str(),
                csvEscape(raw).c_str(),
                bpmText.c_str());
            return toUtf8(line);
        }
        case Format::Jsonl: {
            const std::wstring raw = rawHex(e, source, 4096);
            std::wstring line = formatf(
                L"{\"seq\":%llu,\"timestamp\":\"%s\",\"offset_ms\":%.3f,"
                L"\"delta_ms\":%.3f,\"device\":\"%s\",\"channel\":%s,"
                L"\"type\":\"%s\",\"status\":%u,\"data1\":%u,\"data2\":%u,"
                L"\"data1_text\":\"%s\",\"data2_text\":\"%s\","
                L"\"description\":\"%s\",\"raw\":\"%s\",\"bpm\":%s}\n",
                static_cast<unsigned long long>(e.seq),
                jsonEscape(stamp).c_str(),
                e.timeMs,
                deltaMs,
                jsonEscape(dev).c_str(),
                ch ? std::to_wstring(ch).c_str() : L"null",
                jsonEscape(type).c_str(),
                e.status,
                e.d1,
                e.d2,
                jsonEscape(d1t).c_str(),
                jsonEscape(d2t).c_str(),
                jsonEscape(desc).c_str(),
                jsonEscape(raw).c_str(),
                bpmText.empty() ? L"null" : bpmText.c_str());
            return toUtf8(line);
        }
        case Format::Text:
        default: {
            const std::wstring raw = rawHex(e, source, 24);
            std::wstring chText = ch ? formatf(L"ch%-2d", ch) : std::wstring(L"  \x2013 ");
            std::wstring line = formatf(
                L"%s  %+9.3f  %-24.24s  %s  %-16.16s  %-28.28s  %s\r\n",
                stamp.c_str(),
                deltaMs,
                dev.c_str(),
                chText.c_str(),
                type.c_str(),
                d1t.c_str(),
                desc.c_str());
            // Keep the raw bytes on the line so the file is self-contained.
            line.pop_back();
            line.pop_back();
            line += formatf(L"   [%s]\r\n", raw.c_str());
            return toUtf8(line);
        }
    }
}

} // namespace

std::wstring wallClock(FILETIME captureStart, double offsetMs, bool withDate) {
    ULARGE_INTEGER start;
    start.LowPart  = captureStart.dwLowDateTime;
    start.HighPart = captureStart.dwHighDateTime;

    // FILETIME counts 100-nanosecond intervals.
    const long long ticks = static_cast<long long>(offsetMs * 10000.0 + 0.5);
    ULARGE_INTEGER at;
    at.QuadPart = start.QuadPart + static_cast<unsigned long long>(ticks);

    FILETIME ft;
    ft.dwLowDateTime  = at.LowPart;
    ft.dwHighDateTime = at.HighPart;

    SYSTEMTIME utc = {};
    SYSTEMTIME local = {};
    if (!FileTimeToSystemTime(&ft, &utc) ||
        !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) {
        return formatf(L"+%.3f ms", offsetMs);
    }

    if (withDate) {
        return formatf(L"%04d-%02d-%02d %02d:%02d:%02d.%03d",
                       local.wYear, local.wMonth, local.wDay,
                       local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
    }
    return formatf(L"%02d:%02d:%02d.%03d",
                   local.wHour, local.wMinute, local.wSecond, local.wMilliseconds);
}

bool writeLog(const std::wstring&             path,
              Format                          format,
              const std::vector<midi::Event>& events,
              const Source&                   source,
              FILETIME                        captureStart,
              std::wstring&                   error) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = L"Could not create the file. Check the folder is writable.";
        return false;
    }

    // A BOM keeps Excel and Notepad from guessing the encoding wrongly.
    if (format == Format::Csv || format == Format::Text) {
        writeAll(h, "\xEF\xBB\xBF");
    }

    if (format == Format::Text) {
        std::wstring banner = formatf(
            L"MidiScope capture log\r\n"
            L"Capture started : %s\r\n"
            L"Events          : %zu\r\n"
            L"Devices         : ",
            wallClock(captureStart, 0.0, true).c_str(), events.size());
        const auto& names = source.activeDeviceNames();
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) banner += L", ";
            banner += names[i];
        }
        if (names.empty()) banner += L"(none)";
        banner += L"\r\n";
        banner += std::wstring(120, L'-') + L"\r\n";
        writeAll(h, toUtf8(banner));
    }

    writeAll(h, headerFor(format));

    double last = -1.0;
    std::string chunk;
    chunk.reserve(1 << 16);

    for (const midi::Event& e : events) {
        const double delta = (last < 0.0) ? 0.0 : (e.timeMs - last);
        last = e.timeMs;
        chunk += lineFor(format, e, source, captureStart, delta);
        if (chunk.size() >= (1u << 16)) {
            writeAll(h, chunk);
            chunk.clear();
        }
    }
    writeAll(h, chunk);

    CloseHandle(h);
    return true;
}

// ---------------------------------------------------------- StreamLogger ---

StreamLogger::~StreamLogger() { close(); }

bool StreamLogger::open(const std::wstring& path, Format format, std::wstring& error) {
    close();
    handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle_ == INVALID_HANDLE_VALUE) {
        handle_ = nullptr;
        error   = L"Could not create the log file. Check the folder is writable.";
        return false;
    }
    format_     = format;
    path_       = path;
    lastTimeMs_ = -1.0;

    if (format_ == Format::Csv || format_ == Format::Text) {
        writeAll(handle_, "\xEF\xBB\xBF");
    }
    writeAll(handle_, headerFor(format_));
    return true;
}

void StreamLogger::close() {
    if (isOpen()) {
        FlushFileBuffers(handle_);
        CloseHandle(handle_);
    }
    handle_ = nullptr;
    path_.clear();
}

void StreamLogger::append(const std::vector<midi::Event>& events,
                          const Source&                   source,
                          FILETIME                        captureStart) {
    if (!isOpen() || events.empty()) return;

    std::string chunk;
    chunk.reserve(events.size() * 128);
    for (const midi::Event& e : events) {
        const double delta = (lastTimeMs_ < 0.0) ? 0.0 : (e.timeMs - lastTimeMs_);
        lastTimeMs_ = e.timeMs;
        chunk += lineFor(format_, e, source, captureStart, delta);
    }
    writeAll(handle_, chunk);
}

} // namespace logio

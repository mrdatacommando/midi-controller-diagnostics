// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

// midi_engine.h - Device enumeration and real-time capture built on winmm.
//
// Capture runs on winmm's own callback threads. Those callbacks are only
// allowed to touch a short list of functions, so they do the minimum possible:
// copy the message into a fixed ring buffer and signal a helper thread when a
// SysEx buffer needs recycling. The UI drains the ring on a timer.
#pragma once

#include <windows.h>
#include <mmsystem.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace midi {

struct DeviceInfo {
    UINT         id = 0;
    std::wstring name;
};

// One input forwarded to one output. Routes are held by device id rather than
// by slot, so they survive capture being stopped and started again.
struct ThruRoute {
    UINT inputId  = 0;
    UINT outputId = 0;
};

struct Event {
    uint64_t seq        = 0;
    double   timeMs     = 0.0;  // milliseconds since capture started
    int      deviceSlot = 0;    // index into Engine::openDeviceNames()
    uint8_t  status     = 0;
    uint8_t  d1         = 0;
    uint8_t  d2         = 0;
    int64_t  sysexIndex = -1;   // index for Engine::sysexAt(), or -1
    float    bpm        = 0.0f; // tempo at this instant; timing clock only
};

class Engine {
public:
    Engine();
    ~Engine();

    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    static std::vector<DeviceInfo> enumerateInputs();
    static std::vector<DeviceInfo> enumerateOutputs();

    // Opens every id in `inputIds`. Devices that fail are skipped and described
    // in `errors`; returns true when at least one device opened.
    bool start(const std::vector<UINT>& inputIds, std::vector<std::wstring>& errors);
    void stop();
    bool running() const { return running_; }

    const std::vector<std::wstring>& openDeviceNames() const { return openNames_; }

    // Moves everything captured since the previous call into `out`.
    void drain(std::vector<Event>& out);

    uint64_t droppedCount() const;
    uint64_t totalCount() const { return nextSeq_; }

    // Wall-clock instant that timeMs == 0 corresponds to.
    FILETIME captureStartTime() const { return captureStartWall_; }

    // Returns nullptr once a blob has aged out of the bounded store.
    const std::vector<uint8_t>* sysexAt(int64_t index) const;

    bool openOutput(UINT id);
    void closeOutput();
    bool outputOpen() const { return hOut_ != nullptr; }
    bool sendShort(uint8_t status, uint8_t d1, uint8_t d2);
    bool sendPanic();

    // --- MIDI thru ---
    // Short messages are forwarded straight from the input callback, which is
    // the only way to keep playing latency down; SysEx goes via the helper
    // thread because it needs a prepared buffer. Routes can be set at any
    // time, but only take effect while capture is running.
    void   setThruRoutes(const std::vector<ThruRoute>& routes,
                         std::vector<std::wstring>&    errors);
    void   setThruEnabled(bool on);
    bool   thruEnabled() const { return thruEnabled_ != 0; }
    size_t thruActiveCount() const;

    static std::wstring errorText(MMRESULT r);

private:
    static constexpr size_t kSysexBuffersPerDevice = 4;
    static constexpr size_t kSysexBufferBytes      = 16 * 1024;
    static constexpr size_t kRingCapacity          = 1u << 16;   // 65536 events
    static constexpr size_t kStagingSlots          = 256;
    static constexpr size_t kMaxStoredSysex        = 8192;

    struct RawEvent {
        DWORD    tsMs      = 0;   // ms since this device's midiInStart
        int      slot      = 0;
        uint8_t  status    = 0;
        uint8_t  d1        = 0;
        uint8_t  d2        = 0;
        int32_t  sysexSlot = -1;
        uint32_t sysexLen  = 0;
    };

    struct CallbackCtx {
        Engine* engine = nullptr;
        int     slot   = 0;
    };

    // Held by unique_ptr so the MIDIHDR and CallbackCtx addresses handed to
    // winmm stay put even as the device list grows.
    struct OpenDevice {
        HMIDIIN     handle = nullptr;
        CallbackCtx ctx;
        MIDIHDR     headers[kSysexBuffersPerDevice] = {};
        std::vector<char> buffers[kSysexBuffersPerDevice];
        double      baseMs = 0.0;
        bool        prepared[kSysexBuffersPerDevice] = {};
    };

    struct ThruOut {
        UINT     id     = 0;
        HMIDIOUT handle = nullptr;
    };

    // A SysEx dump waiting to be forwarded, named by its staging slot so the
    // callback never has to allocate.
    struct SysexJob {
        int      stageSlot = -1;
        uint32_t len       = 0;
        int      inputSlot = 0;
    };

    static void CALLBACK inputProc(HMIDIIN h, UINT msg, DWORD_PTR inst,
                                   DWORD_PTR p1, DWORD_PTR p2);
    static DWORD WINAPI  requeueThreadProc(LPVOID param);

    void handleShort(int slot, DWORD msg, DWORD tsMs);
    void handleLong(int slot, MIDIHDR* hdr, DWORD tsMs);
    void pushRaw(const RawEvent& e);
    void runRequeueLoop();
    void closeAllInputs();

    void rebuildThru(std::vector<std::wstring>& errors);
    void closeThruOutputs();
    int  slotForInput(UINT inputId) const;
    static void sendSysexBlocking(HMIDIOUT h, const uint8_t* data, size_t len);

    mutable CRITICAL_SECTION lock_{};
    std::vector<RawEvent>    ring_;
    size_t                   ringHead_  = 0;   // next write position
    size_t                   ringCount_ = 0;
    uint64_t                 dropped_   = 0;
    uint64_t                 nextSeq_   = 0;

    // Staging area the callbacks copy SysEx into, recycled round-robin.
    std::vector<uint8_t> staging_;
    size_t               stagingNext_ = 0;

    // Bounded persistent store the UI reads from.
    std::deque<std::vector<uint8_t>> sysexStore_;
    int64_t                          sysexBase_ = 0;

    std::vector<std::unique_ptr<OpenDevice>> devices_;
    std::vector<std::wstring>                openNames_;
    std::vector<UINT>                        openIds_;
    bool                                     running_ = false;
    volatile LONG                            closing_ = 0;

    // Thru. thruBySlot_ maps an input slot to indices into thruOuts_, and both
    // are only ever read by callbacks while the lock is held.
    std::vector<ThruRoute>        thruRoutes_;
    std::vector<ThruOut>          thruOuts_;
    std::vector<std::vector<int>> thruBySlot_;
    std::vector<SysexJob>         sysexForward_;
    volatile LONG                 thruEnabled_ = 0;

    // Requeue plumbing: callbacks may not call midiInAddBuffer themselves.
    HANDLE                requeueEvent_  = nullptr;
    HANDLE                requeueThread_ = nullptr;
    volatile LONG         requeueQuit_   = 0;
    std::vector<MIDIHDR*> requeueQueue_;
    std::vector<int>      requeueSlots_;

    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER captureStartQpc_{};
    FILETIME      captureStartWall_{};

    HMIDIOUT hOut_ = nullptr;
};

} // namespace midi

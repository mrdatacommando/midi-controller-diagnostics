// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

#include "midi_engine.h"

#include <algorithm>

namespace midi {

Engine::Engine() {
    InitializeCriticalSection(&lock_);
    ring_.resize(kRingCapacity);
    staging_.resize(kStagingSlots * kSysexBufferBytes);
    QueryPerformanceFrequency(&qpcFreq_);
}

Engine::~Engine() {
    stop();
    closeOutput();
    DeleteCriticalSection(&lock_);
}

std::wstring Engine::errorText(MMRESULT r) {
    wchar_t buf[MAXERRORLENGTH] = {};
    if (midiInGetErrorTextW(r, buf, MAXERRORLENGTH) == MMSYSERR_NOERROR && buf[0]) {
        return buf;
    }
    wchar_t fallback[64];
    _snwprintf_s(fallback, _countof(fallback), _TRUNCATE, L"MMSYSTEM error %u", r);
    return fallback;
}

std::vector<DeviceInfo> Engine::enumerateInputs() {
    std::vector<DeviceInfo> out;
    const UINT n = midiInGetNumDevs();
    out.reserve(n);
    for (UINT i = 0; i < n; ++i) {
        MIDIINCAPSW caps = {};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            DeviceInfo d;
            d.id   = i;
            d.name = caps.szPname;
            if (d.name.empty()) d.name = L"(unnamed device)";
            out.push_back(std::move(d));
        }
    }
    return out;
}

std::vector<DeviceInfo> Engine::enumerateOutputs() {
    std::vector<DeviceInfo> out;
    const UINT n = midiOutGetNumDevs();
    out.reserve(n);
    for (UINT i = 0; i < n; ++i) {
        MIDIOUTCAPSW caps = {};
        if (midiOutGetDevCapsW(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR) {
            DeviceInfo d;
            d.id   = i;
            d.name = caps.szPname;
            if (d.name.empty()) d.name = L"(unnamed device)";
            out.push_back(std::move(d));
        }
    }
    return out;
}

// ---------------------------------------------------------------- capture ---

bool Engine::start(const std::vector<UINT>& inputIds, std::vector<std::wstring>& errors) {
    if (running_) stop();

    EnterCriticalSection(&lock_);
    ringHead_    = 0;
    ringCount_   = 0;
    dropped_     = 0;
    nextSeq_     = 0;
    stagingNext_ = 0;
    sysexStore_.clear();
    sysexBase_ = 0;
    LeaveCriticalSection(&lock_);

    InterlockedExchange(&closing_, 0);
    InterlockedExchange(&requeueQuit_, 0);

    requeueEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!requeueEvent_) {
        errors.push_back(L"Could not create the SysEx recycling event.");
        return false;
    }
    requeueThread_ = CreateThread(nullptr, 0, &Engine::requeueThreadProc, this, 0, nullptr);
    if (!requeueThread_) {
        CloseHandle(requeueEvent_);
        requeueEvent_ = nullptr;
        errors.push_back(L"Could not start the SysEx recycling thread.");
        return false;
    }

    GetSystemTimeAsFileTime(&captureStartWall_);
    QueryPerformanceCounter(&captureStartQpc_);

    const auto available = enumerateInputs();
    auto nameForId = [&available](UINT id) -> std::wstring {
        for (const auto& d : available) {
            if (d.id == id) return d.name;
        }
        return L"(device " + std::to_wstring(id) + L")";
    };

    devices_.clear();
    openNames_.clear();
    openIds_.clear();

    for (UINT id : inputIds) {
        auto dev = std::make_unique<OpenDevice>();
        dev->ctx.engine = this;
        dev->ctx.slot   = static_cast<int>(devices_.size());

        MMRESULT r = midiInOpen(&dev->handle, id,
                                reinterpret_cast<DWORD_PTR>(&Engine::inputProc),
                                reinterpret_cast<DWORD_PTR>(&dev->ctx),
                                CALLBACK_FUNCTION);
        if (r != MMSYSERR_NOERROR) {
            errors.push_back(nameForId(id) + L": " + errorText(r) +
                             L" (another application may already have it open)");
            continue;
        }

        for (size_t i = 0; i < kSysexBuffersPerDevice; ++i) {
            dev->buffers[i].assign(kSysexBufferBytes, 0);
            MIDIHDR& h       = dev->headers[i];
            ZeroMemory(&h, sizeof(h));
            h.lpData         = dev->buffers[i].data();
            h.dwBufferLength = static_cast<DWORD>(kSysexBufferBytes);
            if (midiInPrepareHeader(dev->handle, &h, sizeof(h)) == MMSYSERR_NOERROR) {
                dev->prepared[i] = true;
                midiInAddBuffer(dev->handle, &h, sizeof(h));
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        dev->baseMs = static_cast<double>(now.QuadPart - captureStartQpc_.QuadPart) * 1000.0 /
                      static_cast<double>(qpcFreq_.QuadPart);

        r = midiInStart(dev->handle);
        if (r != MMSYSERR_NOERROR) {
            errors.push_back(nameForId(id) + L": " + errorText(r));
            midiInClose(dev->handle);
            continue;
        }

        openNames_.push_back(nameForId(id));
        openIds_.push_back(id);
        devices_.push_back(std::move(dev));
    }

    if (devices_.empty()) {
        InterlockedExchange(&requeueQuit_, 1);
        SetEvent(requeueEvent_);
        WaitForSingleObject(requeueThread_, 2000);
        CloseHandle(requeueThread_);
        CloseHandle(requeueEvent_);
        requeueThread_ = nullptr;
        requeueEvent_  = nullptr;
        return false;
    }

    running_ = true;

    // Any thru routes set while stopped come alive now that the inputs exist.
    rebuildThru(errors);

    return true;
}

void Engine::closeAllInputs() {
    for (auto& dev : devices_) {
        if (!dev->handle) continue;
        midiInStop(dev->handle);
        midiInReset(dev->handle);   // hands every queued buffer back

        for (size_t i = 0; i < kSysexBuffersPerDevice; ++i) {
            if (!dev->prepared[i]) continue;
            // midiInReset is asynchronous; give the buffers a moment to return.
            for (int attempt = 0; attempt < 50; ++attempt) {
                if (midiInUnprepareHeader(dev->handle, &dev->headers[i],
                                          sizeof(MIDIHDR)) != MIDIERR_STILLPLAYING) {
                    break;
                }
                Sleep(2);
            }
            dev->prepared[i] = false;
        }
        midiInClose(dev->handle);
        dev->handle = nullptr;
    }
    devices_.clear();
}

void Engine::stop() {
    if (!running_) {
        devices_.clear();
        return;
    }
    running_ = false;

    // Stops callbacks from staging new work before the buffers come back.
    InterlockedExchange(&closing_, 1);

    closeThruOutputs();
    closeAllInputs();
    openIds_.clear();

    if (requeueThread_) {
        InterlockedExchange(&requeueQuit_, 1);
        SetEvent(requeueEvent_);
        WaitForSingleObject(requeueThread_, 2000);
        CloseHandle(requeueThread_);
        requeueThread_ = nullptr;
    }
    if (requeueEvent_) {
        CloseHandle(requeueEvent_);
        requeueEvent_ = nullptr;
    }

    EnterCriticalSection(&lock_);
    requeueQueue_.clear();
    requeueSlots_.clear();
    LeaveCriticalSection(&lock_);
}

// --------------------------------------------------------------- callbacks ---

void CALLBACK Engine::inputProc(HMIDIIN, UINT msg, DWORD_PTR inst,
                                DWORD_PTR p1, DWORD_PTR p2) {
    auto* ctx = reinterpret_cast<CallbackCtx*>(inst);
    if (!ctx || !ctx->engine) return;

    switch (msg) {
        case MIM_DATA:
            ctx->engine->handleShort(ctx->slot, static_cast<DWORD>(p1),
                                     static_cast<DWORD>(p2));
            break;
        case MIM_LONGDATA:
            ctx->engine->handleLong(ctx->slot, reinterpret_cast<MIDIHDR*>(p1),
                                    static_cast<DWORD>(p2));
            break;
        case MIM_ERROR: {
            // Malformed bytes on the wire - surface as a dropped-message count.
            Engine* e = ctx->engine;
            EnterCriticalSection(&e->lock_);
            ++e->dropped_;
            LeaveCriticalSection(&e->lock_);
            break;
        }
        case MIM_LONGERROR:
            ctx->engine->handleLong(ctx->slot, reinterpret_cast<MIDIHDR*>(p1),
                                    static_cast<DWORD>(p2));
            break;
        default:
            break;
    }
}

void Engine::handleShort(int slot, DWORD msg, DWORD tsMs) {
    RawEvent e;
    e.tsMs   = tsMs;
    e.slot   = slot;
    e.status = static_cast<uint8_t>(msg & 0xFF);
    e.d1     = static_cast<uint8_t>((msg >> 8) & 0x7F);
    e.d2     = static_cast<uint8_t>((msg >> 16) & 0x7F);

    EnterCriticalSection(&lock_);

    // Thru first, so forwarding is not delayed by the bookkeeping below.
    // midiOutShortMsg is one of the few calls a MIDI input callback is allowed
    // to make, which is what makes real-time forwarding possible here at all.
    if (thruEnabled_ && slot >= 0 && static_cast<size_t>(slot) < thruBySlot_.size()) {
        for (int oi : thruBySlot_[static_cast<size_t>(slot)]) {
            if (oi >= 0 && static_cast<size_t>(oi) < thruOuts_.size() &&
                thruOuts_[static_cast<size_t>(oi)].handle) {
                midiOutShortMsg(thruOuts_[static_cast<size_t>(oi)].handle, msg);
            }
        }
    }

    if (ringCount_ == kRingCapacity) {
        ++dropped_;                       // overwrite the oldest entry
        ring_[ringHead_] = e;
        ringHead_ = (ringHead_ + 1) % kRingCapacity;
    } else {
        ring_[ringHead_] = e;
        ringHead_ = (ringHead_ + 1) % kRingCapacity;
        ++ringCount_;
    }

    LeaveCriticalSection(&lock_);
}

void Engine::handleLong(int slot, MIDIHDR* hdr, DWORD tsMs) {
    if (!hdr) return;

    const DWORD recorded = hdr->dwBytesRecorded;
    if (recorded > 0 && !closing_) {
        EnterCriticalSection(&lock_);

        const size_t stageSlot = stagingNext_;
        stagingNext_ = (stagingNext_ + 1) % kStagingSlots;

        const size_t copyLen = (std::min)(static_cast<size_t>(recorded), kSysexBufferBytes);
        memcpy(&staging_[stageSlot * kSysexBufferBytes], hdr->lpData, copyLen);

        RawEvent e;
        e.tsMs      = tsMs;
        e.slot      = slot;
        e.status    = 0xF0;
        e.sysexSlot = static_cast<int32_t>(stageSlot);
        e.sysexLen  = static_cast<uint32_t>(copyLen);

        // SysEx cannot go out from here - midiOutLongMsg needs a prepared
        // buffer - so hand it to the helper thread along with the requeue.
        if (thruEnabled_ && slot >= 0 &&
            static_cast<size_t>(slot) < thruBySlot_.size() &&
            !thruBySlot_[static_cast<size_t>(slot)].empty()) {
            SysexJob job;
            job.stageSlot = static_cast<int>(stageSlot);
            job.len       = static_cast<uint32_t>(copyLen);
            job.inputSlot = slot;
            sysexForward_.push_back(job);
        }

        // pushRaw takes the lock itself, so inline the store here instead.
        if (ringCount_ == kRingCapacity) {
            ++dropped_;
            ring_[ringHead_] = e;
            ringHead_ = (ringHead_ + 1) % kRingCapacity;
        } else {
            ring_[ringHead_] = e;
            ringHead_ = (ringHead_ + 1) % kRingCapacity;
            ++ringCount_;
        }

        LeaveCriticalSection(&lock_);
    }

    if (closing_) return;

    // Callbacks may not call midiInAddBuffer, so hand it to the helper thread.
    EnterCriticalSection(&lock_);
    requeueQueue_.push_back(hdr);
    requeueSlots_.push_back(slot);
    LeaveCriticalSection(&lock_);
    if (requeueEvent_) SetEvent(requeueEvent_);
}

void Engine::pushRaw(const RawEvent& e) {
    EnterCriticalSection(&lock_);
    if (ringCount_ == kRingCapacity) {
        ++dropped_;                       // overwrite the oldest entry
        ring_[ringHead_] = e;
        ringHead_ = (ringHead_ + 1) % kRingCapacity;
    } else {
        ring_[ringHead_] = e;
        ringHead_ = (ringHead_ + 1) % kRingCapacity;
        ++ringCount_;
    }
    LeaveCriticalSection(&lock_);
}

DWORD WINAPI Engine::requeueThreadProc(LPVOID param) {
    static_cast<Engine*>(param)->runRequeueLoop();
    return 0;
}

void Engine::runRequeueLoop() {
    std::vector<MIDIHDR*>  pending;
    std::vector<int>       slots;
    std::vector<SysexJob>  jobs;
    std::vector<uint8_t>   blob;

    while (true) {
        WaitForSingleObject(requeueEvent_, 250);
        if (requeueQuit_) return;

        pending.clear();
        slots.clear();
        jobs.clear();
        EnterCriticalSection(&lock_);
        pending.swap(requeueQueue_);
        slots.swap(requeueSlots_);
        jobs.swap(sysexForward_);
        LeaveCriticalSection(&lock_);

        if (closing_ || requeueQuit_) return;

        // Forward SysEx before recycling, so the staging slot is read back
        // before it can be reused by a later dump.
        for (const SysexJob& job : jobs) {
            if (job.stageSlot < 0 || job.len == 0) continue;

            std::vector<HMIDIOUT> targets;
            EnterCriticalSection(&lock_);
            blob.assign(
                staging_.begin() + static_cast<ptrdiff_t>(job.stageSlot) * kSysexBufferBytes,
                staging_.begin() + static_cast<ptrdiff_t>(job.stageSlot) * kSysexBufferBytes +
                    job.len);
            if (thruEnabled_ && job.inputSlot >= 0 &&
                static_cast<size_t>(job.inputSlot) < thruBySlot_.size()) {
                for (int oi : thruBySlot_[static_cast<size_t>(job.inputSlot)]) {
                    if (oi >= 0 && static_cast<size_t>(oi) < thruOuts_.size() &&
                        thruOuts_[static_cast<size_t>(oi)].handle) {
                        targets.push_back(thruOuts_[static_cast<size_t>(oi)].handle);
                    }
                }
            }
            LeaveCriticalSection(&lock_);

            for (HMIDIOUT h : targets) {
                sendSysexBlocking(h, blob.data(), blob.size());
            }
        }

        for (size_t i = 0; i < pending.size(); ++i) {
            const int slot = slots[i];
            if (slot < 0 || static_cast<size_t>(slot) >= devices_.size()) continue;
            HMIDIIN h = devices_[slot]->handle;
            if (!h || closing_) continue;
            pending[i]->dwBytesRecorded = 0;
            midiInAddBuffer(h, pending[i], sizeof(MIDIHDR));
        }
    }
}

// Runs on the helper thread, so blocking until the driver is done is fine.
void Engine::sendSysexBlocking(HMIDIOUT h, const uint8_t* data, size_t len) {
    if (!h || !data || len == 0) return;

    std::vector<char> buf(data, data + len);
    MIDIHDR hdr = {};
    hdr.lpData          = buf.data();
    hdr.dwBufferLength  = static_cast<DWORD>(len);
    hdr.dwBytesRecorded = static_cast<DWORD>(len);

    if (midiOutPrepareHeader(h, &hdr, sizeof(hdr)) != MMSYSERR_NOERROR) return;
    if (midiOutLongMsg(h, &hdr, sizeof(hdr)) == MMSYSERR_NOERROR) {
        for (int i = 0; i < 3000 && !(hdr.dwFlags & MHDR_DONE); ++i) Sleep(1);
    }
    for (int i = 0; i < 100; ++i) {
        if (midiOutUnprepareHeader(h, &hdr, sizeof(hdr)) != MIDIERR_STILLPLAYING) break;
        Sleep(2);
    }
}

// ------------------------------------------------------------------- drain ---

void Engine::drain(std::vector<Event>& out) {
    out.clear();

    EnterCriticalSection(&lock_);

    const size_t count = ringCount_;
    if (count == 0) {
        LeaveCriticalSection(&lock_);
        return;
    }
    out.reserve(count);

    size_t idx = (ringHead_ + kRingCapacity - count) % kRingCapacity;
    for (size_t i = 0; i < count; ++i) {
        const RawEvent& r = ring_[idx];
        idx = (idx + 1) % kRingCapacity;

        Event e;
        e.seq        = nextSeq_++;
        e.deviceSlot = r.slot;
        e.status     = r.status;
        e.d1         = r.d1;
        e.d2         = r.d2;

        const double base = (r.slot >= 0 && static_cast<size_t>(r.slot) < devices_.size())
                                ? devices_[r.slot]->baseMs
                                : 0.0;
        e.timeMs = base + static_cast<double>(r.tsMs);

        if (r.sysexSlot >= 0) {
            const uint8_t* src = &staging_[static_cast<size_t>(r.sysexSlot) * kSysexBufferBytes];
            sysexStore_.emplace_back(src, src + r.sysexLen);
            e.sysexIndex = sysexBase_ + static_cast<int64_t>(sysexStore_.size()) - 1;
            while (sysexStore_.size() > kMaxStoredSysex) {
                sysexStore_.pop_front();
                ++sysexBase_;
            }
        }

        out.push_back(e);
    }

    ringCount_ = 0;
    LeaveCriticalSection(&lock_);
}

uint64_t Engine::droppedCount() const {
    EnterCriticalSection(&lock_);
    const uint64_t d = dropped_;
    LeaveCriticalSection(&lock_);
    return d;
}

const std::vector<uint8_t>* Engine::sysexAt(int64_t index) const {
    if (index < sysexBase_) return nullptr;
    const size_t local = static_cast<size_t>(index - sysexBase_);
    if (local >= sysexStore_.size()) return nullptr;
    return &sysexStore_[local];
}

// ------------------------------------------------------------------ output ---

bool Engine::openOutput(UINT id) {
    closeOutput();
    return midiOutOpen(&hOut_, id, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
}

void Engine::closeOutput() {
    if (hOut_) {
        midiOutReset(hOut_);
        midiOutClose(hOut_);
        hOut_ = nullptr;
    }
}

bool Engine::sendShort(uint8_t status, uint8_t d1, uint8_t d2) {
    if (!hOut_) return false;
    const DWORD msg = static_cast<DWORD>(status) |
                      (static_cast<DWORD>(d1) << 8) |
                      (static_cast<DWORD>(d2) << 16);
    return midiOutShortMsg(hOut_, msg) == MMSYSERR_NOERROR;
}

// --------------------------------------------------------------- MIDI thru ---

int Engine::slotForInput(UINT inputId) const {
    for (size_t i = 0; i < openIds_.size(); ++i) {
        if (openIds_[i] == inputId) return static_cast<int>(i);
    }
    return -1;
}

size_t Engine::thruActiveCount() const {
    EnterCriticalSection(&lock_);
    size_t n = 0;
    for (const auto& targets : thruBySlot_) n += targets.size();
    LeaveCriticalSection(&lock_);
    return n;
}

void Engine::setThruEnabled(bool on) {
    InterlockedExchange(&thruEnabled_, on ? 1 : 0);
    if (!on) {
        // Leaving a held note ringing on the far side would be unkind.
        EnterCriticalSection(&lock_);
        std::vector<HMIDIOUT> handles;
        for (const ThruOut& o : thruOuts_) {
            if (o.handle) handles.push_back(o.handle);
        }
        LeaveCriticalSection(&lock_);
        for (HMIDIOUT h : handles) {
            for (uint8_t ch = 0; ch < 16; ++ch) {
                const DWORD status = 0xB0u | ch;
                midiOutShortMsg(h, status | (123u << 8));   // all notes off
                midiOutShortMsg(h, status | (120u << 8));   // all sound off
            }
        }
    }
}

void Engine::setThruRoutes(const std::vector<ThruRoute>& routes,
                           std::vector<std::wstring>&    errors) {
    thruRoutes_ = routes;
    rebuildThru(errors);
}

void Engine::closeThruOutputs() {
    std::vector<ThruOut> old;
    EnterCriticalSection(&lock_);
    old.swap(thruOuts_);
    thruBySlot_.clear();
    sysexForward_.clear();
    LeaveCriticalSection(&lock_);

    // Closing outside the lock: midiOutClose can take a while and callbacks
    // must not be held up behind it.
    for (ThruOut& o : old) {
        if (!o.handle) continue;
        midiOutReset(o.handle);
        midiOutClose(o.handle);
    }
}

void Engine::rebuildThru(std::vector<std::wstring>& errors) {
    // Detach and close whatever is currently wired up. Route changes come from
    // a button click, so reopening from scratch is cheap enough to be worth
    // the much simpler code.
    closeThruOutputs();

    if (devices_.empty()) return;   // routes are remembered until capture starts

    const auto outputs = enumerateOutputs();
    auto outputName = [&outputs](UINT id) -> std::wstring {
        for (const auto& d : outputs) {
            if (d.id == id) return d.name;
        }
        return L"(output " + std::to_wstring(id) + L")";
    };

    std::vector<ThruOut>          outs;
    std::vector<std::vector<int>> bySlot(devices_.size());

    for (const ThruRoute& r : thruRoutes_) {
        const int slot = slotForInput(r.inputId);
        if (slot < 0) continue;     // that input is not open in this capture

        int oi = -1;
        for (size_t i = 0; i < outs.size(); ++i) {
            if (outs[i].id == r.outputId) { oi = static_cast<int>(i); break; }
        }
        if (oi < 0) {
            HMIDIOUT h = nullptr;
            const MMRESULT res = midiOutOpen(&h, r.outputId, 0, 0, CALLBACK_NULL);
            if (res != MMSYSERR_NOERROR) {
                errors.push_back(outputName(r.outputId) + L": " + errorText(res) +
                                 L" (another application may already have it open)");
                continue;
            }
            outs.push_back({r.outputId, h});
            oi = static_cast<int>(outs.size()) - 1;
        }

        auto& targets = bySlot[static_cast<size_t>(slot)];
        if (std::find(targets.begin(), targets.end(), oi) == targets.end()) {
            targets.push_back(oi);
        }
    }

    EnterCriticalSection(&lock_);
    thruOuts_.swap(outs);
    thruBySlot_.swap(bySlot);
    LeaveCriticalSection(&lock_);
}

bool Engine::sendPanic() {
    if (!hOut_) return false;
    for (uint8_t ch = 0; ch < 16; ++ch) {
        const uint8_t status = static_cast<uint8_t>(0xB0 | ch);
        sendShort(status, 64, 0);    // sustain pedal up
        sendShort(status, 120, 0);   // all sound off
        sendShort(status, 123, 0);   // all notes off
    }
    return true;
}

} // namespace midi

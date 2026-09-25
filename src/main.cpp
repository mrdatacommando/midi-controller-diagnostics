// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Mark Van de Velde

// main.cpp - MidiScope: a live MIDI monitor and diagnostic window.

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "log_writer.h"
#include "midi_decode.h"
#include "midi_engine.h"

#pragma comment(linker,                                                    \
                "\"/manifestdependency:type='win32' "                      \
                "name='Microsoft.Windows.Common-Controls' version='6.0.0.0' " \
                "processorArchitecture='*' publicKeyToken='6595b64144ccf1df' " \
                "language='*'\"")

namespace {

// ------------------------------------------------------------- identifiers ---

enum : int {
    IDC_BTN_START = 1000,
    IDC_BTN_FREEZE,
    IDC_BTN_CLEAR,
    IDC_BTN_RESCAN,
    IDC_BTN_SAVE,
    IDC_BTN_STREAM,
    IDC_CHK_AUTOSCROLL,
    IDC_LIST_DEVICES,
    IDC_LIST_FEED,
    IDC_LIST_ACTIVITY,
    IDC_CMB_CHANNEL,
    IDC_CMB_OUT,
    IDC_CMB_SEND_CH,
    IDC_CMB_SEND_TYPE,
    IDC_EDIT_D1,
    IDC_EDIT_D2,
    IDC_BTN_SEND,
    IDC_BTN_PANIC,
    IDC_STATUS,
    IDC_LBL_DEVICES,
    IDC_LBL_FILTERS,
    IDC_LBL_CHANNEL,
    IDC_LBL_ACTIVITY,
    IDC_LBL_SEND,
    IDC_LBL_D1,
    IDC_LBL_D2,
    IDC_CMB_THRU_IN,
    IDC_CMB_THRU_OUT,
    IDC_BTN_THRU_ADD,
    IDC_BTN_THRU_REMOVE,
    IDC_CMB_THRU_ROUTES,
    IDC_CHK_THRU_ON,
    IDC_LBL_THRU,
    IDC_LBL_THRU_ARROW,
    IDC_SLIDER_DIAL,
    IDC_LBL_DIAL,
    IDC_LBL_DIAL_VALUE,
    IDC_BTN_SWEEP,
    IDC_CHK_FILTER_FIRST = 1100   // ten consecutive ids follow
};

enum : UINT {
    TIMER_DRAIN = 1,   // pull events out of the engine
    TIMER_UI    = 2,   // status bar, activity panel, hot-plug check
    TIMER_SWEEP = 3    // automatic dial sweep
};

constexpr size_t kMaxEvents     = 300000;
constexpr size_t kTrimChunk     = 30000;
constexpr int    kOutDeviceSlot = -2;   // marks an event we sent ourselves

struct FilterDef {
    unsigned       bit;
    const wchar_t* label;
};

const FilterDef kFilters[] = {
    {midi::F_NOTE,       L"Notes"},
    {midi::F_CC,         L"Control Change"},
    {midi::F_PITCHBEND,  L"Pitch Bend"},
    {midi::F_AFTERTOUCH, L"Aftertouch"},
    {midi::F_PROGRAM,    L"Program Change"},
    {midi::F_SYSEX,      L"SysEx"},
    {midi::F_TRANSPORT,  L"Transport"},
    {midi::F_OTHER,      L"Other"},
    {midi::F_CLOCK,      L"Clock (noisy)"},
    {midi::F_SENSING,    L"Active Sensing (noisy)"},
};
constexpr int kFilterCount = static_cast<int>(_countof(kFilters));

// Message types available in the send panel.
struct SendType {
    const wchar_t* label;
    uint8_t        status;   // high nibble for channel messages, full byte for system
    bool           channelMessage;
    int            dataBytes;
};

const SendType kSendTypes[] = {
    {L"Note On",         0x90, true,  2},
    {L"Note Off",        0x80, true,  2},
    {L"Control Change",  0xB0, true,  2},
    {L"Program Change",  0xC0, true,  1},
    {L"Pitch Bend",      0xE0, true,  2},
    {L"Poly Aftertouch", 0xA0, true,  2},
    {L"Ch Aftertouch",   0xD0, true,  1},
    {L"Start",           0xFA, false, 0},
    {L"Continue",        0xFB, false, 0},
    {L"Stop",            0xFC, false, 0},
    {L"Timing Clock",    0xF8, false, 0},
};

// --------------------------------------------------------- activity panel ---

enum class ActKind : uint8_t { Note, CC, Bend, PolyAT, ChanAT, Program, Clock, Other };

struct ActivityKey {
    int     slot;
    int     channel;
    ActKind kind;
    uint8_t number;

    bool operator<(const ActivityKey& o) const {
        if (slot != o.slot)       return slot < o.slot;
        if (channel != o.channel) return channel < o.channel;
        if (kind != o.kind)       return kind < o.kind;
        return number < o.number;
    }
};

struct ActivityValue {
    int      lastValue = 0;
    uint64_t count     = 0;
    double   lastMs    = 0.0;
    uint64_t lastSeq   = 0;
    double   bpm       = 0.0;   // clock rows only
};

struct ActivityRow {
    std::wstring device;
    std::wstring channel;
    std::wstring control;
    std::wstring value;
    std::wstring count;
    std::wstring lastSeen;
};

// ------------------------------------------------------------------ helpers ---

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

// ============================================================================

class App : public logio::Source {
public:
    bool create(HINSTANCE inst, int showCmd);
    HWND window() const { return hwnd_; }
    static LRESULT CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    // logio::Source - device slots and SysEx indices stay valid for the whole
    // session, even across stop/start with a different device selection.
    std::wstring deviceName(int slot) const override;
    const std::vector<uint8_t>* sysex(int64_t index) const override;
    std::vector<std::wstring> activeDeviceNames() const override;

private:
    // lifecycle
    void onCreate(HWND hwnd);
    void onSize();
    void onDestroy();
    void onCommand(int id, int code);
    LRESULT onNotify(NMHDR* hdr);

    // building blocks
    void createControls();
    void applyFont();
    int  scale(int v) const { return MulDiv(v, dpi_, 96); }

    // devices
    void refreshDeviceList(bool preserveChecks);
    void refreshOutputList();
    std::vector<UINT> checkedInputIds() const;

    // capture
    void startCapture();
    void stopCapture();
    void toggleCapture();
    void clearEvents();
    void pumpEvents();
    void appendEvent(const midi::Event& e);
    void trimIfNeeded();
    void rebuildVisible();
    bool passesFilter(const midi::Event& e) const;
    unsigned currentFilterMask() const;

    // rendering
    const midi::Event* eventForRow(int row) const;
    std::wstring cellText(const midi::Event& e, int column, int row) const;
    void updateStatus();
    void refreshActivity();
    COLORREF colourFor(const midi::Event& e) const;

    // logging
    void saveLogDialog();
    void toggleStreamLog();

    // sending
    bool ensureOutput();
    void doSend();
    void doPanic();
    void sendValue(int value);        // used by the dial and the sweep
    void onDialMoved();
    void startSweep();
    void stepSweep();

    // thru
    void refreshThruCombos();
    void refreshRouteList();
    void applyThruRoutes();
    void addThruRoute();
    void removeThruRoute();

    HWND hwnd_      = nullptr;
    HWND devices_   = nullptr;
    HWND feed_      = nullptr;
    HWND activity_  = nullptr;
    HWND status_    = nullptr;
    HWND channelCmb_ = nullptr;
    HWND outCmb_    = nullptr;
    HWND sendChCmb_ = nullptr;
    HWND sendTypeCmb_ = nullptr;
    HWND editD1_    = nullptr;
    HWND editD2_    = nullptr;
    HWND filterChecks_[kFilterCount] = {};
    HWND autoScroll_ = nullptr;
    HWND btnStart_   = nullptr;
    HWND btnFreeze_  = nullptr;
    HWND btnStream_  = nullptr;
    HWND thruInCmb_     = nullptr;
    HWND thruOutCmb_    = nullptr;
    HWND thruRoutesCmb_ = nullptr;
    HWND thruOnChk_     = nullptr;
    HWND dial_          = nullptr;
    HWND dialValueLbl_  = nullptr;

    HFONT uiFont_   = nullptr;
    HFONT monoFont_ = nullptr;
    int   dpi_      = 96;

    midi::Engine             engine_;
    std::vector<midi::Event> drained_;

    std::deque<midi::Event> events_;
    uint64_t                eventBase_ = 0;   // seq of events_.front()
    std::deque<uint64_t>    visible_;
    uint64_t                nextSeq_ = 0;

    // The engine renumbers its device slots on every start, so events are
    // remapped onto this ever-growing list as they are drained.
    std::vector<std::wstring> slotNames_;
    int                       slotBase_ = 0;

    // SysEx payloads are copied out of the engine for the same reason.
    std::deque<std::vector<uint8_t>> sysexStore_;
    int64_t                          sysexBase_ = 0;
    static constexpr size_t          kMaxStoredSysex = 8192;

    std::map<ActivityKey, ActivityValue> activityMap_;
    std::vector<ActivityRow>             activityRows_;

    std::map<int, midi::ClockTracker> clocks_;    // tempo, per sending device
    int                         lastClockSlot_ = -1;
    DWORD                       lastClockTick_ = 0;

    logio::StreamLogger streamLog_;

    bool     frozen_          = false;
    bool     updatingDevices_ = false;
    bool     sweepRunning_    = false;
    unsigned lastDeviceCount_ = 0;
    uint64_t lastRateSeq_  = 0;
    DWORD    lastRateTick_ = 0;
    double   eventsPerSec_ = 0.0;

    double        captureOffsetMs_ = 0.0;
    LARGE_INTEGER qpcFreq_{};
    LARGE_INTEGER logStartQpc_{};
    FILETIME      logStartWall_{};

    std::vector<midi::DeviceInfo> inputCache_;
    std::vector<midi::DeviceInfo> outputCache_;
    UINT                          currentOutId_ = UINT_MAX;

    std::vector<midi::ThruRoute> thruRoutes_;
    int                          sweepValue_ = 0;
    int                          sweepDir_   = 1;

    mutable std::wstring scratch_;   // backing store for LVN_GETDISPINFO
};

// ----------------------------------------------------------------- create ---

bool App::create(HINSTANCE inst, int showCmd) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = &App::wndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"MidiScopeWindow";
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return false;

    hwnd_ = CreateWindowExW(0, wc.lpszClassName,
                            L"MidiScope \x2013 live MIDI monitor",
                            WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
                            nullptr, nullptr, inst, this);
    if (!hwnd_) return false;

    ShowWindow(hwnd_, showCmd);
    UpdateWindow(hwnd_);
    return true;
}

LRESULT CALLBACK App::wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs  = reinterpret_cast<CREATESTRUCTW*>(lp);
        auto* app = static_cast<App*>(cs->lpCreateParams);
        app->hwnd_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return DefWindowProcW(h, msg, wp, lp);
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!app) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
        case WM_CREATE:
            app->onCreate(h);
            return 0;

        case WM_SIZE:
            app->onSize();
            return 0;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
            mmi->ptMinTrackSize.x = MulDiv(980, app->dpi_, 96);
            mmi->ptMinTrackSize.y = MulDiv(620, app->dpi_, 96);
            return 0;
        }

        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wp);
            auto* r   = reinterpret_cast<RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top,
                         r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->applyFont();
            app->onSize();
            return 0;
        }

        case WM_COMMAND:
            app->onCommand(LOWORD(wp), HIWORD(wp));
            return 0;

        case WM_NOTIFY:
            return app->onNotify(reinterpret_cast<NMHDR*>(lp));

        case WM_HSCROLL:
            // The trackbar reports every drag position; each one is sent.
            if (reinterpret_cast<HWND>(lp) == app->dial_) {
                app->onDialMoved();
                return 0;
            }
            return DefWindowProcW(h, msg, wp, lp);

        case WM_TIMER:
            if (wp == TIMER_SWEEP) {
                app->stepSweep();
            } else if (wp == TIMER_DRAIN) {
                app->pumpEvents();
            } else if (wp == TIMER_UI) {
                app->updateStatus();
                app->refreshActivity();
                const unsigned n = midiInGetNumDevs();
                if (n != app->lastDeviceCount_) {
                    app->lastDeviceCount_ = n;
                    app->refreshDeviceList(true);
                    app->refreshOutputList();
                    app->refreshThruCombos();
                    app->refreshRouteList();
                }
            }
            return 0;

        case WM_CLOSE:
            DestroyWindow(h);
            return 0;

        case WM_DESTROY:
            app->onDestroy();
            PostQuitMessage(0);
            return 0;

        default:
            return DefWindowProcW(h, msg, wp, lp);
    }
}

void App::onCreate(HWND hwnd) {
    hwnd_ = hwnd;

    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    auto getDpi = reinterpret_cast<UINT(WINAPI*)(HWND)>(
        user32 ? GetProcAddress(user32, "GetDpiForWindow") : nullptr);
    dpi_ = getDpi ? static_cast<int>(getDpi(hwnd)) : 96;
    if (dpi_ <= 0) dpi_ = 96;

    QueryPerformanceFrequency(&qpcFreq_);
    GetSystemTimeAsFileTime(&logStartWall_);
    QueryPerformanceCounter(&logStartQpc_);

    createControls();
    applyFont();

    refreshDeviceList(false);
    refreshOutputList();
    refreshThruCombos();
    refreshRouteList();
    engine_.setThruEnabled(true);
    lastDeviceCount_ = midiInGetNumDevs();

    onSize();
    updateStatus();

    SetTimer(hwnd_, TIMER_DRAIN, 30, nullptr);
    SetTimer(hwnd_, TIMER_UI, 250, nullptr);
}

void App::onDestroy() {
    KillTimer(hwnd_, TIMER_DRAIN);
    KillTimer(hwnd_, TIMER_UI);
    KillTimer(hwnd_, TIMER_SWEEP);
    streamLog_.close();
    engine_.stop();
    engine_.closeOutput();
    if (uiFont_)   DeleteObject(uiFont_);
    if (monoFont_) DeleteObject(monoFont_);
}

// --------------------------------------------------------------- controls ---

void App::createControls() {
    HINSTANCE inst = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(hwnd_, GWLP_HINSTANCE));

    auto mkButton = [&](int id, const wchar_t* text, DWORD extra = 0) {
        return CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | extra,
                               0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };
    auto mkLabel = [&](int id, const wchar_t* text) {
        return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                               0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst, nullptr);
    };

    btnStart_  = mkButton(IDC_BTN_START,  L"Start capture", BS_DEFPUSHBUTTON);
    btnFreeze_ = mkButton(IDC_BTN_FREEZE, L"Freeze view");
    mkButton(IDC_BTN_CLEAR,  L"Clear");
    mkButton(IDC_BTN_RESCAN, L"Rescan devices");
    mkButton(IDC_BTN_SAVE,   L"Save log\x2026");
    btnStream_ = mkButton(IDC_BTN_STREAM, L"Stream to file\x2026");
    autoScroll_ = mkButton(IDC_CHK_AUTOSCROLL, L"Auto-scroll", BS_AUTOCHECKBOX);
    SendMessageW(autoScroll_, BM_SETCHECK, BST_CHECKED, 0);

    mkLabel(IDC_LBL_DEVICES, L"MIDI inputs \x2013 tick the ones to watch");
    devices_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                   LVS_NOCOLUMNHEADER | LVS_SINGLESEL,
                               0, 0, 10, 10, hwnd_,
                               reinterpret_cast<HMENU>(IDC_LIST_DEVICES), inst, nullptr);
    ListView_SetExtendedListViewStyle(
        devices_, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW col = {};
    col.mask = LVCF_WIDTH | LVCF_TEXT;
    col.cx   = scale(240);
    col.pszText = const_cast<wchar_t*>(L"Device");
    ListView_InsertColumn(devices_, 0, &col);

    mkLabel(IDC_LBL_FILTERS, L"Show message types");
    for (int i = 0; i < kFilterCount; ++i) {
        filterChecks_[i] = mkButton(IDC_CHK_FILTER_FIRST + i, kFilters[i].label, BS_AUTOCHECKBOX);
        const bool on = (midi::kDefaultFilter & kFilters[i].bit) != 0;
        SendMessageW(filterChecks_[i], BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    mkLabel(IDC_LBL_CHANNEL, L"Channel");
    channelCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  0, 0, 10, 200, hwnd_,
                                  reinterpret_cast<HMENU>(IDC_CMB_CHANNEL), inst, nullptr);
    SendMessageW(channelCmb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All channels"));
    for (int c = 1; c <= 16; ++c) {
        SendMessageW(channelCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(formatf(L"Channel %d", c).c_str()));
    }
    SendMessageW(channelCmb_, CB_SETCURSEL, 0, 0);

    feed_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
                                LVS_OWNERDATA | LVS_SHOWSELALWAYS,
                            0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(IDC_LIST_FEED), inst, nullptr);
    ListView_SetExtendedListViewStyle(
        feed_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

    struct ColDef { const wchar_t* title; int width; };
    const ColDef feedCols[] = {
        {L"Time",         95}, {L"\x0394 ms",  58}, {L"Device",   140},
        {L"Ch",           32}, {L"Message",   112}, {L"Data 1",   175},
        {L"Data 2",       56}, {L"What it means", 310}, {L"Raw", 112},
    };
    for (int i = 0; i < static_cast<int>(_countof(feedCols)); ++i) {
        LVCOLUMNW c = {};
        c.mask    = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        c.cx      = scale(feedCols[i].width);
        c.iSubItem = i;
        c.pszText = const_cast<wchar_t*>(feedCols[i].title);
        ListView_InsertColumn(feed_, i, &c);
    }

    mkLabel(IDC_LBL_ACTIVITY, L"Activity \x2013 every control seen, newest first");
    activity_ = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_OWNERDATA,
                                0, 0, 10, 10, hwnd_,
                                reinterpret_cast<HMENU>(IDC_LIST_ACTIVITY), inst, nullptr);
    ListView_SetExtendedListViewStyle(
        activity_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

    const ColDef actCols[] = {
        {L"Device", 150}, {L"Ch", 34}, {L"Control", 250},
        {L"Value",   90}, {L"Hits", 70}, {L"Last seen", 110},
    };
    for (int i = 0; i < static_cast<int>(_countof(actCols)); ++i) {
        LVCOLUMNW c = {};
        c.mask     = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;
        c.cx       = scale(actCols[i].width);
        c.iSubItem = i;
        c.pszText  = const_cast<wchar_t*>(actCols[i].title);
        ListView_InsertColumn(activity_, i, &c);
    }

    // --- MIDI thru ---
    mkLabel(IDC_LBL_THRU, L"MIDI thru \x2013 route one device into another (runs while capturing)");
    thruInCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 240, hwnd_,
                                 reinterpret_cast<HMENU>(IDC_CMB_THRU_IN), inst, nullptr);
    mkLabel(IDC_LBL_THRU_ARROW, L"\x2192");
    thruOutCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                  0, 0, 10, 240, hwnd_,
                                  reinterpret_cast<HMENU>(IDC_CMB_THRU_OUT), inst, nullptr);
    mkButton(IDC_BTN_THRU_ADD, L"Add route");
    thruRoutesCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                     0, 0, 10, 240, hwnd_,
                                     reinterpret_cast<HMENU>(IDC_CMB_THRU_ROUTES), inst, nullptr);
    mkButton(IDC_BTN_THRU_REMOVE, L"Remove");
    thruOnChk_ = mkButton(IDC_CHK_THRU_ON, L"Thru on", BS_AUTOCHECKBOX);
    SendMessageW(thruOnChk_, BM_SETCHECK, BST_CHECKED, 0);

    // --- send panel ---
    mkLabel(IDC_LBL_SEND, L"Send test message");
    outCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                              0, 0, 10, 240, hwnd_,
                              reinterpret_cast<HMENU>(IDC_CMB_OUT), inst, nullptr);

    sendChCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                 0, 0, 10, 240, hwnd_,
                                 reinterpret_cast<HMENU>(IDC_CMB_SEND_CH), inst, nullptr);
    for (int c = 1; c <= 16; ++c) {
        SendMessageW(sendChCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(formatf(L"Ch %d", c).c_str()));
    }
    SendMessageW(sendChCmb_, CB_SETCURSEL, 0, 0);

    sendTypeCmb_ = CreateWindowExW(0, L"COMBOBOX", L"",
                                   WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                   0, 0, 10, 240, hwnd_,
                                   reinterpret_cast<HMENU>(IDC_CMB_SEND_TYPE), inst, nullptr);
    for (const SendType& t : kSendTypes) {
        SendMessageW(sendTypeCmb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t.label));
    }
    SendMessageW(sendTypeCmb_, CB_SETCURSEL, 0, 0);

    mkLabel(IDC_LBL_D1, L"Data 1");
    editD1_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"60",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                              0, 0, 10, 10, hwnd_,
                              reinterpret_cast<HMENU>(IDC_EDIT_D1), inst, nullptr);
    mkLabel(IDC_LBL_D2, L"Data 2");
    editD2_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"100",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_NUMBER,
                              0, 0, 10, 10, hwnd_,
                              reinterpret_cast<HMENU>(IDC_EDIT_D2), inst, nullptr);

    mkButton(IDC_BTN_SEND,  L"Send");
    mkButton(IDC_BTN_PANIC, L"Panic (all notes off)");

    // --- dial ---
    // The dial drives the value byte of whichever message type is selected
    // above, so one control sweeps a CC, a velocity, aftertouch or pitch bend
    // without needing a type picker of its own.
    mkLabel(IDC_LBL_DIAL, L"Dial \x2013 sweeps the value, sending as it moves:");
    dial_ = CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                            0, 0, 10, 10, hwnd_,
                            reinterpret_cast<HMENU>(IDC_SLIDER_DIAL), inst, nullptr);
    SendMessageW(dial_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 127));
    SendMessageW(dial_, TBM_SETPAGESIZE, 0, 8);
    SendMessageW(dial_, TBM_SETPOS, TRUE, 64);
    dialValueLbl_ = mkLabel(IDC_LBL_DIAL_VALUE, L"64");
    mkButton(IDC_BTN_SWEEP, L"Sweep 0\x2013" L"127");

    status_ = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                              WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                              0, 0, 10, 10, hwnd_,
                              reinterpret_cast<HMENU>(IDC_STATUS), inst, nullptr);
    int parts[] = {scale(190), scale(360), scale(480), scale(600), scale(860), -1};
    SendMessageW(status_, SB_SETPARTS, _countof(parts), reinterpret_cast<LPARAM>(parts));
}

void App::applyFont() {
    if (uiFont_)   DeleteObject(uiFont_);
    if (monoFont_) DeleteObject(monoFont_);

    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    ncm.lfMessageFont.lfHeight = -MulDiv(9, dpi_, 72);
    uiFont_ = CreateFontIndirectW(&ncm.lfMessageFont);

    LOGFONTW mono = ncm.lfMessageFont;
    wcscpy_s(mono.lfFaceName, L"Consolas");
    mono.lfHeight = -MulDiv(9, dpi_, 72);
    monoFont_ = CreateFontIndirectW(&mono);

    EnumChildWindows(hwnd_, [](HWND child, LPARAM param) -> BOOL {
        auto* self = reinterpret_cast<App*>(param);
        const int id = GetDlgCtrlID(child);
        HFONT f = (id == IDC_LIST_FEED || id == IDC_LIST_ACTIVITY)
                      ? self->monoFont_ : self->uiFont_;
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
}

void App::onSize() {
    if (!hwnd_ || !status_) return;

    RECT rc;
    GetClientRect(hwnd_, &rc);
    SendMessageW(status_, WM_SIZE, 0, 0);

    RECT sb;
    GetWindowRect(status_, &sb);
    const int statusH = sb.bottom - sb.top;

    const int pad       = scale(8);
    const int rowH      = scale(26);
    const int toolbarH  = scale(34);
    const int leftW     = scale(268);
    const int labelH    = scale(18);
    // Three stacked rows: thru, send, dial.
    const int bottomH   = labelH * 2 + rowH * 3 + scale(20);

    const int width  = rc.right - rc.left;
    const int height = rc.bottom - rc.top - statusH;

    auto place = [&](int id, int x, int y, int w, int h) {
        HWND c = GetDlgItem(hwnd_, id);
        if (c) MoveWindow(c, x, y, w, h, TRUE);
    };

    // Toolbar
    int x = pad;
    const int y0 = pad;
    const int bw = scale(112);
    place(IDC_BTN_START,  x, y0, bw, toolbarH - scale(6)); x += bw + scale(6);
    place(IDC_BTN_FREEZE, x, y0, bw, toolbarH - scale(6)); x += bw + scale(6);
    place(IDC_BTN_CLEAR,  x, y0, scale(72), toolbarH - scale(6)); x += scale(78);
    place(IDC_BTN_RESCAN, x, y0, scale(120), toolbarH - scale(6)); x += scale(126);
    place(IDC_BTN_SAVE,   x, y0, scale(96), toolbarH - scale(6)); x += scale(102);
    place(IDC_BTN_STREAM, x, y0, scale(130), toolbarH - scale(6)); x += scale(136);
    place(IDC_CHK_AUTOSCROLL, x, y0 + scale(4), scale(100), scale(20));

    const int contentY = y0 + toolbarH + pad;
    const int contentH = height - contentY - pad;

    // Left column
    int ly = contentY;
    place(IDC_LBL_DEVICES, pad, ly, leftW, labelH);
    ly += labelH + scale(2);

    const int filtersH = kFilterCount * scale(21) + labelH + scale(6);
    const int channelH = labelH + rowH + scale(6);
    const int devicesH = (std::max)(scale(120), contentH - filtersH - channelH - scale(12));

    MoveWindow(devices_, pad, ly, leftW, devicesH, TRUE);
    ListView_SetColumnWidth(devices_, 0, leftW - scale(28));
    ly += devicesH + scale(10);

    place(IDC_LBL_FILTERS, pad, ly, leftW, labelH);
    ly += labelH + scale(2);
    for (int i = 0; i < kFilterCount; ++i) {
        MoveWindow(filterChecks_[i], pad + scale(4), ly, leftW - scale(8), scale(20), TRUE);
        ly += scale(21);
    }
    ly += scale(6);
    place(IDC_LBL_CHANNEL, pad, ly, leftW, labelH);
    ly += labelH + scale(2);
    MoveWindow(channelCmb_, pad + scale(4), ly, leftW - scale(8), scale(200), TRUE);

    // Right column
    const int rx = pad + leftW + pad;
    const int rw = width - rx - pad;

    const int bottomTop   = contentY + contentH - bottomH;
    const int activityH   = (std::max)(scale(96), (contentH - bottomH) / 4);
    const int activityTop = bottomTop - activityH - pad;
    const int feedH       = activityTop - labelH - contentY - scale(2);

    MoveWindow(feed_, rx, contentY, rw, (std::max)(scale(120), feedH), TRUE);

    place(IDC_LBL_ACTIVITY, rx, activityTop - labelH, rw, labelH);
    MoveWindow(activity_, rx, activityTop, rw, activityH, TRUE);

    // Thru row
    int ty = bottomTop;
    place(IDC_LBL_THRU, rx, ty, scale(520), labelH);
    ty += labelH + scale(2);
    int sx = rx;
    MoveWindow(thruInCmb_, sx, ty, scale(180), scale(240), TRUE);  sx += scale(184);
    place(IDC_LBL_THRU_ARROW, sx, ty + scale(4), scale(14), labelH); sx += scale(18);
    MoveWindow(thruOutCmb_, sx, ty, scale(180), scale(240), TRUE); sx += scale(186);
    place(IDC_BTN_THRU_ADD, sx, ty, scale(84), rowH);              sx += scale(92);
    MoveWindow(thruRoutesCmb_, sx, ty, scale(260), scale(240), TRUE); sx += scale(266);
    place(IDC_BTN_THRU_REMOVE, sx, ty, scale(72), rowH);           sx += scale(78);
    place(IDC_CHK_THRU_ON, sx, ty + scale(4), scale(80), scale(20));

    // Send row
    int sy = ty + rowH + scale(6);
    place(IDC_LBL_SEND, rx, sy, scale(200), labelH);
    sy += labelH + scale(2);
    sx = rx;
    MoveWindow(outCmb_,      sx, sy, scale(180), scale(240), TRUE); sx += scale(186);
    MoveWindow(sendChCmb_,   sx, sy, scale(66),  scale(240), TRUE); sx += scale(72);
    MoveWindow(sendTypeCmb_, sx, sy, scale(136), scale(240), TRUE); sx += scale(142);
    place(IDC_LBL_D1, sx, sy + scale(4), scale(42), labelH); sx += scale(44);
    MoveWindow(editD1_, sx, sy, scale(50), rowH, TRUE);      sx += scale(56);
    place(IDC_LBL_D2, sx, sy + scale(4), scale(42), labelH); sx += scale(44);
    MoveWindow(editD2_, sx, sy, scale(50), rowH, TRUE);      sx += scale(56);
    place(IDC_BTN_SEND,  sx, sy, scale(66), rowH);           sx += scale(72);
    place(IDC_BTN_PANIC, sx, sy, scale(146), rowH);

    // Dial row
    const int dy = sy + rowH + scale(5);
    place(IDC_LBL_DIAL, rx, dy + scale(5), scale(250), labelH);
    const int dialX = rx + scale(254);
    const int dialW = (std::min)(scale(340), (std::max)(scale(140), rw - scale(444)));
    MoveWindow(dial_, dialX, dy, dialW, rowH, TRUE);
    place(IDC_LBL_DIAL_VALUE, dialX + dialW + scale(8), dy + scale(5), scale(40), labelH);
    place(IDC_BTN_SWEEP, dialX + dialW + scale(52), dy, scale(110), rowH);
}

// ---------------------------------------------------------------- devices ---

void App::refreshDeviceList(bool preserveChecks) {
    // Ticking items programmatically raises the same notification a user
    // click does; the guard keeps that from restarting capture.
    updatingDevices_ = true;
    std::vector<std::wstring> wereChecked;
    if (preserveChecks) {
        const int n = ListView_GetItemCount(devices_);
        for (int i = 0; i < n; ++i) {
            if (ListView_GetCheckState(devices_, i)) {
                if (static_cast<size_t>(i) < inputCache_.size()) {
                    wereChecked.push_back(inputCache_[i].name);
                }
            }
        }
    }

    inputCache_ = midi::Engine::enumerateInputs();
    ListView_DeleteAllItems(devices_);

    for (size_t i = 0; i < inputCache_.size(); ++i) {
        LVITEMW item = {};
        item.mask     = LVIF_TEXT;
        item.iItem    = static_cast<int>(i);
        item.pszText  = const_cast<wchar_t*>(inputCache_[i].name.c_str());
        ListView_InsertItem(devices_, &item);

        const bool check =
            std::find(wereChecked.begin(), wereChecked.end(), inputCache_[i].name) !=
            wereChecked.end();
        ListView_SetCheckState(devices_, static_cast<int>(i), check);
    }

    if (inputCache_.empty()) {
        LVITEMW item = {};
        item.mask    = LVIF_TEXT;
        item.iItem   = 0;
        item.pszText = const_cast<wchar_t*>(L"(no MIDI inputs found)");
        ListView_InsertItem(devices_, &item);
    }

    updatingDevices_ = false;
}

void App::refreshOutputList() {
    // Device ids can shift as ports come and go, so drop any cached handle.
    engine_.closeOutput();
    currentOutId_ = UINT_MAX;

    const int previous = static_cast<int>(SendMessageW(outCmb_, CB_GETCURSEL, 0, 0));
    outputCache_ = midi::Engine::enumerateOutputs();
    SendMessageW(outCmb_, CB_RESETCONTENT, 0, 0);
    if (outputCache_.empty()) {
        SendMessageW(outCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(no MIDI outputs)"));
    } else {
        for (const auto& d : outputCache_) {
            SendMessageW(outCmb_, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(d.name.c_str()));
        }
    }
    const int count = static_cast<int>(SendMessageW(outCmb_, CB_GETCOUNT, 0, 0));
    SendMessageW(outCmb_, CB_SETCURSEL,
                 (previous >= 0 && previous < count) ? previous : 0, 0);
}

std::vector<UINT> App::checkedInputIds() const {
    std::vector<UINT> ids;
    const int n = ListView_GetItemCount(devices_);
    for (int i = 0; i < n && static_cast<size_t>(i) < inputCache_.size(); ++i) {
        if (ListView_GetCheckState(devices_, i)) ids.push_back(inputCache_[i].id);
    }
    return ids;
}

// -------------------------------------------------------------- MIDI thru ---

void App::refreshThruCombos() {
    const int prevIn  = static_cast<int>(SendMessageW(thruInCmb_, CB_GETCURSEL, 0, 0));
    const int prevOut = static_cast<int>(SendMessageW(thruOutCmb_, CB_GETCURSEL, 0, 0));

    SendMessageW(thruInCmb_, CB_RESETCONTENT, 0, 0);
    for (const auto& d : inputCache_) {
        SendMessageW(thruInCmb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
    }
    if (inputCache_.empty()) {
        SendMessageW(thruInCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(no MIDI inputs)"));
    }

    SendMessageW(thruOutCmb_, CB_RESETCONTENT, 0, 0);
    for (const auto& d : outputCache_) {
        SendMessageW(thruOutCmb_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name.c_str()));
    }
    if (outputCache_.empty()) {
        SendMessageW(thruOutCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(no MIDI outputs)"));
    }

    const int nIn  = static_cast<int>(SendMessageW(thruInCmb_, CB_GETCOUNT, 0, 0));
    const int nOut = static_cast<int>(SendMessageW(thruOutCmb_, CB_GETCOUNT, 0, 0));
    SendMessageW(thruInCmb_, CB_SETCURSEL,
                 (prevIn >= 0 && prevIn < nIn) ? prevIn : 0, 0);
    SendMessageW(thruOutCmb_, CB_SETCURSEL,
                 (prevOut >= 0 && prevOut < nOut) ? prevOut : 0, 0);
}

void App::refreshRouteList() {
    const int prev = static_cast<int>(SendMessageW(thruRoutesCmb_, CB_GETCURSEL, 0, 0));
    SendMessageW(thruRoutesCmb_, CB_RESETCONTENT, 0, 0);

    auto nameOf = [](const std::vector<midi::DeviceInfo>& list, UINT id) -> std::wstring {
        for (const auto& d : list) {
            if (d.id == id) return d.name;
        }
        return L"(device " + std::to_wstring(id) + L", not connected)";
    };

    for (const midi::ThruRoute& r : thruRoutes_) {
        SendMessageW(thruRoutesCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(
                         (nameOf(inputCache_, r.inputId) + L"  \x2192  " +
                          nameOf(outputCache_, r.outputId)).c_str()));
    }
    if (thruRoutes_.empty()) {
        SendMessageW(thruRoutesCmb_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"(no routes)"));
        SendMessageW(thruRoutesCmb_, CB_SETCURSEL, 0, 0);
        return;
    }

    const int n = static_cast<int>(thruRoutes_.size());
    SendMessageW(thruRoutesCmb_, CB_SETCURSEL, (prev >= 0 && prev < n) ? prev : n - 1, 0);
}

void App::applyThruRoutes() {
    std::vector<std::wstring> errors;
    engine_.setThruRoutes(thruRoutes_, errors);
    engine_.setThruEnabled(
        SendMessageW(thruOnChk_, BM_GETCHECK, 0, 0) == BST_CHECKED);

    if (!errors.empty()) {
        std::wstring text = L"Some thru routes could not be connected:\n\n";
        for (const auto& e : errors) text += L"  \x2022  " + e + L"\n";
        MessageBoxW(hwnd_, text.c_str(), L"MIDI thru", MB_OK | MB_ICONWARNING);
    }
    updateStatus();
}

void App::addThruRoute() {
    const int inSel  = static_cast<int>(SendMessageW(thruInCmb_, CB_GETCURSEL, 0, 0));
    const int outSel = static_cast<int>(SendMessageW(thruOutCmb_, CB_GETCURSEL, 0, 0));
    if (inSel < 0 || static_cast<size_t>(inSel) >= inputCache_.size() ||
        outSel < 0 || static_cast<size_t>(outSel) >= outputCache_.size()) {
        MessageBoxW(hwnd_, L"Choose a MIDI input and a MIDI output to connect.",
                    L"Add route", MB_OK | MB_ICONINFORMATION);
        return;
    }

    midi::ThruRoute route;
    route.inputId  = inputCache_[static_cast<size_t>(inSel)].id;
    route.outputId = outputCache_[static_cast<size_t>(outSel)].id;

    for (const midi::ThruRoute& r : thruRoutes_) {
        if (r.inputId == route.inputId && r.outputId == route.outputId) {
            MessageBoxW(hwnd_, L"That route is already in the list.", L"Add route",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
    }

    // Routing a port back into its own other half is a classic way to build a
    // feedback loop, so it is worth a second look before wiring it up.
    if (inputCache_[static_cast<size_t>(inSel)].name ==
        outputCache_[static_cast<size_t>(outSel)].name) {
        const std::wstring warn =
            L"The input and output have the same name:\n\n    " +
            inputCache_[static_cast<size_t>(inSel)].name +
            L"\n\nRouting a device straight back to itself can create a feedback "
            L"loop that floods the port. Add it anyway?";
        if (MessageBoxW(hwnd_, warn.c_str(), L"Add route",
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
            return;
        }
    }

    thruRoutes_.push_back(route);
    refreshRouteList();
    applyThruRoutes();
}

void App::removeThruRoute() {
    if (thruRoutes_.empty()) return;
    const int sel = static_cast<int>(SendMessageW(thruRoutesCmb_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= thruRoutes_.size()) return;

    thruRoutes_.erase(thruRoutes_.begin() + sel);
    refreshRouteList();
    applyThruRoutes();
}

// ---------------------------------------------------------------- capture ---

void App::startCapture() {
    const std::vector<UINT> ids = checkedInputIds();
    if (ids.empty()) {
        MessageBoxW(hwnd_,
                    L"Tick at least one MIDI input in the list on the left, then start again.",
                    L"Nothing selected", MB_OK | MB_ICONINFORMATION);
        return;
    }

    std::vector<std::wstring> errors;
    const bool ok = engine_.start(ids, errors);

    if (!errors.empty()) {
        std::wstring text = ok ? L"Some devices could not be opened:\n\n"
                               : L"No devices could be opened:\n\n";
        for (const auto& e : errors) text += L"  \x2022  " + e + L"\n";
        MessageBoxW(hwnd_, text.c_str(), L"MIDI input",
                    MB_OK | (ok ? MB_ICONWARNING : MB_ICONERROR));
    }
    if (!ok) return;

    // New slots are appended rather than reused, so events captured before an
    // earlier stop keep naming the device they really came from.
    slotBase_ = static_cast<int>(slotNames_.size());
    for (const std::wstring& name : engine_.openDeviceNames()) {
        slotNames_.push_back(name);
    }

    // The engine restarts its clock at zero each time. Anchor the first
    // capture to the display timeline, and shift later ones onto it so a
    // stop/start pair does not make timestamps jump backwards.
    const FILETIME engineStart = engine_.captureStartTime();
    if (events_.empty()) {
        logStartWall_ = engineStart;
        QueryPerformanceCounter(&logStartQpc_);
        captureOffsetMs_ = 0.0;
    } else {
        ULARGE_INTEGER a, b;
        a.LowPart  = engineStart.dwLowDateTime;
        a.HighPart = engineStart.dwHighDateTime;
        b.LowPart  = logStartWall_.dwLowDateTime;
        b.HighPart = logStartWall_.dwHighDateTime;
        captureOffsetMs_ =
            static_cast<double>(static_cast<long long>(a.QuadPart - b.QuadPart)) / 10000.0;
    }

    SetWindowTextW(btnStart_, L"Stop capture");
    updateStatus();
}

void App::stopCapture() {
    engine_.stop();
    SetWindowTextW(btnStart_, L"Start capture");
    updateStatus();
}

void App::toggleCapture() {
    if (engine_.running()) stopCapture();
    else                   startCapture();
}

void App::clearEvents() {
    events_.clear();
    visible_.clear();
    activityMap_.clear();
    activityRows_.clear();
    clocks_.clear();
    lastClockSlot_ = -1;
    lastClockTick_ = 0;
    sysexStore_.clear();
    sysexBase_ = 0;
    eventBase_ = nextSeq_;
    ListView_SetItemCountEx(feed_, 0, LVSICF_NOINVALIDATEALL);
    ListView_SetItemCountEx(activity_, 0, LVSICF_NOINVALIDATEALL);
    InvalidateRect(feed_, nullptr, TRUE);
    InvalidateRect(activity_, nullptr, TRUE);
    updateStatus();
}

unsigned App::currentFilterMask() const {
    unsigned mask = 0;
    for (int i = 0; i < kFilterCount; ++i) {
        if (SendMessageW(filterChecks_[i], BM_GETCHECK, 0, 0) == BST_CHECKED) {
            mask |= kFilters[i].bit;
        }
    }
    return mask;
}

bool App::passesFilter(const midi::Event& e) const {
    const midi::Category cat = midi::categorize(e.status, e.d1);
    if ((currentFilterMask() & midi::filterBitFor(cat)) == 0) return false;

    const int sel = static_cast<int>(SendMessageW(channelCmb_, CB_GETCURSEL, 0, 0));
    if (sel > 0) {
        const int ch = channelOf(e.status);
        if (ch != sel) return false;   // system messages have no channel, so hide them
    }
    return true;
}

void App::appendEvent(const midi::Event& in) {
    midi::Event e = in;
    e.seq = nextSeq_++;

    // Timing clock: work out the tempo of whichever device is sending it.
    // This runs before filtering, so the tempo readout keeps working even with
    // Clock unticked and the clock stream hidden from the feed.
    if (e.status == 0xF8) {
        midi::ClockTracker& t = clocks_[e.deviceSlot];
        e.bpm = static_cast<float>(t.push(e.timeMs));

        lastClockSlot_ = e.deviceSlot;
        lastClockTick_ = GetTickCount();

        ActivityValue& v = activityMap_[ActivityKey{e.deviceSlot, 0, ActKind::Clock, 0}];
        v.bpm     = t.bpm();
        v.count   = t.count();
        v.lastMs  = e.timeMs;
        v.lastSeq = e.seq;
    }

    events_.push_back(e);
    if (passesFilter(e)) visible_.push_back(e.seq);

    // Activity tracking
    const uint8_t type = e.status & 0xF0;
    ActivityKey key{e.deviceSlot, channelOf(e.status), ActKind::Other, 0};
    bool track = true;
    int  value = 0;

    if (midi::isChannelMessage(e.status)) {
        switch (type) {
            case 0x90: case 0x80:
                key.kind = ActKind::Note;   key.number = e.d1;
                value = (type == 0x90 && e.d2 > 0) ? e.d2 : 0;
                break;
            case 0xB0:
                key.kind = ActKind::CC;     key.number = e.d1; value = e.d2; break;
            case 0xE0:
                key.kind = ActKind::Bend;   key.number = 0;
                value = midi::pitchBendValue(e.d1, e.d2); break;
            case 0xA0:
                key.kind = ActKind::PolyAT; key.number = e.d1; value = e.d2; break;
            case 0xD0:
                key.kind = ActKind::ChanAT; key.number = 0; value = e.d1; break;
            case 0xC0:
                key.kind = ActKind::Program; key.number = 0; value = e.d1; break;
            default: track = false; break;
        }
    } else {
        track = false;
    }

    if (track) {
        ActivityValue& v = activityMap_[key];
        v.lastValue = value;
        v.count++;
        v.lastMs  = e.timeMs;
        v.lastSeq = e.seq;
    }
}

void App::trimIfNeeded() {
    if (events_.size() <= kMaxEvents) return;

    const size_t drop = (std::max)(kTrimChunk, events_.size() - kMaxEvents);
    const size_t actual = (std::min)(drop, events_.size());
    events_.erase(events_.begin(), events_.begin() + static_cast<ptrdiff_t>(actual));
    eventBase_ += actual;

    while (!visible_.empty() && visible_.front() < eventBase_) visible_.pop_front();
}

void App::rebuildVisible() {
    visible_.clear();
    for (const midi::Event& e : events_) {
        if (passesFilter(e)) visible_.push_back(e.seq);
    }
    ListView_SetItemCountEx(feed_, static_cast<int>(visible_.size()), LVSICF_NOSCROLL);
    InvalidateRect(feed_, nullptr, TRUE);
}

void App::pumpEvents() {
    if (!engine_.running()) return;

    engine_.drain(drained_);
    if (drained_.empty()) return;

    // Lift the engine's per-session slots and SysEx indices into the app's
    // session-wide numbering before anything else looks at them.
    for (midi::Event& e : drained_) {
        e.deviceSlot += slotBase_;
        e.timeMs     += captureOffsetMs_;
        if (e.sysexIndex >= 0) {
            const std::vector<uint8_t>* blob = engine_.sysexAt(e.sysexIndex);
            if (blob) {
                sysexStore_.push_back(*blob);
                e.sysexIndex = sysexBase_ + static_cast<int64_t>(sysexStore_.size()) - 1;
                while (sysexStore_.size() > kMaxStoredSysex) {
                    sysexStore_.pop_front();
                    ++sysexBase_;
                }
            } else {
                e.sysexIndex = -1;
            }
        }
    }

    // The file log gets every event, filtered or not.
    if (streamLog_.isOpen()) {
        streamLog_.append(drained_, *this, logStartWall_);
    }

    for (const midi::Event& e : drained_) appendEvent(e);
    trimIfNeeded();

    if (!frozen_) {
        ListView_SetItemCountEx(feed_, static_cast<int>(visible_.size()),
                                LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        if (SendMessageW(autoScroll_, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            !visible_.empty()) {
            ListView_EnsureVisible(feed_, static_cast<int>(visible_.size()) - 1, FALSE);
        }
    }
}

// -------------------------------------------------------------- rendering ---

const midi::Event* App::eventForRow(int row) const {
    if (row < 0 || static_cast<size_t>(row) >= visible_.size()) return nullptr;
    const uint64_t seq = visible_[static_cast<size_t>(row)];
    if (seq < eventBase_) return nullptr;
    const size_t idx = static_cast<size_t>(seq - eventBase_);
    if (idx >= events_.size()) return nullptr;
    return &events_[idx];
}

std::wstring App::deviceName(int slot) const {
    if (slot == kOutDeviceSlot) return L"\x2192 sent";
    if (slot >= 0 && static_cast<size_t>(slot) < slotNames_.size()) return slotNames_[slot];
    return L"(unknown)";
}

std::vector<std::wstring> App::activeDeviceNames() const {
    return engine_.openDeviceNames();
}

const std::vector<uint8_t>* App::sysex(int64_t index) const {
    if (index < sysexBase_) return nullptr;
    const size_t local = static_cast<size_t>(index - sysexBase_);
    if (local >= sysexStore_.size()) return nullptr;
    return &sysexStore_[local];
}

std::wstring App::cellText(const midi::Event& e, int column, int row) const {
    switch (column) {
        case 0:
            return logio::wallClock(logStartWall_, e.timeMs, false);
        case 1: {
            // Gap since the previous row actually shown, which is what the
            // reader is comparing against.
            const midi::Event* prev = eventForRow(row - 1);
            if (!prev) return L"\x2013";
            return formatf(L"%.1f", e.timeMs - prev->timeMs);
        }
        case 2:
            return deviceName(e.deviceSlot);
        case 3: {
            const int ch = channelOf(e.status);
            return ch ? std::to_wstring(ch) : std::wstring(L"\x2013");
        }
        case 4:
            return midi::categoryName(midi::categorize(e.status, e.d1));
        case 5:
            return midi::data1Text(e.status, e.d1);
        case 6:
            return midi::data2Text(e.status, e.d1, e.d2);
        case 7: {
            if (e.status == 0xF8) {
                return e.bpm > 0.0f
                           ? formatf(L"Timing clock \x2013 %.1f BPM from %s",
                                     e.bpm, deviceName(e.deviceSlot).c_str())
                           : std::wstring(L"Timing clock \x2013 measuring tempo\x2026");
            }
            const std::vector<uint8_t>* blob =
                (e.sysexIndex >= 0) ? sysex(e.sysexIndex) : nullptr;
            return midi::describe(e.status, e.d1, e.d2,
                                  blob ? blob->data() : nullptr,
                                  blob ? blob->size() : 0);
        }
        case 8: {
            if (e.sysexIndex >= 0) {
                const std::vector<uint8_t>* blob = sysex(e.sysexIndex);
                if (!blob) return L"(expired)";
                return midi::hexBytes(blob->data(), blob->size(), 10);
            }
            const uint8_t bytes[3] = {e.status, e.d1, e.d2};
            return midi::hexBytes(bytes, 1 + static_cast<size_t>(midi::dataByteCount(e.status)), 3);
        }
        default:
            return std::wstring();
    }
}

COLORREF App::colourFor(const midi::Event& e) const {
    switch (midi::categorize(e.status, e.d1)) {
        case midi::Category::NoteOn:
            return (e.d2 == 0) ? RGB(110, 130, 110) : RGB(20, 120, 40);
        case midi::Category::NoteOff:        return RGB(110, 130, 110);
        case midi::Category::ControlChange:  return RGB(20, 70, 170);
        case midi::Category::PitchBend:      return RGB(120, 40, 160);
        case midi::Category::PolyAftertouch:
        case midi::Category::ChannelAftertouch: return RGB(160, 80, 0);
        case midi::Category::ProgramChange:  return RGB(140, 20, 90);
        case midi::Category::SysEx:          return RGB(190, 90, 0);
        case midi::Category::Clock:
        case midi::Category::ActiveSensing:  return RGB(150, 150, 150);
        default:                             return RGB(40, 40, 40);
    }
}

void App::refreshActivity() {
    struct Entry { const ActivityKey* key; const ActivityValue* val; };
    std::vector<Entry> entries;
    entries.reserve(activityMap_.size());
    for (const auto& kv : activityMap_) entries.push_back({&kv.first, &kv.second});

    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        return a.val->lastSeq > b.val->lastSeq;
    });
    if (entries.size() > 500) entries.resize(500);

    activityRows_.clear();
    activityRows_.reserve(entries.size());

    for (const Entry& en : entries) {
        ActivityRow row;
        row.device  = deviceName(en.key->slot);
        row.channel = en.key->channel ? std::to_wstring(en.key->channel) : std::wstring(L"\x2013");

        switch (en.key->kind) {
            case ActKind::Note:
                row.control = formatf(L"Note %s (%u)",
                                      midi::noteName(en.key->number).c_str(), en.key->number);
                row.value   = en.val->lastValue ? formatf(L"vel %d", en.val->lastValue)
                                                : std::wstring(L"off");
                break;
            case ActKind::CC:
                row.control = formatf(L"CC %u \x2013 %s", en.key->number,
                                      midi::ccName(en.key->number));
                row.value   = std::to_wstring(en.val->lastValue);
                break;
            case ActKind::Bend:
                row.control = L"Pitch Bend";
                row.value   = formatf(L"%+d", en.val->lastValue);
                break;
            case ActKind::PolyAT:
                row.control = formatf(L"Aftertouch %s",
                                      midi::noteName(en.key->number).c_str());
                row.value   = std::to_wstring(en.val->lastValue);
                break;
            case ActKind::ChanAT:
                row.control = L"Channel Aftertouch";
                row.value   = std::to_wstring(en.val->lastValue);
                break;
            case ActKind::Program:
                row.control = L"Program Change";
                row.value   = formatf(L"%d \x2013 %s", en.val->lastValue,
                                      midi::gmProgramName(
                                          static_cast<uint8_t>(en.val->lastValue)));
                break;
            case ActKind::Clock:
                row.control = L"Timing Clock \x2013 tempo source";
                row.value   = en.val->bpm > 0.0 ? formatf(L"%.1f BPM", en.val->bpm)
                                                : std::wstring(L"measuring\x2026");
                break;
            default:
                row.control = L"\x2013";
                break;
        }

        row.count    = std::to_wstring(en.val->count);
        row.lastSeen = logio::wallClock(logStartWall_, en.val->lastMs, false);
        activityRows_.push_back(std::move(row));
    }

    ListView_SetItemCountEx(activity_, static_cast<int>(activityRows_.size()),
                            LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
    InvalidateRect(activity_, nullptr, FALSE);
}

void App::updateStatus() {
    const bool running = engine_.running();

    std::wstring state;
    if (!running)      state = L"Stopped";
    else if (frozen_)  state = L"Capturing (view frozen)";
    else               state = L"Capturing";

    if (!thruRoutes_.empty()) {
        const bool on = engine_.thruEnabled();
        const size_t live = engine_.thruActiveCount();
        if (!on) {
            state += L"  \x00b7  thru off";
        } else if (!running) {
            state += formatf(L"  \x00b7  %zu route%s (start capture)",
                             thruRoutes_.size(), thruRoutes_.size() == 1 ? L"" : L"s");
        } else {
            state += formatf(L"  \x00b7  thru \x00d7%zu", live);
        }
    }

    const DWORD now = GetTickCount();
    if (lastRateTick_ == 0) {
        lastRateTick_ = now;
        lastRateSeq_  = nextSeq_;
    } else if (now - lastRateTick_ >= 500) {
        const double seconds = (now - lastRateTick_) / 1000.0;
        eventsPerSec_ = static_cast<double>(nextSeq_ - lastRateSeq_) / seconds;
        lastRateTick_ = now;
        lastRateSeq_  = nextSeq_;
    }

    const uint64_t dropped = engine_.droppedCount();

    auto setPart = [&](int part, const std::wstring& text) {
        SendMessageW(status_, SB_SETTEXTW, part, reinterpret_cast<LPARAM>(text.c_str()));
    };

    // A tempo more than two seconds old is stale, not current.
    std::wstring clockText = L"No clock";
    if (lastClockTick_ && GetTickCount() - lastClockTick_ < 2000) {
        auto it = clocks_.find(lastClockSlot_);
        if (it != clocks_.end() && it->second.bpm() > 0.0) {
            clockText = formatf(L"\x2669 %.1f BPM \x2013 %s", it->second.bpm(),
                                deviceName(lastClockSlot_).c_str());
        } else {
            clockText = L"Clock \x2013 measuring\x2026";
        }
    }

    setPart(0, state);
    setPart(1, formatf(L"%zu shown / %llu captured", visible_.size(),
                       static_cast<unsigned long long>(nextSeq_)));
    setPart(2, formatf(L"%.0f msg/sec", eventsPerSec_));
    setPart(3, dropped ? formatf(L"%llu dropped", static_cast<unsigned long long>(dropped))
                       : std::wstring(L"no drops"));
    setPart(4, clockText);
    setPart(5, streamLog_.isOpen()
                   ? L"Streaming to " + std::wstring(PathFindFileNameW(streamLog_.path().c_str()))
                   : std::wstring(L"Not streaming to file"));
}

// ---------------------------------------------------------------- commands ---

void App::onCommand(int id, int code) {
    if (id >= IDC_CHK_FILTER_FIRST && id < IDC_CHK_FILTER_FIRST + kFilterCount) {
        rebuildVisible();
        return;
    }

    switch (id) {
        case IDC_BTN_START:
            toggleCapture();
            break;

        case IDC_BTN_FREEZE:
            frozen_ = !frozen_;
            SetWindowTextW(btnFreeze_, frozen_ ? L"Resume view" : L"Freeze view");
            if (!frozen_) {
                ListView_SetItemCountEx(feed_, static_cast<int>(visible_.size()), LVSICF_NOSCROLL);
                InvalidateRect(feed_, nullptr, TRUE);
            }
            updateStatus();
            break;

        case IDC_BTN_CLEAR:
            clearEvents();
            break;

        case IDC_BTN_RESCAN:
            refreshDeviceList(true);
            refreshOutputList();
            refreshThruCombos();
            refreshRouteList();
            break;

        case IDC_BTN_SAVE:
            saveLogDialog();
            break;

        case IDC_BTN_STREAM:
            toggleStreamLog();
            break;

        case IDC_CMB_CHANNEL:
            if (code == CBN_SELCHANGE) rebuildVisible();
            break;

        case IDC_BTN_SEND:
            doSend();
            break;

        case IDC_BTN_PANIC:
            doPanic();
            break;

        case IDC_BTN_THRU_ADD:
            addThruRoute();
            break;

        case IDC_BTN_THRU_REMOVE:
            removeThruRoute();
            break;

        case IDC_CHK_THRU_ON:
            engine_.setThruEnabled(
                SendMessageW(thruOnChk_, BM_GETCHECK, 0, 0) == BST_CHECKED);
            updateStatus();
            break;

        case IDC_BTN_SWEEP:
            startSweep();
            break;

        default:
            break;
    }
}

LRESULT App::onNotify(NMHDR* hdr) {
    if (!hdr) return 0;

    if (hdr->idFrom == IDC_LIST_FEED && hdr->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
        if (di->item.mask & LVIF_TEXT) {
            const midi::Event* e = eventForRow(di->item.iItem);
            scratch_ = e ? cellText(*e, di->item.iSubItem, di->item.iItem) : std::wstring();
            di->item.pszText = const_cast<wchar_t*>(scratch_.c_str());
        }
        return 0;
    }

    if (hdr->idFrom == IDC_LIST_ACTIVITY && hdr->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(hdr);
        if (di->item.mask & LVIF_TEXT) {
            const int row = di->item.iItem;
            scratch_.clear();
            if (row >= 0 && static_cast<size_t>(row) < activityRows_.size()) {
                const ActivityRow& r = activityRows_[static_cast<size_t>(row)];
                switch (di->item.iSubItem) {
                    case 0: scratch_ = r.device;   break;
                    case 1: scratch_ = r.channel;  break;
                    case 2: scratch_ = r.control;  break;
                    case 3: scratch_ = r.value;    break;
                    case 4: scratch_ = r.count;    break;
                    case 5: scratch_ = r.lastSeen; break;
                    default: break;
                }
            }
            di->item.pszText = const_cast<wchar_t*>(scratch_.c_str());
        }
        return 0;
    }

    // Colour the feed by message type so movement is obvious at a glance.
    if (hdr->idFrom == IDC_LIST_FEED && hdr->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(hdr);
        switch (cd->nmcd.dwDrawStage) {
            case CDDS_PREPAINT:
                return CDRF_NOTIFYITEMDRAW;
            case CDDS_ITEMPREPAINT: {
                const midi::Event* e = eventForRow(static_cast<int>(cd->nmcd.dwItemSpec));
                if (e) cd->clrText = colourFor(*e);
                return CDRF_DODEFAULT;
            }
            default:
                return CDRF_DODEFAULT;
        }
    }

    if (hdr->idFrom == IDC_LIST_DEVICES && hdr->code == LVN_ITEMCHANGED) {
        auto* nv = reinterpret_cast<NMLISTVIEW*>(hdr);
        // A checkbox toggle arrives as a state-image change. Reopen through
        // startCapture() rather than the engine directly, so device slots and
        // the display clock are renumbered with it.
        if (!updatingDevices_ && (nv->uChanged & LVIF_STATE) && engine_.running()) {
            const UINT before = (nv->uOldState & LVIS_STATEIMAGEMASK) >> 12;
            const UINT after  = (nv->uNewState & LVIS_STATEIMAGEMASK) >> 12;
            if (before && after && before != after) {
                stopCapture();
                if (!checkedInputIds().empty()) startCapture();
            }
        }
        return 0;
    }

    return 0;
}

// ----------------------------------------------------------------- logging ---

void App::saveLogDialog() {
    if (events_.empty()) {
        MessageBoxW(hwnd_, L"There is nothing captured yet.", L"Save log",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, _countof(file), _TRUNCATE,
                 L"midiscope-%04d%02d%02d-%02d%02d%02d.csv",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFilter = L"CSV spreadsheet (*.csv)\0*.csv\0"
                      L"JSON Lines (*.jsonl)\0*.jsonl\0"
                      L"Plain text (*.txt)\0*.txt\0";
    ofn.lpstrFile   = file;
    ofn.nMaxFile    = _countof(file);
    ofn.lpstrTitle  = L"Save capture log";
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt = L"csv";

    if (!GetSaveFileNameW(&ofn)) return;

    logio::Format fmt = logio::Format::Csv;
    if (ofn.nFilterIndex == 2)      fmt = logio::Format::Jsonl;
    else if (ofn.nFilterIndex == 3) fmt = logio::Format::Text;

    // Honour an extension the user typed by hand.
    const wchar_t* ext = PathFindExtensionW(file);
    if (ext) {
        if (_wcsicmp(ext, L".jsonl") == 0)     fmt = logio::Format::Jsonl;
        else if (_wcsicmp(ext, L".txt") == 0)  fmt = logio::Format::Text;
        else if (_wcsicmp(ext, L".csv") == 0)  fmt = logio::Format::Csv;
    }

    const std::vector<midi::Event> snapshot(events_.begin(), events_.end());
    std::wstring error;
    if (!logio::writeLog(file, fmt, snapshot, *this, logStartWall_, error)) {
        MessageBoxW(hwnd_, error.c_str(), L"Save log", MB_OK | MB_ICONERROR);
        return;
    }

    const std::wstring msg =
        formatf(L"Saved %zu events to\n\n%s", snapshot.size(), file);
    MessageBoxW(hwnd_, msg.c_str(), L"Save log", MB_OK | MB_ICONINFORMATION);
}

void App::toggleStreamLog() {
    if (streamLog_.isOpen()) {
        streamLog_.close();
        SetWindowTextW(btnStream_, L"Stream to file\x2026");
        updateStatus();
        return;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t file[MAX_PATH];
    _snwprintf_s(file, _countof(file), _TRUNCATE,
                 L"midiscope-live-%04d%02d%02d-%02d%02d%02d.csv",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd_;
    ofn.lpstrFilter  = L"CSV spreadsheet (*.csv)\0*.csv\0"
                       L"JSON Lines (*.jsonl)\0*.jsonl\0"
                       L"Plain text (*.txt)\0*.txt\0";
    ofn.lpstrFile    = file;
    ofn.nMaxFile     = _countof(file);
    ofn.lpstrTitle   = L"Stream capture to file";
    ofn.Flags        = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.nFilterIndex = 1;
    ofn.lpstrDefExt  = L"csv";

    if (!GetSaveFileNameW(&ofn)) return;

    logio::Format fmt = logio::Format::Csv;
    if (ofn.nFilterIndex == 2)      fmt = logio::Format::Jsonl;
    else if (ofn.nFilterIndex == 3) fmt = logio::Format::Text;

    std::wstring error;
    if (!streamLog_.open(file, fmt, error)) {
        MessageBoxW(hwnd_, error.c_str(), L"Stream to file", MB_OK | MB_ICONERROR);
        return;
    }
    SetWindowTextW(btnStream_, L"Stop streaming");
    updateStatus();
}

// ----------------------------------------------------------------- sending ---

// Opening a MIDI output is slow enough to be felt, so the handle is kept open
// and only swapped when the chosen device actually changes.
bool App::ensureOutput() {
    const int sel = static_cast<int>(SendMessageW(outCmb_, CB_GETCURSEL, 0, 0));
    if (sel < 0 || static_cast<size_t>(sel) >= outputCache_.size()) {
        MessageBoxW(hwnd_, L"Choose a MIDI output first.", L"Send",
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }

    const UINT id = outputCache_[static_cast<size_t>(sel)].id;
    if (engine_.outputOpen() && currentOutId_ == id) return true;

    if (!engine_.openOutput(id)) {
        currentOutId_ = UINT_MAX;
        MessageBoxW(hwnd_,
                    L"That MIDI output could not be opened. Another application may be using it.",
                    L"Send", MB_OK | MB_ICONERROR);
        return false;
    }
    currentOutId_ = id;
    return true;
}

void App::doSend() {
    if (!ensureOutput()) return;

    const int typeSel = static_cast<int>(SendMessageW(sendTypeCmb_, CB_GETCURSEL, 0, 0));
    if (typeSel < 0 || static_cast<size_t>(typeSel) >= _countof(kSendTypes)) return;
    const SendType& type = kSendTypes[typeSel];

    const int ch = static_cast<int>(SendMessageW(sendChCmb_, CB_GETCURSEL, 0, 0));

    auto readByte = [&](HWND edit) -> uint8_t {
        wchar_t text[16] = {};
        GetWindowTextW(edit, text, _countof(text));
        const int v = _wtoi(text);
        return static_cast<uint8_t>((std::max)(0, (std::min)(127, v)));
    };

    const uint8_t d1 = (type.dataBytes >= 1) ? readByte(editD1_) : 0;
    const uint8_t d2 = (type.dataBytes >= 2) ? readByte(editD2_) : 0;
    const uint8_t status = type.channelMessage
                               ? static_cast<uint8_t>(type.status | (ch & 0x0F))
                               : type.status;

    if (!engine_.sendShort(status, d1, d2)) {
        MessageBoxW(hwnd_, L"The message could not be sent.", L"Send",
                    MB_OK | MB_ICONERROR);
        return;
    }

    // Echo it into the feed so the round trip is visible in one place.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    midi::Event echo;
    echo.timeMs = static_cast<double>(now.QuadPart - logStartQpc_.QuadPart) * 1000.0 /
                  static_cast<double>(qpcFreq_.QuadPart);
    echo.deviceSlot = kOutDeviceSlot;
    echo.status = status;
    echo.d1     = d1;
    echo.d2     = d2;
    appendEvent(echo);
    trimIfNeeded();
    if (!frozen_) {
        ListView_SetItemCountEx(feed_, static_cast<int>(visible_.size()), LVSICF_NOSCROLL);
    }
}

void App::doPanic() {
    if (!ensureOutput()) return;
    engine_.sendPanic();
}

// The dial drives the value byte of whichever message type is selected: data 2
// for two-byte messages, data 1 for the one-byte ones. So the same control
// sweeps a CC, a note velocity, aftertouch or pitch bend.
void App::sendValue(int value) {
    if (!ensureOutput()) return;

    const int typeSel = static_cast<int>(SendMessageW(sendTypeCmb_, CB_GETCURSEL, 0, 0));
    if (typeSel < 0 || static_cast<size_t>(typeSel) >= _countof(kSendTypes)) return;
    const SendType& type = kSendTypes[typeSel];
    if (type.dataBytes == 0) return;   // nothing to sweep on a transport message

    const int ch = static_cast<int>(SendMessageW(sendChCmb_, CB_GETCURSEL, 0, 0));
    const uint8_t status = type.channelMessage
                               ? static_cast<uint8_t>(type.status | (ch & 0x0F))
                               : type.status;

    const int v = (std::max)(0, (std::min)(127, value));

    uint8_t d1 = 0, d2 = 0;
    if (type.dataBytes == 1) {
        d1 = static_cast<uint8_t>(v);
    } else if (type.status == 0xE0) {
        // Spread 0..127 across the full 14-bit bend range.
        const int bend = v * 129;                     // 0..16383
        d1 = static_cast<uint8_t>(bend & 0x7F);
        d2 = static_cast<uint8_t>((bend >> 7) & 0x7F);
    } else {
        wchar_t text[16] = {};
        GetWindowTextW(editD1_, text, _countof(text));
        d1 = static_cast<uint8_t>((std::max)(0, (std::min)(127, _wtoi(text))));
        d2 = static_cast<uint8_t>(v);
    }

    if (!engine_.sendShort(status, d1, d2)) return;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    midi::Event echo;
    echo.timeMs = static_cast<double>(now.QuadPart - logStartQpc_.QuadPart) * 1000.0 /
                  static_cast<double>(qpcFreq_.QuadPart);
    echo.deviceSlot = kOutDeviceSlot;
    echo.status = status;
    echo.d1     = d1;
    echo.d2     = d2;
    appendEvent(echo);
    trimIfNeeded();
    if (!frozen_) {
        ListView_SetItemCountEx(feed_, static_cast<int>(visible_.size()),
                                LVSICF_NOSCROLL | LVSICF_NOINVALIDATEALL);
        if (SendMessageW(autoScroll_, BM_GETCHECK, 0, 0) == BST_CHECKED &&
            !visible_.empty()) {
            ListView_EnsureVisible(feed_, static_cast<int>(visible_.size()) - 1, FALSE);
        }
    }
}

void App::onDialMoved() {
    const int pos = static_cast<int>(SendMessageW(dial_, TBM_GETPOS, 0, 0));
    SetWindowTextW(dialValueLbl_, std::to_wstring(pos).c_str());
    sendValue(pos);
}

void App::startSweep() {
    if (sweepRunning_) {
        KillTimer(hwnd_, TIMER_SWEEP);
        sweepRunning_ = false;
        SetWindowTextW(GetDlgItem(hwnd_, IDC_BTN_SWEEP), L"Sweep 0\x2013" L"127");
        return;
    }
    if (!ensureOutput()) return;

    sweepValue_   = 0;
    sweepDir_     = 1;
    sweepRunning_ = true;
    SetWindowTextW(GetDlgItem(hwnd_, IDC_BTN_SWEEP), L"Stop sweep");
    SetTimer(hwnd_, TIMER_SWEEP, 12, nullptr);   // 0 -> 127 -> 0 in about 3 s
}

void App::stepSweep() {
    SendMessageW(dial_, TBM_SETPOS, TRUE, sweepValue_);
    SetWindowTextW(dialValueLbl_, std::to_wstring(sweepValue_).c_str());
    sendValue(sweepValue_);

    sweepValue_ += sweepDir_;
    if (sweepValue_ > 127) { sweepValue_ = 126; sweepDir_ = -1; }
    else if (sweepValue_ < 0) {
        // One full there-and-back is enough to see a receiving control track it.
        KillTimer(hwnd_, TIMER_SWEEP);
        sweepRunning_ = false;
        sweepValue_   = 0;
        SendMessageW(dial_, TBM_SETPOS, TRUE, 0);
        SetWindowTextW(dialValueLbl_, L"0");
        SetWindowTextW(GetDlgItem(hwnd_, IDC_BTN_SWEEP), L"Sweep 0\x2013" L"127");
    }
}

} // namespace

// -------------------------------------------------------------------- main ---

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int showCmd) {
    // Per-monitor DPI where the OS supports it, loaded dynamically so the exe
    // still starts on older builds.
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        using SetCtxFn = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
        if (auto setCtx = reinterpret_cast<SetCtxFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
            setCtx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        } else {
            SetProcessDPIAware();
        }
    }

    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    App app;
    if (!app.create(inst, showCmd)) {
        MessageBoxW(nullptr, L"The main window could not be created.",
                    L"MidiScope", MB_OK | MB_ICONERROR);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Lets Tab move between the controls the way a dialog would.
        if (!IsDialogMessageW(app.window(), &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return static_cast<int>(msg.wParam);
}

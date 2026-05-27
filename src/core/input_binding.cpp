#include "input_binding.h"
#include "logging.h"

#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <xinput.h>
#include <dinput.h>

#include <array>
#include <vector>
#include <sstream>
#include <iomanip>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "xinput.lib")

namespace psvr2pt {

// ---------------------------------------------------------------------------
// GUID helpers
// ---------------------------------------------------------------------------

static std::string guid_to_string(const GUID& g) {
    char buf[40];
    snprintf(buf, sizeof(buf),
             "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
             g.Data1, g.Data2, g.Data3,
             g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
             g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return buf;
}

static bool string_to_guid(const std::string& s, GUID& out) {
    // UuidFromStringA expects no braces; strip them if present.
    const char* p = s.c_str();
    if (*p == '{') ++p;
    std::string stripped(p);
    if (!stripped.empty() && stripped.back() == '}') stripped.pop_back();
    return UuidFromStringA(reinterpret_cast<RPC_CSTR>(const_cast<char*>(stripped.c_str())),
                           &out) == RPC_S_OK;
}

// ---------------------------------------------------------------------------
// VK name lookup
// ---------------------------------------------------------------------------

static std::string vk_name(int vk) {
    struct { int vk; const char* name; } kNames[] = {
        {VK_SPACE,"Space"},{VK_RETURN,"Enter"},{VK_ESCAPE,"Escape"},
        {VK_TAB,"Tab"},{VK_BACK,"Backspace"},{VK_DELETE,"Delete"},
        {VK_INSERT,"Insert"},{VK_HOME,"Home"},{VK_END,"End"},
        {VK_PRIOR,"PageUp"},{VK_NEXT,"PageDown"},
        {VK_LEFT,"Left"},{VK_RIGHT,"Right"},{VK_UP,"Up"},{VK_DOWN,"Down"},
        {VK_F1,"F1"},{VK_F2,"F2"},{VK_F3,"F3"},{VK_F4,"F4"},
        {VK_F5,"F5"},{VK_F6,"F6"},{VK_F7,"F7"},{VK_F8,"F8"},
        {VK_F9,"F9"},{VK_F10,"F10"},{VK_F11,"F11"},{VK_F12,"F12"},
        {VK_NUMPAD0,"Num0"},{VK_NUMPAD1,"Num1"},{VK_NUMPAD2,"Num2"},
        {VK_NUMPAD3,"Num3"},{VK_NUMPAD4,"Num4"},{VK_NUMPAD5,"Num5"},
        {VK_NUMPAD6,"Num6"},{VK_NUMPAD7,"Num7"},{VK_NUMPAD8,"Num8"},
        {VK_NUMPAD9,"Num9"},{VK_MULTIPLY,"Num*"},{VK_ADD,"Num+"},
        {VK_SUBTRACT,"Num-"},{VK_DIVIDE,"Num/"},
        {VK_LSHIFT,"LShift"},{VK_RSHIFT,"RShift"},
        {VK_LCONTROL,"LCtrl"},{VK_RCONTROL,"RCtrl"},
        {VK_LMENU,"LAlt"},{VK_RMENU,"RAlt"},
    };
    for (auto& e : kNames) if (e.vk == vk) return e.name;
    if (vk >= 'A' && vk <= 'Z') return std::string(1, static_cast<char>(vk));
    if (vk >= '0' && vk <= '9') return std::string(1, static_cast<char>(vk));
    char buf[16]; snprintf(buf, sizeof(buf), "VK#%d", vk);
    return buf;
}

// ---------------------------------------------------------------------------
// XInput trigger pseudo-button constants
// ---------------------------------------------------------------------------

static constexpr unsigned kXiLT          = 0x00010000u;
static constexpr unsigned kXiRT          = 0x00020000u;
static constexpr BYTE     kTriggerThresh = 30;

static std::string xinput_button_name(unsigned mask) {
    if (mask == kXiLT) return "LT";
    if (mask == kXiRT) return "RT";
    struct { unsigned m; const char* n; } kBtns[] = {
        {0x0001,"DUp"},{0x0002,"DDown"},{0x0004,"DLeft"},{0x0008,"DRight"},
        {0x0010,"Start"},{0x0020,"Back"},{0x0040,"LThumb"},{0x0080,"RThumb"},
        {0x0100,"LB"},{0x0200,"RB"},{0x1000,"A"},{0x2000,"B"},{0x4000,"X"},{0x8000,"Y"},
    };
    for (auto& e : kBtns) if (e.m == mask) return e.n;
    return "Btn?";
}

// ---------------------------------------------------------------------------
// Minimal JSON helpers
// ---------------------------------------------------------------------------

static std::string json_str(const std::string& key, const std::string& val) {
    return "\"" + key + "\":\"" + val + "\"";
}
static std::string json_int(const std::string& key, int val) {
    return "\"" + key + "\":" + std::to_string(val);
}
static std::string json_uint(const std::string& key, unsigned val) {
    return "\"" + key + "\":" + std::to_string(val);
}
static std::string extract_json_str(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return {};
    return json.substr(pos, end - pos);
}
static int extract_json_int(const std::string& json, const std::string& key, int def = 0) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    try { return std::stoi(json.substr(pos)); } catch (...) { return def; }
}
static unsigned extract_json_uint(const std::string& json, const std::string& key, unsigned def = 0) {
    const std::string needle = "\"" + key + "\":";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return def;
    pos += needle.size();
    try { return static_cast<unsigned>(std::stoul(json.substr(pos))); } catch (...) { return def; }
}

// ---------------------------------------------------------------------------
// PassthroughBinding public API
// ---------------------------------------------------------------------------

std::string PassthroughBinding::display_name() const {
    switch (type) {
        case BindingType::None:     return "None";
        case BindingType::Keyboard: return "Key: " + vk_name(vk_code);
        case BindingType::XInput:
            return "Gamepad " + std::to_string(xinput_controller)
                 + " - " + xinput_button_name(xinput_button_mask);
        case BindingType::DirectInput: {
            const std::string prefix = dinput_device_name.empty()
                ? "HOTAS" : dinput_device_name;
            return prefix + " - Btn " + std::to_string(dinput_button_index);
        }
    }
    return "Unknown";
}

std::string PassthroughBinding::to_json() const {
    switch (type) {
        case BindingType::None:
            return "{" + json_str("type","none") + "}";
        case BindingType::Keyboard:
            return "{" + json_str("type","keyboard") + ","
                       + json_int("vk", vk_code) + "}";
        case BindingType::XInput:
            return "{" + json_str("type","xinput") + ","
                       + json_int("ctrl", xinput_controller) + ","
                       + json_uint("btn", xinput_button_mask) + "}";
        case BindingType::DirectInput:
            return "{" + json_str("type","dinput") + ","
                       + json_str("guid", dinput_device_guid) + ","
                       + json_int("btn", dinput_button_index) + "}";
    }
    return "{" + json_str("type","none") + "}";
}

PassthroughBinding PassthroughBinding::from_json(const std::string& j) {
    PassthroughBinding b;
    const std::string t = extract_json_str(j, "type");
    if (t == "keyboard") {
        b.type    = BindingType::Keyboard;
        b.vk_code = extract_json_int(j, "vk");
    } else if (t == "xinput") {
        b.type               = BindingType::XInput;
        b.xinput_controller  = extract_json_int(j, "ctrl");
        b.xinput_button_mask = extract_json_uint(j, "btn");
    } else if (t == "dinput") {
        b.type                = BindingType::DirectInput;
        b.dinput_device_guid  = extract_json_str(j, "guid");
        b.dinput_button_index = extract_json_int(j, "btn");
    }
    return b;
}

// ---------------------------------------------------------------------------
// DirectInput device enumeration helpers (shared by Poller and Capturer)
// ---------------------------------------------------------------------------

static IDirectInput8A* g_di          = nullptr;
static int             g_di_refcnt   = 0;
static bool            g_com_we_own  = false;

static IDirectInput8A* di_acquire_instance() {
    if (g_di) { ++g_di_refcnt; return g_di; }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == S_OK) {
        g_com_we_own = true;
    } else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
        // COM already initialised by the host process (e.g. a game engine).
        // RPC_E_CHANGED_MODE means a different apartment model is in use — that
        // is fine for DirectInput; we must not call CoUninitialize when done.
        g_com_we_own = false;
    } else {
        return nullptr;
    }

    hr = DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION,
                             IID_IDirectInput8A,
                             reinterpret_cast<void**>(&g_di), nullptr);
    if (FAILED(hr)) {
        if (g_com_we_own) { CoUninitialize(); g_com_we_own = false; }
        return nullptr;
    }
    g_di_refcnt = 1;
    return g_di;
}

static void di_release_instance() {
    if (--g_di_refcnt <= 0) {
        if (g_di) { g_di->Release(); g_di = nullptr; }
        if (g_com_we_own) { CoUninitialize(); g_com_we_own = false; }
        g_di_refcnt = 0;
    }
}

struct ENUM_CTX { std::vector<GUID> guids; };
static BOOL CALLBACK enum_joysticks(const DIDEVICEINSTANCEA* inst, void* ctx) {
    static_cast<ENUM_CTX*>(ctx)->guids.push_back(inst->guidInstance);
    return DIENUM_CONTINUE;
}

// Xbox controllers appear in both DInput and XInput — skip them in DInput.
static bool is_xinput_device(IDirectInput8A* di_inst, const GUID& guid) {
    IDirectInputDevice8A* dev = nullptr;
    if (FAILED(di_inst->CreateDevice(guid, &dev, nullptr))) return false;
    bool result = false;
    DIPROPGUIDANDPATH path{};
    path.diph.dwSize       = sizeof(path);
    path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
    path.diph.dwObj        = 0;
    path.diph.dwHow        = DIPH_DEVICE;
    if (SUCCEEDED(dev->GetProperty(DIPROP_GUIDANDPATH, &path.diph)))
        result = (wcsstr(path.wszPath, L"&IG_") != nullptr);
    dev->Release();
    return result;
}

// ---------------------------------------------------------------------------
// BindingPoller — polls each frame while the layer is running
// ---------------------------------------------------------------------------

struct DPollerDev {
    GUID                  guid;
    IDirectInputDevice8A* dev = nullptr;
};

struct BindingPoller::Impl {
    PassthroughBinding binding;
    DPollerDev         di_dev;
    bool               di_open = false;

    void set(const PassthroughBinding& b) {
        if (di_open) {
            di_dev.dev->Unacquire();
            di_dev.dev->Release();
            di_dev.dev = nullptr;
            di_release_instance();
            di_open = false;
        }
        binding = b;
        if (b.type != BindingType::DirectInput) return;

        GUID guid{};
        if (b.dinput_device_guid.empty() || !string_to_guid(b.dinput_device_guid, guid)) {
            PT_LOG_WARN("BindingPoller: invalid DInput GUID '{}'", b.dinput_device_guid);
            return;
        }

        IDirectInput8A* di = di_acquire_instance();
        if (!di) {
            PT_LOG_WARN("BindingPoller: failed to acquire DirectInput instance");
            return;
        }

        di_dev.guid = guid;
        HRESULT hr = di->CreateDevice(guid, &di_dev.dev, nullptr);
        if (FAILED(hr)) {
            PT_LOG_WARN("BindingPoller: CreateDevice failed hr={:#010x}", static_cast<unsigned>(hr));
            di_release_instance();
            return;
        }

        di_dev.dev->SetDataFormat(&c_dfDIJoystick2);
        hr = di_dev.dev->SetCooperativeLevel(GetDesktopWindow(),
                                              DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
        if (FAILED(hr))
            PT_LOG_WARN("BindingPoller: SetCooperativeLevel hr={:#010x}", static_cast<unsigned>(hr));

        hr = di_dev.dev->Acquire();
        if (SUCCEEDED(hr)) {
            di_open = true;
            PT_LOG_INFO("BindingPoller: DInput device acquired for btn {}", b.dinput_button_index);
        } else {
            PT_LOG_WARN("BindingPoller: Acquire failed hr={:#010x}", static_cast<unsigned>(hr));
            di_dev.dev->Release();
            di_dev.dev = nullptr;
            di_release_instance();
        }
    }

    bool poll() {
        switch (binding.type) {
            case BindingType::None: return false;
            case BindingType::Keyboard:
                return (GetAsyncKeyState(binding.vk_code) & 0x8000) != 0;
            case BindingType::XInput: {
                XINPUT_STATE xs{};
                if (XInputGetState(binding.xinput_controller, &xs) != ERROR_SUCCESS) return false;
                if (binding.xinput_button_mask & kXiLT) return xs.Gamepad.bLeftTrigger  >= kTriggerThresh;
                if (binding.xinput_button_mask & kXiRT) return xs.Gamepad.bRightTrigger >= kTriggerThresh;
                return (xs.Gamepad.wButtons & binding.xinput_button_mask) != 0;
            }
            case BindingType::DirectInput: {
                if (!di_open || !di_dev.dev) return false;
                di_dev.dev->Poll();
                DIJOYSTATE2 st{};
                HRESULT hr = di_dev.dev->GetDeviceState(sizeof(st), &st);
                if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
                    hr = di_dev.dev->Acquire();
                    if (FAILED(hr)) return false;
                    di_dev.dev->Poll();
                    hr = di_dev.dev->GetDeviceState(sizeof(st), &st);
                }
                if (FAILED(hr)) return false;
                const int idx = binding.dinput_button_index;
                if (idx < 0 || idx >= 128) return false;
                return (st.rgbButtons[idx] & 0x80) != 0;
            }
        }
        return false;
    }

    ~Impl() {
        if (di_open && di_dev.dev) {
            di_dev.dev->Unacquire();
            di_dev.dev->Release();
            di_release_instance();
        }
    }
};

BindingPoller::BindingPoller()  : impl_(std::make_unique<Impl>()) {}
BindingPoller::~BindingPoller() = default;
void BindingPoller::set_binding(const PassthroughBinding& b) { impl_->set(b); }
bool BindingPoller::poll()                                    { return impl_->poll(); }

// ---------------------------------------------------------------------------
// BindingCapturer
//
// Devices are opened when the config GUI opens and kept alive until it closes.
// By the time the user clicks "Set binding", devices have been running for
// seconds and driver state is fully settled.
//
// Device lifetime vs capture session:
//   open_devices()  — called when the config GUI opens
//   start()         — snapshots current state as baseline; begins listening
//   scan()          — WaitForSingleObject(0) + edge diff, called each frame
//   stop()          — ends the capture session, keeps devices open
//   close_devices() — called when the config GUI closes
// ---------------------------------------------------------------------------

struct CapturerDev {
    GUID                  guid;
    IDirectInputDevice8A* dev   = nullptr;
    HANDLE                event = NULL;   // auto-reset Win32 event
    DIJOYSTATE2           mState{};       // baseline state for the current capture session
    std::string           product_name;
};

struct BindingCapturer::Impl {
    bool              active       = false;
    bool              done         = false;
    bool              devices_open = false;
    PassthroughBinding result;

    // Keyboard prev snapshot
    std::array<BYTE, 256> kb_prev{};

    // XInput prev snapshot
    struct XiPrev { bool valid=false; WORD buttons=0; BYTE lt=0; BYTE rt=0; };
    std::array<XiPrev, 4> xi_prev{};

    // DirectInput devices — kept open for the lifetime of the GUI window
    IDirectInput8A*          di = nullptr;
    std::vector<CapturerDev> di_devs;

    // ---- open_devices() ----
    // Opens devices and starts event notification. Devices stay open until
    // close_devices(). Called once when the config window opens.

    void open_devices() {
        if (devices_open) return;

        di = di_acquire_instance();
        if (!di) return;

        ENUM_CTX ctx;
        di->EnumDevices(DI8DEVCLASS_GAMECTRL, enum_joysticks, &ctx, DIEDFL_ATTACHEDONLY);

        for (const GUID& guid : ctx.guids) {
            if (is_xinput_device(di, guid)) continue;

            CapturerDev d;
            d.guid = guid;
            if (FAILED(di->CreateDevice(guid, &d.dev, nullptr))) continue;

            d.dev->SetDataFormat(&c_dfDIJoystick2);
            d.event = CreateEventA(nullptr, FALSE, FALSE, nullptr);  // auto-reset
            if (!d.event) { d.dev->Release(); continue; }
            d.dev->SetEventNotification(d.event);
            d.dev->SetCooperativeLevel(NULL, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE);
            if (FAILED(d.dev->Acquire())) {
                d.dev->SetEventNotification(NULL);
                CloseHandle(d.event); d.dev->Release(); continue;
            }

            DIDEVICEINSTANCEA info{}; info.dwSize = sizeof(info);
            if (SUCCEEDED(d.dev->GetDeviceInfo(&info)))
                d.product_name = info.tszProductName;

            // Initial mState — devices just opened, may not be settled yet.
            // start() will re-snapshot when the user actually clicks Set Binding.
            d.dev->Poll();
            d.dev->GetDeviceState(sizeof(d.mState), &d.mState);

            di_devs.push_back(std::move(d));
        }
        devices_open = true;
    }

    // ---- close_devices() ----

    void close_devices() {
        active = false;
        for (auto& d : di_devs) {
            if (d.dev) {
                d.dev->SetEventNotification(NULL);  // detach before closing handle
                d.dev->Unacquire();
                d.dev->Release();
            }
            if (d.event) CloseHandle(d.event);
        }
        di_devs.clear();
        if (di) { di_release_instance(); di = nullptr; }
        devices_open = false;
    }

    // ---- start() ----
    // Snapshots current device state as the baseline. By the time this is called
    // (user clicked "Set binding"), devices have been open since the GUI opened
    // and driver state is settled.

    void start() {
        done   = false;
        active = true;
        result = {};

        // Keyboard: snapshot current state
        for (int vk = 1; vk < 256; ++vk)
            kb_prev[vk] = (GetAsyncKeyState(vk) & 0x8000) ? 0x80u : 0u;

        // XInput: snapshot current state
        for (int c = 0; c < 4; ++c) {
            auto& p = xi_prev[c]; p = {};
            XINPUT_STATE xs{};
            p.valid = (XInputGetState(c, &xs) == ERROR_SUCCESS);
            if (p.valid) {
                p.buttons = xs.Gamepad.wButtons;
                p.lt      = xs.Gamepad.bLeftTrigger;
                p.rt      = xs.Gamepad.bRightTrigger;
            }
        }

        // DirectInput: re-snapshot baseline from devices that have been running
        // since open_devices(), so driver state is fully settled.
        for (auto& d : di_devs) {
            d.dev->Poll();
            if (FAILED(d.dev->GetDeviceState(sizeof(d.mState), &d.mState)))
                ZeroMemory(&d.mState, sizeof(d.mState));
            // Drain any pending events accumulated since open_devices() so we
            // don't immediately fire on stale rising edges.
            ResetEvent(d.event);
        }
    }

    // ---- scan() ----

    bool scan() {
        if (!active || done) return done;

        // Keyboard
        for (int vk = 1; vk < 256; ++vk) {
            const BYTE cur = (GetAsyncKeyState(vk) & 0x8000) ? 0x80u : 0u;
            const BYTE was = kb_prev[vk];
            kb_prev[vk] = cur;
            if ((cur & 0x80) && !(was & 0x80)) {
                PassthroughBinding b{};
                b.type    = BindingType::Keyboard;
                b.vk_code = vk;
                result = b; done = true; active = false; return true;
            }
        }

        // XInput
        for (int c = 0; c < 4; ++c) {
            XINPUT_STATE xs{};
            const bool ok = (XInputGetState(c, &xs) == ERROR_SUCCESS);
            auto& p = xi_prev[c];
            if (!ok) { p.valid = false; continue; }
            if (!p.valid) {
                p = { true, xs.Gamepad.wButtons,
                      xs.Gamepad.bLeftTrigger, xs.Gamepad.bRightTrigger };
                continue;
            }
            const WORD cur_btn = xs.Gamepad.wButtons;
            const BYTE cur_lt  = xs.Gamepad.bLeftTrigger;
            const BYTE cur_rt  = xs.Gamepad.bRightTrigger;
            const WORD rising  = cur_btn & static_cast<WORD>(~p.buttons);
            if (rising) {
                PassthroughBinding b{};
                b.type               = BindingType::XInput;
                b.xinput_controller  = c;
                b.xinput_button_mask = rising & (~static_cast<unsigned>(rising) + 1u);
                result = b; done = true; active = false; return true;
            }
            if (cur_lt >= kTriggerThresh && p.lt < kTriggerThresh) {
                PassthroughBinding b{};
                b.type               = BindingType::XInput;
                b.xinput_controller  = c;
                b.xinput_button_mask = kXiLT;
                result = b; done = true; active = false; return true;
            }
            if (cur_rt >= kTriggerThresh && p.rt < kTriggerThresh) {
                PassthroughBinding b{};
                b.type               = BindingType::XInput;
                b.xinput_controller  = c;
                b.xinput_button_mask = kXiRT;
                result = b; done = true; active = false; return true;
            }
            p.buttons = cur_btn;
            p.lt      = cur_lt;
            p.rt      = cur_rt;
        }

        // DirectInput — event-driven: WaitForSingleObject(0) returns WAIT_OBJECT_0
        // only when the device has signalled new data.
        for (auto& d : di_devs) {
            if (WaitForSingleObject(d.event, 0) != WAIT_OBJECT_0) continue;

            d.dev->Poll();

            DIJOYSTATE2 newState{};
            if (FAILED(d.dev->GetDeviceState(sizeof(newState), &newState))) continue;

            for (int btn = 0; btn < static_cast<int>(sizeof(newState.rgbButtons)); ++btn) {
                if (d.mState.rgbButtons[btn] != newState.rgbButtons[btn]) {
                    if (newState.rgbButtons[btn] & 0x80) {
                        PassthroughBinding b{};
                        b.type                = BindingType::DirectInput;
                        b.dinput_device_guid  = guid_to_string(d.guid);
                        b.dinput_button_index = btn;
                        b.dinput_device_name  = d.product_name;
                        result = b; done = true; active = false; return true;
                    }
                }
            }
            d.mState = newState;
        }

        return false;
    }

    void stop() { active = false; }
    ~Impl() { close_devices(); }
};

BindingCapturer::BindingCapturer()  : impl_(std::make_unique<Impl>()) {}
BindingCapturer::~BindingCapturer() = default;
void BindingCapturer::open_devices()          { impl_->open_devices(); }
void BindingCapturer::close_devices()         { impl_->close_devices(); }
void BindingCapturer::start()                 { impl_->start(); }
void BindingCapturer::stop()                  { impl_->stop(); }
bool BindingCapturer::scan()                  { return impl_->scan(); }
PassthroughBinding BindingCapturer::captured() const { return impl_->result; }

}  // namespace psvr2pt

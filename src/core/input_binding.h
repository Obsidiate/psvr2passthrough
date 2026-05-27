#pragma once

#include <string>
#include <memory>

namespace psvr2pt {

// Represents a single button binding that can be keyboard, XInput, or DirectInput.
enum class BindingType : int {
    None        = 0,
    Keyboard    = 1,   // Win32 virtual-key code
    XInput      = 2,   // Xbox-compatible gamepad
    DirectInput = 3,   // HOTAS / joystick (any DirectInput device)
};

struct PassthroughBinding {
    BindingType type = BindingType::None;

    // BindingType::Keyboard
    int vk_code = 0;                    // e.g. VK_F3

    // BindingType::XInput
    int      xinput_controller  = 0;    // 0-3
    unsigned xinput_button_mask = 0;    // XINPUT_GAMEPAD_* bitmask

    // BindingType::DirectInput
    std::string dinput_device_guid;     // "{xxxxxxxx-xxxx-...}" string form
    int         dinput_button_index = 0;
    std::string dinput_device_name;     // product name for display only

    bool is_none() const { return type == BindingType::None; }

    // Human-readable label for the GUI (e.g. "F3", "XInput 0 - A", "HOTAS Btn 12").
    std::string display_name() const;

    // Compact JSON object string, e.g. {"type":"keyboard","vk":70}
    std::string to_json() const;
    static PassthroughBinding from_json(const std::string& json_object);
};

// -----------------------------------------------------------------------
// BindingPoller — keeps alive in the layer, polls each frame.
// poll() returns true while the button is currently held.
// -----------------------------------------------------------------------
class BindingPoller {
public:
    BindingPoller();
    ~BindingPoller();

    void set_binding(const PassthroughBinding& b);
    bool poll();   // true = currently pressed

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// -----------------------------------------------------------------------
// BindingCapturer — used by the config GUI to capture a new binding.
//
// Usage:
//   capturer.start();                     // open devices, take baseline snapshot
//   while (!capturer.scan()) Sleep(16);   // wait for a new press
//   auto b = capturer.captured();         // retrieve result
//   capturer.stop();
// -----------------------------------------------------------------------
class BindingCapturer {
public:
    BindingCapturer();
    ~BindingCapturer();

    // Call once when the GUI window opens — opens DInput devices so they are
    // warm before the user ever clicks "Set binding".
    void open_devices();
    // Call once when the GUI window closes.
    void close_devices();

    void start();
    void stop();
    bool scan();                          // returns true when captured
    PassthroughBinding captured() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace psvr2pt

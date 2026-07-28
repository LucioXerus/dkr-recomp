#include "controls.hpp"

#include <array>

namespace dino::input {

// Per-port binding tables. keyboard_input_mappings[port] is unused for
// ports 1..3 by default (the keyboard maps for those ports are empty),
// but the runtime asks each port whether the keyboard can drive it.
// We use a 3-D array: [port][input][binding_slot].
//
// Note: we keep this storage inline (static) rather than as a pointer
// returned from get_input_binding, to match the existing RmlUi binding
// pattern (RmlUi does `&get_input_binding(...)` and stores the
// reference). Per-port storage is allocated at static-init time.
using input_mapping = std::array<InputField, bindings_per_input>;
using input_mapping_array = std::array<input_mapping, static_cast<size_t>(GameInput::COUNT)>;
static std::array<input_mapping_array, MAX_CONTROLLERS> keyboard_input_mappings{};
static std::array<input_mapping_array, MAX_CONTROLLERS> controller_input_mappings{};

// Make the button value array, which maps a button index to its bit field.
#define DEFINE_INPUT(name, value, readable) uint16_t(value##u),
static const std::array n64_button_values = {
    DEFINE_N64_BUTTON_INPUTS()
};
#undef DEFINE_INPUT

// Make the input name array.
#define DEFINE_INPUT(name, value, readable) readable,
static const std::vector<std::string> input_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

// Make the input enum name array.
#define DEFINE_INPUT(name, value, readable) #name,
static const std::vector<std::string> input_enum_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

size_t get_num_inputs() {
    return (size_t)GameInput::COUNT;
}

const std::string& get_input_name(GameInput input) {
    return input_names.at(static_cast<size_t>(input));
}

const std::string& get_input_enum_name(GameInput input) {
    return input_enum_names.at(static_cast<size_t>(input));
}

GameInput get_input_from_enum_name(const std::string_view enum_name) {
    auto find_it = std::find(input_enum_names.begin(), input_enum_names.end(), enum_name);
    if (find_it == input_enum_names.end()) {
        return GameInput::COUNT;
    }

    return static_cast<GameInput>(find_it - input_enum_names.begin());
}

// Per-port accessor (port 0..MAX_CONTROLLERS-1). Out-of-range ports map
// to a static zero binding so out-of-range writes do not corrupt state.
InputField& get_input_binding(int port, GameInput input, size_t binding_index, InputDevice device) {
    if (port < 0 || port >= MAX_CONTROLLERS) {
        static InputField dummy = {};
        return dummy;
    }
    input_mapping_array& device_mappings = (device == InputDevice::Controller) ? controller_input_mappings[port] : keyboard_input_mappings[port];
    input_mapping& cur_input_mapping = device_mappings.at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        return cur_input_mapping[binding_index];
    }
    else {
        static InputField dummy_field = {};
        return dummy_field;
    }
}

void set_input_binding(int port, GameInput input, size_t binding_index, InputDevice device, InputField value) {
    if (port < 0 || port >= MAX_CONTROLLERS) {
        return;
    }
    input_mapping_array& device_mappings = (device == InputDevice::Controller) ? controller_input_mappings[port] : keyboard_input_mappings[port];
    input_mapping& cur_input_mapping = device_mappings.at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        cur_input_mapping[binding_index] = value;
    }
}

// Active-config-port wrappers. These delegate to the per-port accessor
// using the active port selected in the UI. This keeps every existing
// call site working without a port parameter.
InputField& get_input_binding(GameInput input, size_t binding_index, InputDevice device) {
    return get_input_binding(get_active_config_port(), input, binding_index, device);
}

void set_input_binding(GameInput input, size_t binding_index, InputDevice device, InputField value) {
    set_input_binding(get_active_config_port(), input, binding_index, device, value);
}

// get_n64_input for a specific N64 port. Reads the per-port binding
// tables (controller for that port + keyboard if the port allows it) and
// produces the N64 controller packet.
//
// Keyboard handling note: a single physical keyboard can drive any port
// the user wants, but only ports whose keyboard binding table is
// non-empty will respond. By default only P1 has keyboard bindings, so
// the keyboard drives P1. If a user rebinds keyboard to P2..P4 via the
// config UI, those ports will also respond.
bool get_n64_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    uint16_t cur_buttons = 0;
    float cur_x = 0.0f;
    float cur_y = 0.0f;

    // Out-of-range ports: return false so the runtime marks the port as
    // not connected (CHNL_ERR_NORESP).
    if (controller_num < 0 || controller_num >= MAX_CONTROLLERS) {
        return false;
    }

    if (!game_input_disabled()) {
        const auto& kbd = keyboard_input_mappings[controller_num];
        const auto& ctl = controller_input_mappings[controller_num];

        for (size_t i = 0; i < n64_button_values.size(); i++) {
            size_t input_index = (size_t)GameInput::N64_BUTTON_START + i;
            cur_buttons |= get_input_digital(controller_num, kbd[input_index]) ? n64_button_values[i] : 0;
            cur_buttons |= get_input_digital(controller_num, ctl[input_index]) ? n64_button_values[i] : 0;
        }

        float joystick_x = get_input_analog(controller_num, ctl[(size_t)GameInput::X_AXIS_POS])
                        - get_input_analog(controller_num, ctl[(size_t)GameInput::X_AXIS_NEG]);

        float joystick_y = get_input_analog(controller_num, ctl[(size_t)GameInput::Y_AXIS_POS])
                        - get_input_analog(controller_num, ctl[(size_t)GameInput::Y_AXIS_NEG]);

        apply_joystick_deadzone(joystick_x, joystick_y, &joystick_x, &joystick_y);

        cur_x = get_input_analog(controller_num, kbd[(size_t)GameInput::X_AXIS_POS])
                - get_input_analog(controller_num, kbd[(size_t)GameInput::X_AXIS_NEG]) + joystick_x;

        cur_y = get_input_analog(controller_num, kbd[(size_t)GameInput::Y_AXIS_POS])
                - get_input_analog(controller_num, kbd[(size_t)GameInput::Y_AXIS_NEG]) + joystick_y;

        float joystick_range = get_joystick_range() / 100.0f;

        cur_x *= joystick_range;
        cur_y *= joystick_range;
    }

    *buttons_out = cur_buttons;
    *x_out = std::clamp(cur_x, -1.0f, 1.0f);
    *y_out = std::clamp(cur_y, -1.0f, 1.0f);

    return true;
}

}

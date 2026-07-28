#pragma once

#include "input.hpp"

namespace dino::input {
    size_t get_num_inputs();
    const std::string& get_input_name(GameInput input);
    const std::string& get_input_enum_name(GameInput input);
    GameInput get_input_from_enum_name(const std::string_view name);
    // Active-config-port accessors. The UI config page uses these so it
    // does not have to thread a port number through every call site.
    InputField& get_input_binding(GameInput input, size_t binding_index, InputDevice device);
    void set_input_binding(GameInput input, size_t binding_index, InputDevice device, InputField value);
    // Per-port accessors used by the runtime when polling each N64 port.
    // These read and write the per-port binding table directly, NOT the
    // active-config-port view. The active port for editing is managed by
    // get_active_config_port / set_active_config_port in input.hpp.
    InputField& get_input_binding(int port, GameInput input, size_t binding_index, InputDevice device);
    void set_input_binding(int port, GameInput input, size_t binding_index, InputDevice device, InputField value);

    // Per-port game input poll. Called by the runtime for each of the 4
    // N64 ports. Returns true if the port has any controller assigned
    // (even if no buttons are pressed), false otherwise. When false, the
    // runtime reports CHNL_ERR_NORESP for that port and the game treats
    // it as disconnected.
    bool get_n64_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out);
}

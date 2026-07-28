#include "input.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstring>

#include "ultramodern/ultramodern.hpp"

#include "config/config.hpp"
#include "debug_ui/debug_ui.hpp"
#include "promptfont.h"
#include "GamepadMotion.hpp"
#include "ui/recomp_ui.h"

#include "controls.hpp"

constexpr float axis_threshold = 0.5f;

// Per-port SDL gamecontroller slot. Index = port. The runtime fills these as
// SDL emits CONTROLLERDEVICEADDED events. The slot is consumed by
// get_n64_input(port, ...) to produce the N64 controller packet for that
// port. We deliberately avoid reusing the existing "cur_controllers" mutex
// global for per-port semantics; the legacy code already mutates that list
// every poll, so we keep that path intact and only add the parallel
// per-port table.
struct PortControllerSlot {
    SDL_GameController* controller = nullptr;
    SDL_JoystickID instance_id = -1;
    bool assigned = false;
};

struct ControllerState {
    SDL_GameController* controller;
    std::array<float, 3> latest_accelerometer;
    GamepadMotion motion;
    uint32_t prev_gyro_timestamp;
    ControllerState() : controller{}, latest_accelerometer{}, motion{}, prev_gyro_timestamp{} {
        motion.Reset();
        motion.SetCalibrationMode(GamepadMotionHelpers::CalibrationMode::Stillness | GamepadMotionHelpers::CalibrationMode::SensorFusion);
    };
};

static struct {
    const Uint8* keys = nullptr;
    SDL_Keymod keymod = SDL_Keymod::KMOD_NONE;
    int numkeys = 0;
    std::array<std::atomic_uint8_t, SDL_NUM_SCANCODES> key_press_latches{};
    std::atomic_int32_t mouse_wheel_pos = 0;
    std::mutex cur_controllers_mutex;
    std::vector<SDL_GameController*> cur_controllers{};
    std::unordered_map<SDL_JoystickID, ControllerState> controller_states;

    // Per-port slot table. port_controller_slots[port] holds the controller
    // currently assigned to that port, or {.assigned=false} if no controller
    // is plugged into that port. We do not change existing cur_controllers
    // logic (it is still used for analog/button aggregation and rumble);
    // this table is the source of truth for "which physical controller
    // belongs to which N64 port".
    std::array<PortControllerSlot, dino::input::MAX_CONTROLLERS> port_controller_slots{};
    // The next port that should be filled by a newly-attached controller.
    // We use a simple next-port assignment: controllers are added to port
    // 0, then 1, then 2, then 3, in the order they connect. This matches
    // the behavior of every console port the user has ever used: P1 is
    // whoever plugged in first.
    std::atomic_int next_free_port{0};
    // Lock guarding port_controller_slots and next_free_port. The legacy
    // cur_controllers_mutex covers the existing controller_states map; this
    // new mutex covers only the new per-port data so we can avoid mixing
    // responsibilities.
    std::mutex port_mutex;

    std::array<float, 2> rotation_delta{};
    std::array<float, 2> mouse_delta{};
    std::mutex pending_input_mutex;
    std::array<float, 2> pending_rotation_delta{};
    std::array<float, 2> pending_mouse_delta{};

    // Per-port rumble state. The legacy code only supports a single
    // rumble_active flag because there is only one port; we now keep an
    // array so each player can independently trigger their controller's
    // rumble motor.
    float cur_rumble;
    std::array<std::atomic_bool, dino::input::MAX_CONTROLLERS> rumble_active{};
} InputState;

static struct {
    std::list<std::filesystem::path> files_dropped;
} DropState;

std::atomic<dino::input::InputDevice> scanning_device = dino::input::InputDevice::COUNT;
std::atomic<dino::input::InputField> scanned_input;

namespace {
// Active config port. The UI for the controls page consults this when
// reading and writing binding storage. Default is port 0 to match the
// pre-patch behavior for existing users.
std::atomic<int> active_config_port{0};
}

int dino::input::get_active_config_port() {
    int p = active_config_port.load();
    if (p < 0) p = 0;
    if (p >= MAX_CONTROLLERS) p = MAX_CONTROLLERS - 1;
    return p;
}

void dino::input::set_active_config_port(int port) {
    if (port < 0) port = 0;
    if (port >= MAX_CONTROLLERS) port = MAX_CONTROLLERS - 1;
    active_config_port.store(port);
}

void dino::input::cycle_active_config_port() {
    int next = (get_active_config_port() + 1) % MAX_CONTROLLERS;
    set_active_config_port(next);
}

const char* dino::input::get_port_label(int port) {
    if (port < 0 || port >= MAX_CONTROLLERS) return "P?";
    static const char* labels[dino::input::MAX_CONTROLLERS] = {
        "Player 1", "Player 2", "Player 3", "Player 4"
    };
    return labels[port];
}

enum class InputType {
    None = 0, // Using zero for None ensures that default initialized InputFields are unbound.
    Keyboard,
    Mouse,
    ControllerDigital,
    ControllerAnalog // Axis input_id values are the SDL value + 1
};

void set_scanned_input(dino::input::InputField value) {
    scanning_device.store(dino::input::InputDevice::COUNT);
    scanned_input.store(value);
}

dino::input::InputField dino::input::get_scanned_input() {
    dino::input::InputField ret = scanned_input.load();
    scanned_input.store({});
    return ret;
}

void dino::input::start_scanning_input(dino::input::InputDevice device) {
    scanned_input.store({});
    scanning_device.store(device);
}

void dino::input::stop_scanning_input() {
    scanning_device.store(dino::input::InputDevice::COUNT);
}

void queue_if_enabled(SDL_Event* event) {
    if (!dino::input::all_input_disabled()) {
        recompui::queue_event(*event);
    }
}

// Find the port currently assigned to a given SDL instance_id, or -1 if
// none. Caller must hold InputState.port_mutex.
static int find_port_for_instance_locked(SDL_JoystickID instance_id) {
    for (int i = 0; i < dino::input::MAX_CONTROLLERS; i++) {
        if (InputState.port_controller_slots[i].assigned &&
            InputState.port_controller_slots[i].instance_id == instance_id) {
            return i;
        }
    }
    return -1;
}

// Assign a freshly-opened controller to a free port. Returns the port
// index, or -1 if no port is free. Caller must hold InputState.port_mutex.
static int assign_new_port_locked(SDL_GameController* controller, SDL_JoystickID instance_id) {
    // First try to fill the first free slot in order, regardless of next_free_port.
    for (int i = 0; i < dino::input::MAX_CONTROLLERS; i++) {
        if (!InputState.port_controller_slots[i].assigned) {
            InputState.port_controller_slots[i].controller = controller;
            InputState.port_controller_slots[i].instance_id = instance_id;
            InputState.port_controller_slots[i].assigned = true;
            return i;
        }
    }
    return -1;
}

static void open_controller_device(int device_index) {
    if (!SDL_IsGameController(device_index)) {
        return;
    }

    SDL_GameController* controller = SDL_GameControllerOpen(device_index);
    if (controller == nullptr) {
        fprintf(stderr, "Failed to open controller %d: %s\n", device_index, SDL_GetError());
        return;
    }

    SDL_JoystickID instance_id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    {
        std::lock_guard lock{ InputState.cur_controllers_mutex };
        ControllerState& state = InputState.controller_states[instance_id];
        if (state.controller != nullptr) {
            SDL_GameControllerClose(controller);
            return;
        }
        state.controller = controller;
    }

    // Assign a port slot for this controller. We do this even if no port
    // is free; the controller still shows up in the legacy cur_controllers
    // list (used for rumble and the master aggregation path) but won't
    // drive a specific N64 port if there is no slot.
    {
        std::lock_guard plock{ InputState.port_mutex };
        int existing = find_port_for_instance_locked(instance_id);
        if (existing == -1) {
            int assigned = assign_new_port_locked(controller, instance_id);
            if (assigned == -1) {
                fprintf(stderr,
                    "Controller opened: %s (instance %d) but no free N64 ports; not driving a port.\n",
                    SDL_GameControllerName(controller), instance_id);
            } else {
                fprintf(stderr,
                    "Controller opened: %s (instance %d) assigned to N64 port %d\n",
                    SDL_GameControllerName(controller), instance_id, assigned);
            }
        }
    }

    if (SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_GYRO) &&
        SDL_GameControllerHasSensor(controller, SDL_SensorType::SDL_SENSOR_ACCEL)) {
        SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_GYRO, SDL_TRUE);
        SDL_GameControllerSetSensorEnabled(controller, SDL_SensorType::SDL_SENSOR_ACCEL, SDL_TRUE);
    }
}

void dino::input::initialize_controllers() {
    for (int device_index = 0; device_index < SDL_NumJoysticks(); device_index++) {
        open_controller_device(device_index);
    }
}

static std::atomic_bool cursor_enabled = true;

void recompui::set_cursor_visible(bool visible) {
    cursor_enabled.store(visible);
}

bool should_override_keystate(SDL_Scancode key, SDL_Keymod mod) {
    // Override Enter when Alt is held.
    if (key == SDL_Scancode::SDL_SCANCODE_RETURN) {
        if (mod & SDL_Keymod::KMOD_ALT) {
            return true;
        }
    }

    return false;
}

bool sdl_event_filter(void* userdata, SDL_Event* event) {
    switch (event->type) {
    case SDL_EventType::SDL_KEYDOWN:
        {
            SDL_KeyboardEvent* keyevent = &event->key;

            // Skip repeated events when not in the menu
            if (!recompui::is_context_capturing_input() &&
                event->key.repeat) {
                break;
            }

            if (dino::debug_ui::want_capture_keyboard()) {
                break;
            }

            if ((keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_RETURN && (keyevent->keysym.mod & SDL_Keymod::KMOD_ALT)) ||
                keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_F11
            ) {
                recompui::toggle_fullscreen();
            }
            if (scanning_device != dino::input::InputDevice::COUNT) {
                if (keyevent->keysym.scancode == SDL_Scancode::SDL_SCANCODE_ESCAPE) {
                    dino::input::cancel_scanning_input();
                } else if (scanning_device == dino::input::InputDevice::Keyboard) {
                    set_scanned_input({(uint32_t)InputType::Keyboard, keyevent->keysym.scancode});
                }
            } else {
                if (!should_override_keystate(keyevent->keysym.scancode, static_cast<SDL_Keymod>(keyevent->keysym.mod))) {
                    if (!keyevent->repeat && keyevent->keysym.scancode < SDL_NUM_SCANCODES) {
                        // Preserve very short taps until the N64 input poll sees
                        // them. This also makes remote and accessibility input
                        // reliable when press/release events arrive together.
                        InputState.key_press_latches[keyevent->keysym.scancode].store(2, std::memory_order_relaxed);
                    }
                    queue_if_enabled(event);
                }
            }
        }
        break;
    case SDL_EventType::SDL_CONTROLLERDEVICEADDED:
        {
            SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
            printf("Controller added: %d\n", controller_event->which);
            open_controller_device(controller_event->which);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERDEVICEREMOVED:
        {
            SDL_ControllerDeviceEvent* controller_event = &event->cdevice;
            printf("Controller removed: %d\n", controller_event->which);
            {
                std::lock_guard lock{ InputState.cur_controllers_mutex };
                auto controller_it = InputState.controller_states.find(controller_event->which);
                if (controller_it != InputState.controller_states.end()) {
                    if (controller_it->second.controller != nullptr) {
                        std::erase(InputState.cur_controllers, controller_it->second.controller);
                        SDL_GameControllerClose(controller_it->second.controller);
                    }
                    InputState.controller_states.erase(controller_it);
                }
            }
            {
                std::lock_guard plock{ InputState.port_mutex };
                int port = find_port_for_instance_locked(controller_event->which);
                if (port != -1) {
                    InputState.port_controller_slots[port] = PortControllerSlot{};
                    // Reset rumble for the disconnected port.
                    InputState.rumble_active[port].store(false);
                }
            }
        }
        break;
    case SDL_EventType::SDL_QUIT: {
#if defined(__APPLE__)
        // Cocoa closes the native window before SDL delivers SDL_QUIT. Opening
        // an in-game confirmation here leaves a headless process with no way
        // to answer it, so follow normal macOS close/Command-Q behavior.
        ultramodern::quit();
        return true;
#else
        if (!ultramodern::is_game_started()) {
            ultramodern::quit();
            return true;
        }

        dino::config::open_quit_game_prompt();
        recompui::activate_mouse();
        break;
#endif
    }
    case SDL_EventType::SDL_MOUSEWHEEL:
        {
            SDL_MouseWheelEvent* wheel_event = &event->wheel;
            InputState.mouse_wheel_pos.fetch_add(wheel_event->y * (wheel_event->direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1));
        }
        queue_if_enabled(event);
        break;
    case SDL_EventType::SDL_CONTROLLERBUTTONDOWN:
        if (scanning_device != dino::input::InputDevice::COUNT) {
            auto menuToggleBinding0 = dino::input::get_input_binding(dino::input::get_active_config_port(), dino::input::GameInput::TOGGLE_MENU, 0, dino::input::InputDevice::Controller);
            auto menuToggleBinding1 = dino::input::get_input_binding(dino::input::get_active_config_port(), dino::input::GameInput::TOGGLE_MENU, 1, dino::input::InputDevice::Controller);
            // note - magic number: 0 is InputType::None
            if ((menuToggleBinding0.input_type != 0 && event->cbutton.button == menuToggleBinding0.input_id) ||
                (menuToggleBinding1.input_type != 0 && event->cbutton.button == menuToggleBinding1.input_id)) {
                dino::input::cancel_scanning_input();
            } else if (scanning_device == dino::input::InputDevice::Controller) {
                SDL_ControllerButtonEvent* button_event = &event->cbutton;
                auto scanned_input_index = dino::input::get_scanned_input_index();
                if ((scanned_input_index == static_cast<int>(dino::input::GameInput::TOGGLE_MENU) ||
                     scanned_input_index == static_cast<int>(dino::input::GameInput::ACCEPT_MENU) ||
                     scanned_input_index == static_cast<int>(dino::input::GameInput::APPLY_MENU)) && (
                     button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_UP ||
                     button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_DOWN ||
                     button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_LEFT ||
                     button_event->button == SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) {
                    break;
                }

                set_scanned_input({(uint32_t)InputType::ControllerDigital, button_event->button});
            }
        } else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERAXISMOTION:
        if (scanning_device == dino::input::InputDevice::Controller) {
            auto scanned_input_index = dino::input::get_scanned_input_index();
            if (scanned_input_index == static_cast<int>(dino::input::GameInput::TOGGLE_MENU) ||
                scanned_input_index == static_cast<int>(dino::input::GameInput::ACCEPT_MENU) ||
                scanned_input_index == static_cast<int>(dino::input::GameInput::APPLY_MENU)) {
                break;
            }

            SDL_ControllerAxisEvent* axis_event = &event->caxis;
            float axis_value = axis_event->value * (1/32768.0f);
            if (axis_value > axis_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                set_scanned_input({(uint32_t)InputType::ControllerAnalog, axis_event->axis + 1});
            }
            else if (axis_value < -axis_threshold) {
                SDL_Event set_stick_return_event;
                set_stick_return_event.type = SDL_USEREVENT;
                set_stick_return_event.user.code = axis_event->axis;
                set_stick_return_event.user.data1 = nullptr;
                set_stick_return_event.user.data2 = nullptr;
                recompui::queue_event(set_stick_return_event);

                set_scanned_input({(uint32_t)InputType::ControllerAnalog, -axis_event->axis - 1});
            }
        } else {
            queue_if_enabled(event);
        }
        break;
    case SDL_EventType::SDL_CONTROLLERSENSORUPDATE:
        {
            float rot_x = 0.0f;
            float rot_y = 0.0f;
            bool has_rotation_delta = false;

            {
                std::lock_guard lock{ InputState.cur_controllers_mutex };
                auto controller_it = InputState.controller_states.find(event->csensor.which);
                if (controller_it == InputState.controller_states.end() ||
                    controller_it->second.controller == nullptr) {
                    break;
                }

                ControllerState& state = controller_it->second;
                if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_ACCEL) {
                    // Convert acceleration to g's.
                    state.latest_accelerometer[0] = event->csensor.data[0] / SDL_STANDARD_GRAVITY;
                    state.latest_accelerometer[1] = event->csensor.data[1] / SDL_STANDARD_GRAVITY;
                    state.latest_accelerometer[2] = event->csensor.data[2] / SDL_STANDARD_GRAVITY;
                }
                else if (event->csensor.sensor == SDL_SensorType::SDL_SENSOR_GYRO) {
                    // Convert rotational velocity to degrees per second.
                    constexpr float rad_to_deg = 180.0f / M_PI;
                    const float x = event->csensor.data[0] * rad_to_deg;
                    const float y = event->csensor.data[1] * rad_to_deg;
                    const float z = event->csensor.data[2] * rad_to_deg;
                    const uint64_t cur_timestamp = event->csensor.timestamp;
                    const uint32_t delta_ms = cur_timestamp - state.prev_gyro_timestamp;
                    state.motion.ProcessMotion(x, y, z,
                        state.latest_accelerometer[0], state.latest_accelerometer[1],
                        state.latest_accelerometer[2], delta_ms * 0.001f);
                    state.prev_gyro_timestamp = cur_timestamp;
                    state.motion.GetPlayerSpaceGyro(rot_x, rot_y);
                    has_rotation_delta = true;
                }
            }

            if (has_rotation_delta) {
                std::lock_guard lock{ InputState.pending_input_mutex };
                InputState.pending_rotation_delta[0] += rot_x;
                InputState.pending_rotation_delta[1] += rot_y;
            }
        }
        break;
    case SDL_EventType::SDL_MOUSEMOTION:
        if (!dino::input::game_input_disabled()) {
            SDL_MouseMotionEvent* motion_event = &event->motion;
            std::lock_guard lock{ InputState.pending_input_mutex };
            InputState.pending_mouse_delta[0] += motion_event->xrel;
            InputState.pending_mouse_delta[1] += motion_event->yrel;
        }
        queue_if_enabled(event);
        break;
    case SDL_EventType::SDL_DROPBEGIN:
        DropState.files_dropped.clear();
        break;
    case SDL_EventType::SDL_DROPFILE:
        DropState.files_dropped.emplace_back(std::filesystem::path(std::u8string_view((const char8_t *)(event->drop.file))));
        SDL_free(event->drop.file);
        break;
    case SDL_EventType::SDL_DROPCOMPLETE:
        recompui::drop_files(DropState.files_dropped);
        break;
    case SDL_EventType::SDL_CONTROLLERBUTTONUP:
        // Always queue button up events to avoid missing them during binding.
        recompui::queue_event(*event);
        break;
    default:
        queue_if_enabled(event);
        break;
    }
    return false;
}

void dino::input::handle_events() {
    SDL_Event cur_event;
    static bool started = false;
    static bool exited = false;
    while (SDL_PollEvent(&cur_event) && !exited) {
        exited = sdl_event_filter(nullptr, &cur_event);

        // Lock the cursor if all three conditions are true: mouse aiming is enabled, game input is not disabled, and the game has been started.
        bool cursor_locked = (dino::input::get_mouse_sensitivity() != 0) && !dino::input::game_input_disabled() && ultramodern::is_game_started();

        // Hide the cursor based on its enable state, but override visibility to false if the cursor is locked.
        bool cursor_visible = cursor_enabled;
        if (cursor_locked) {
            cursor_visible = false;
        }

        // Restore cursor to the default arrow shape after the UI closes.
        // RmlUi doesn't restore the cursor when contexts are hidden.
        static bool was_ui_shown = true;
        if (was_ui_shown) {
            if (!recompui::is_any_context_shown()) {
                static SDL_Cursor *systemArrowCursor = nullptr;
                if (systemArrowCursor == nullptr) {
                    systemArrowCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
                }

                SDL_SetCursor(systemArrowCursor);
                was_ui_shown = false;
            }
        } else {
            if (recompui::is_any_context_shown()) {
                was_ui_shown = true;
            }
        }

        // If the debug UI is open, let ImGui have control over the cursor.
        if (!dino::debug_ui::is_open()) {
            SDL_ShowCursor(cursor_visible ? SDL_ENABLE : SDL_DISABLE);
        }
        SDL_SetRelativeMouseMode(cursor_locked ? SDL_TRUE : SDL_FALSE);
    }

    if (!started && ultramodern::is_game_started()) {
        started = true;
        recompui::process_game_started();
    }
}

constexpr SDL_GameControllerButton SDL_CONTROLLER_BUTTON_SOUTH = SDL_CONTROLLER_BUTTON_A;
constexpr SDL_GameControllerButton SDL_CONTROLLER_BUTTON_EAST = SDL_CONTROLLER_BUTTON_B;
constexpr SDL_GameControllerButton SDL_CONTROLLER_BUTTON_WEST = SDL_CONTROLLER_BUTTON_X;
constexpr SDL_GameControllerButton SDL_CONTROLLER_BUTTON_NORTH = SDL_CONTROLLER_BUTTON_Y;

const dino::input::DefaultN64Mappings dino::input::default_n64_keyboard_mappings = {
    .a = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_SPACE}
    },
    .b = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_LSHIFT},
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_X}
    },
    .l = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_E}
    },
    .r = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_R}
    },
    .z = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_Q}
    },
    .start = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_RETURN}
    },
    .c_left = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_LEFT}
    },
    .c_right = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_RIGHT}
    },
    .c_up = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_UP}
    },
    .c_down = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_DOWN}
    },
    .dpad_left = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_J}
    },
    .dpad_right = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_L}
    },
    .dpad_up = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_I}
    },
    .dpad_down = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_K}
    },
    .analog_left = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_A}
    },
    .analog_right = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_D}
    },
    .analog_up = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_W}
    },
    .analog_down = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_S}
    },
    .toggle_menu = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_ESCAPE}
    },
    .accept_menu = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_RETURN}
    },
    .apply_menu = {
        {.input_type = (uint32_t)InputType::Keyboard, .input_id = SDL_SCANCODE_F}
    }
};

const dino::input::DefaultN64Mappings dino::input::default_n64_controller_mappings = {
    .a = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_SOUTH},
    },
    .b = {
        // Keep the Xbox face-button layout intuitive: A accelerates/accepts
        // and B brakes/cancels. X remains a convenient secondary B binding
        // for players accustomed to the usual N64-to-modern layout.
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_EAST},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_WEST},
    },
    .l = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    },
    .r = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_TRIGGERRIGHT + 1},
    },
    .z = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_TRIGGERLEFT + 1},
    },
    .start = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_START},
    },
    .c_left = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = -(SDL_CONTROLLER_AXIS_RIGHTX + 1)},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    },
    .c_right = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_RIGHTX + 1},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    },
    .c_up = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = -(SDL_CONTROLLER_AXIS_RIGHTY + 1)},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    },
    .c_down = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_RIGHTY + 1},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    },
    .dpad_left = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    },
    .dpad_right = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    },
    .dpad_up = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_UP},
    },
    .dpad_down = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    },
    .analog_left = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = -(SDL_CONTROLLER_AXIS_LEFTX + 1)},
    },
    .analog_right = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_LEFTX + 1},
    },
    .analog_up = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = -(SDL_CONTROLLER_AXIS_LEFTY + 1)},
    },
    .analog_down = {
        {.input_type = (uint32_t)InputType::ControllerAnalog, .input_id = SDL_CONTROLLER_AXIS_LEFTY + 1},
    },
    .toggle_menu = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_BACK},
    },
    .accept_menu = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_SOUTH},
    },
    .apply_menu = {
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_WEST},
        {.input_type = (uint32_t)InputType::ControllerDigital, .input_id = SDL_CONTROLLER_BUTTON_START}
    }
};

// Per-port defaults. Port 0 keeps the standard Xbox-style gamepad layout.
// Ports 1..3 inherit the same layout by default so that a player who plugs
// in a second controller "just works" without rebinding. If a port has no
// controller plugged in at startup, the runtime will report it as
// disconnected to the game, which the N64 libultra code treats as
// "no controller" (CHNL_ERR_NORESP).
const std::array<dino::input::DefaultN64Mappings, dino::input::MAX_CONTROLLERS>
dino::input::default_n64_controller_mappings_per_port = {{
    dino::input::default_n64_controller_mappings,  // P1: full Xbox layout
    dino::input::default_n64_controller_mappings,  // P2: same defaults
    dino::input::default_n64_controller_mappings,  // P3: same defaults
    dino::input::default_n64_controller_mappings,  // P4: same defaults
}};

// Per-port keyboard defaults. Keyboard input is single-device: all ports
// can listen to it, but P1 is the canonical consumer. P2..P4 default to
// empty so the keyboard doesn't fire on every port unless the player
// explicitly opts in.
const std::array<dino::input::DefaultN64Mappings, dino::input::MAX_CONTROLLERS>
dino::input::default_n64_keyboard_mappings_per_port = {{
    dino::input::default_n64_keyboard_mappings,  // P1: full keyboard
    dino::input::DefaultN64Mappings{},           // P2: empty
    dino::input::DefaultN64Mappings{},           // P3: empty
    dino::input::DefaultN64Mappings{},           // P4: empty
}};

void dino::input::poll_inputs() {
    InputState.keys = SDL_GetKeyboardState(&InputState.numkeys);
    InputState.keymod = SDL_GetModState();

    for (auto &latch : InputState.key_press_latches) {
        uint8_t value = latch.load(std::memory_order_relaxed);
        while ((value > 0) && !latch.compare_exchange_weak(value, value - 1, std::memory_order_relaxed)) {
        }
    }

    {
        std::lock_guard lock{ InputState.cur_controllers_mutex };
        InputState.cur_controllers.clear();

        for (const auto& [id, state] : InputState.controller_states) {
            (void)id; // Avoid unused variable warning.
            SDL_GameController* controller = state.controller;
            if (controller != nullptr) {
                InputState.cur_controllers.push_back(controller);
            }
        }
    }

    // Read the deltas while resetting them to zero.
    {
        std::lock_guard lock{ InputState.pending_input_mutex };

        InputState.rotation_delta = InputState.pending_rotation_delta;
        InputState.pending_rotation_delta = { 0.0f, 0.0f };

        InputState.mouse_delta = InputState.pending_mouse_delta;
        InputState.pending_mouse_delta = { 0.0f, 0.0f };
    }
}

void dino::input::set_rumble(int controller_num, bool on) {
    if (controller_num < 0 || controller_num >= MAX_CONTROLLERS) {
        return;
    }
    InputState.rumble_active[controller_num].store(on);
}

ultramodern::input::connected_device_info_t dino::input::get_connected_device_info(int controller_num) {
    if (controller_num < 0 || controller_num >= MAX_CONTROLLERS) {
        return ultramodern::input::connected_device_info_t {
            .connected_device = ultramodern::input::Device::None,
            .connected_pak = ultramodern::input::Pak::None,
        };
    }

    // Look up the port's controller. We do not hold the lock for long: we
    // only copy the pointer and check assigned.
    bool present = false;
    {
        std::lock_guard lock{ InputState.port_mutex };
        present = InputState.port_controller_slots[controller_num].assigned &&
                  InputState.port_controller_slots[controller_num].controller != nullptr;
    }

    if (present) {
        return ultramodern::input::connected_device_info_t {
            .connected_device = ultramodern::input::Device::Controller,
            .connected_pak = ultramodern::input::Pak::RumblePak,
        };
    }

    return ultramodern::input::connected_device_info_t {
        .connected_device = ultramodern::input::Device::None,
        .connected_pak = ultramodern::input::Pak::None,
    };
}

static float smoothstep(float from, float to, float amount) {
    amount = (amount * amount) * (3.0f - 2.0f * amount);
    return std::lerp(from, to, amount);
}

// Update rumble to attempt to mimic the way n64 rumble ramps up and falls off.
// Each port has its own rumble_active flag, but the global rumble_strength
// setting is shared. We only run the smoothing on ports that are actually
// active to avoid a stuck-at-zero ramp on idle ports.
void dino::input::update_rumble() {
    // Note: values are not accurate! just approximations based on feel
    for (int port = 0; port < MAX_CONTROLLERS; port++) {
        bool active = InputState.rumble_active[port].load();
        if (active) {
            InputState.cur_rumble += 0.17f;
            if (InputState.cur_rumble > 1) InputState.cur_rumble = 1;
        } else {
            InputState.cur_rumble *= 0.92f;
            InputState.cur_rumble -= 0.01f;
            if (InputState.cur_rumble < 0) InputState.cur_rumble = 0;
        }
        float smooth_rumble = smoothstep(0, 1, InputState.cur_rumble);

        uint16_t rumble_strength = smooth_rumble * (dino::input::get_rumble_strength() * 0xFFFF / 100);
        uint32_t duration = 1000000; // Dummy duration value that lasts long enough to matter as the game will reset rumble on its own.

        // Find the controller for this port, if any.
        SDL_GameController* target = nullptr;
        {
            std::lock_guard lock{ InputState.port_mutex };
            if (InputState.port_controller_slots[port].assigned) {
                target = InputState.port_controller_slots[port].controller;
            }
        }
        if (target != nullptr) {
            SDL_GameControllerRumble(target, 0, rumble_strength, duration);
        }
    }
}

// Per-port button query. Looks up the controller for the requested port
// and reads the requested button. Returns false if the port has no
// controller assigned. We do NOT fall back to cur_controllers here: each
// port is bound to exactly one physical controller, and mixing the
// aggregation logic with the per-port logic would let an unplugged P1
// still drive P2 from a different controller. This is the same property
// the N64 hardware enforces.
static bool controller_button_state_for_port(int port, int32_t input_id) {
    if (port < 0 || port >= dino::input::MAX_CONTROLLERS) return false;
    if (input_id < 0 || input_id >= SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_MAX) return false;
    SDL_GameController* target = nullptr;
    {
        std::lock_guard lock{ InputState.port_mutex };
        if (InputState.port_controller_slots[port].assigned) {
            target = InputState.port_controller_slots[port].controller;
        }
    }
    if (target == nullptr) return false;
    return SDL_GameControllerGetButton(target, (SDL_GameControllerButton)input_id);
}

// Legacy aggregator: ANY controller presses the button. This is the path
// the existing single-player UI uses; we keep it so the controls menu
// itself remains responsive to whichever controller is currently active
// (since the UI does not have a port concept yet).
static bool controller_button_state(int32_t input_id) {
    if (input_id >= 0 && input_id < SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_MAX) {
        SDL_GameControllerButton button = (SDL_GameControllerButton)input_id;
        bool ret = false;
        {
            std::lock_guard lock{ InputState.cur_controllers_mutex };
            for (const auto& controller : InputState.cur_controllers) {
                ret |= SDL_GameControllerGetButton(controller, button);
            }
        }

        return ret;
    }
    return false;
}

static std::atomic_bool right_analog_suppressed = false;

// Per-port analog query. Returns 0..1 from the controller assigned to the
// given port, treating signed axes via the input_id sign. Returns 0 if
// the port has no controller.
static float controller_axis_state_for_port(int port, int32_t input_id, bool allow_suppression) {
    if (port < 0 || port >= dino::input::MAX_CONTROLLERS) return 0.0f;
    if (abs(input_id) - 1 < 0 || abs(input_id) - 1 >= SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_MAX) return 0.0f;
    SDL_GameControllerAxis axis = (SDL_GameControllerAxis)(abs(input_id) - 1);
    bool negative_range = input_id < 0;
    SDL_GameController* target = nullptr;
    {
        std::lock_guard lock{ InputState.port_mutex };
        if (InputState.port_controller_slots[port].assigned) {
            target = InputState.port_controller_slots[port].controller;
        }
    }
    if (target == nullptr) return 0.0f;

    float cur_val = SDL_GameControllerGetAxis(target, axis) * (1/32768.0f);
    if (negative_range) {
        cur_val = -cur_val;
    }

    // Check if this input is a right analog axis and suppress it accordingly.
    if (allow_suppression && right_analog_suppressed.load() &&
        (axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY)) {
        cur_val = 0;
    }
    return std::clamp(cur_val, 0.0f, 1.0f);
}

float controller_axis_state(int32_t input_id, bool allow_suppression) {
    if (abs(input_id) - 1 < SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_MAX) {
        SDL_GameControllerAxis axis = (SDL_GameControllerAxis)(abs(input_id) - 1);
        bool negative_range = input_id < 0;
        float ret = 0.0f;

        {
            std::lock_guard lock{ InputState.cur_controllers_mutex };
            for (const auto& controller : InputState.cur_controllers) {
                float cur_val = SDL_GameControllerGetAxis(controller, axis) * (1/32768.0f);
                if (negative_range) {
                    cur_val = -cur_val;
                }

                // Check if this input is a right analog axis and suppress it accordingly.
                if (allow_suppression && right_analog_suppressed.load() &&
                    (axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY)) {
                    cur_val = 0;
                }
                ret += std::clamp(cur_val, 0.0f, 1.0f);
            }
        }

        return std::clamp(ret, 0.0f, 1.0f);
    }
    return false;
}

// Per-port input dispatch. Port-aware versions of get_input_analog and
// get_input_digital. The runtime calls these from get_n64_input for each
// of the 4 N64 ports.
float dino::input::get_input_analog(int port, const dino::input::InputField& field) {
    switch ((InputType)field.input_type) {
    case InputType::Keyboard:
        if (InputState.keys && field.input_id >= 0 && field.input_id < InputState.numkeys) {
            if (should_override_keystate(static_cast<SDL_Scancode>(field.input_id), InputState.keymod)) {
                return 0.0f;
            }
            return (InputState.keys[field.input_id] ||
                InputState.key_press_latches[field.input_id].load(std::memory_order_relaxed)) ? 1.0f : 0.0f;
        }
        return 0.0f;
    case InputType::ControllerDigital:
        return controller_button_state_for_port(port, field.input_id) ? 1.0f : 0.0f;
    case InputType::ControllerAnalog:
        return controller_axis_state_for_port(port, field.input_id, true);
    case InputType::Mouse:
        return 0.0f;
    case InputType::None:
        return 0.0f;
    }
    return 0.0f;
}

float dino::input::get_input_analog(int port, const std::span<const dino::input::InputField> fields) {
    float ret = 0.0f;
    for (const auto& field : fields) {
        ret += get_input_analog(port, field);
    }
    return std::clamp(ret, 0.0f, 1.0f);
}

bool dino::input::get_input_digital(int port, const dino::input::InputField& field) {
    switch ((InputType)field.input_type) {
    case InputType::Keyboard:
        if (InputState.keys && field.input_id >= 0 && field.input_id < InputState.numkeys) {
            if (should_override_keystate(static_cast<SDL_Scancode>(field.input_id), InputState.keymod)) {
                return false;
            }
            return (InputState.keys[field.input_id] != 0) ||
                (InputState.key_press_latches[field.input_id].load(std::memory_order_relaxed) != 0);
        }
        return false;
    case InputType::ControllerDigital:
        return controller_button_state_for_port(port, field.input_id);
    case InputType::ControllerAnalog:
        return controller_axis_state_for_port(port, field.input_id, true) >= axis_threshold;
    case InputType::Mouse:
        return false;
    case InputType::None:
        return false;
    }
    return false;
}

bool dino::input::get_input_digital(int port, const std::span<const dino::input::InputField> fields) {
    bool ret = false;
    for (const auto& field : fields) {
        ret |= get_input_digital(port, field);
    }
    return ret;
}

// Backward-compatible wrappers. The legacy code base calls these for UI
// purposes (where any-controller aggregation is the right behavior). For
// game input, the runtime uses the per-port variants above via controls.cpp.
float dino::input::get_input_analog(const dino::input::InputField& field) {
    // Aggregate across all active ports. This keeps the existing controls
    // menu and nav-help bindings responsive regardless of which port the
    // user is editing. Game input uses get_input_analog(int port, ...).
    float best = 0.0f;
    for (int port = 0; port < MAX_CONTROLLERS; port++) {
        best = std::max(best, get_input_analog(port, field));
    }
    return best;
}

float dino::input::get_input_analog(const std::span<const dino::input::InputField> fields) {
    float ret = 0.0f;
    for (const auto& field : fields) {
        ret = std::max(ret, get_input_analog(field));
    }
    return std::clamp(ret, 0.0f, 1.0f);
}

bool dino::input::get_input_digital(const dino::input::InputField& field) {
    switch ((InputType)field.input_type) {
    case InputType::Keyboard:
        if (InputState.keys && field.input_id >= 0 && field.input_id < InputState.numkeys) {
            if (should_override_keystate(static_cast<SDL_Scancode>(field.input_id), InputState.keymod)) {
                return false;
            }
            return (InputState.keys[field.input_id] != 0) ||
                (InputState.key_press_latches[field.input_id].load(std::memory_order_relaxed) != 0);
        }
        return false;
    case InputType::ControllerDigital:
        return controller_button_state(field.input_id);
    case InputType::ControllerAnalog:
        return controller_axis_state(field.input_id, true) >= axis_threshold;
    case InputType::Mouse:
        return false;
    case InputType::None:
        return false;
    }
    return false;
}

bool dino::input::get_input_digital(const std::span<const dino::input::InputField> fields) {
    bool ret = 0;
    for (const auto& field : fields) {
        ret |= get_input_digital(field);
    }
    return ret;
}

void dino::input::get_gyro_deltas(float* x, float* y) {
    std::array<float, 2> cur_rotation_delta = InputState.rotation_delta;
    float sensitivity = (float)dino::input::get_gyro_sensitivity() / 100.0f;
    *x = cur_rotation_delta[0] * sensitivity;
    *y = cur_rotation_delta[1] * sensitivity;
}

void dino::input::get_mouse_deltas(float* x, float* y) {
    std::array<float, 2> cur_mouse_delta = InputState.mouse_delta;
    float sensitivity = (float)dino::input::get_mouse_sensitivity() / 100.0f;
    *x = cur_mouse_delta[0] * sensitivity;
    *y = cur_mouse_delta[1] * sensitivity;
}

void dino::input::apply_joystick_deadzone(float x_in, float y_in, float* x_out, float* y_out) {
    float joystick_deadzone = (float)dino::input::get_joystick_deadzone() / 100.0f;

    if(fabsf(x_in) < joystick_deadzone) {
        x_in = 0.0f;
    }
    else {
        if(x_in > 0.0f) {
            x_in -= joystick_deadzone;
        }
        else {
            x_in += joystick_deadzone;
        }

        x_in /= (1.0f - joystick_deadzone);
    }

    if(fabsf(y_in) < joystick_deadzone) {
        y_in = 0.0f;
    }
    else {
        if(y_in > 0.0f) {
            y_in -= joystick_deadzone;
        }
        else {
            y_in += joystick_deadzone;
        }

        y_in /= (1.0f - joystick_deadzone);
    }

    *x_out = x_in;
    *y_out = y_in;
}

void dino::input::get_right_analog(float* x, float* y) {
    // Right analog: sum across all active ports, then deadzone. This is
    // shared state (the N64 has only one C-stick).
    float x_val =
        controller_axis_state((SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX + 1), false) -
        controller_axis_state(-(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX + 1), false);
    float y_val =
        controller_axis_state((SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY + 1), false) -
        controller_axis_state(-(SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY + 1), false);
    dino::input::apply_joystick_deadzone(x_val, y_val, x, y);
}

void dino::input::set_right_analog_suppressed(bool suppressed) {
    right_analog_suppressed.store(suppressed);
}

bool dino::input::game_input_disabled() {
    // Disable input if any menu that blocks input is open.
    return recompui::is_context_capturing_input() || dino::debug_ui::want_capture_keyboard();
}

bool dino::input::all_input_disabled() {
    // Disable all input if an input is being polled.
    return scanning_device != dino::input::InputDevice::COUNT;
}

std::string controller_button_to_string(SDL_GameControllerButton button) {
    switch (button) {
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_A:
        return PF_GAMEPAD_A;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_B:
        return PF_GAMEPAD_B;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_X:
        return PF_GAMEPAD_X;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_Y:
        return PF_GAMEPAD_Y;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_BACK:
        return PF_XBOX_VIEW;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_GUIDE:
        return PF_GAMEPAD_HOME;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_START:
        return PF_XBOX_MENU;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_LEFTSTICK:
        return PF_ANALOG_L_CLICK;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_RIGHTSTICK:
        return PF_ANALOG_R_CLICK;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
        return PF_XBOX_LEFT_SHOULDER;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
        return PF_XBOX_RIGHT_SHOULDER;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_UP:
        return PF_DPAD_UP;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        return PF_DPAD_DOWN;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        return PF_DPAD_LEFT;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        return PF_DPAD_RIGHT;
    case SDL_GameControllerButton::SDL_CONTROLLER_BUTTON_TOUCHPAD:
        return PF_SONY_TOUCHPAD;
    default:
        return "Button " + std::to_string(button);
    }
}

std::unordered_map<SDL_Scancode, std::string> scancode_codepoints {
    {SDL_SCANCODE_LEFT, PF_KEYBOARD_LEFT},
    // NOTE: UP and RIGHT are swapped with promptfont.
    {SDL_SCANCODE_UP, PF_KEYBOARD_RIGHT},
    {SDL_SCANCODE_RIGHT, PF_KEYBOARD_UP},
    {SDL_SCANCODE_DOWN, PF_KEYBOARD_DOWN},
    {SDL_SCANCODE_A, PF_KEYBOARD_A},
    {SDL_SCANCODE_B, PF_KEYBOARD_B},
    {SDL_SCANCODE_C, PF_KEYBOARD_C},
    {SDL_SCANCODE_D, PF_KEYBOARD_D},
    {SDL_SCANCODE_E, PF_KEYBOARD_E},
    {SDL_SCANCODE_F, PF_KEYBOARD_F},
    {SDL_SCANCODE_G, PF_KEYBOARD_G},
    {SDL_SCANCODE_H, PF_KEYBOARD_H},
    {SDL_SCANCODE_I, PF_KEYBOARD_I},
    {SDL_SCANCODE_J, PF_KEYBOARD_J},
    {SDL_SCANCODE_K, PF_KEYBOARD_K},
    {SDL_SCANCODE_L, PF_KEYBOARD_L},
    {SDL_SCANCODE_M, PF_KEYBOARD_M},
    {SDL_SCANCODE_N, PF_KEYBOARD_N},
    {SDL_SCANCODE_O, PF_KEYBOARD_O},
    {SDL_SCANCODE_P, PF_KEYBOARD_P},
    {SDL_SCANCODE_Q, PF_KEYBOARD_Q},
    {SDL_SCANCODE_R, PF_KEYBOARD_R},
    {SDL_SCANCODE_S, PF_KEYBOARD_S},
    {SDL_SCANCODE_T, PF_KEYBOARD_T},
    {SDL_SCANCODE_U, PF_KEYBOARD_U},
    {SDL_SCANCODE_V, PF_KEYBOARD_V},
    {SDL_SCANCODE_W, PF_KEYBOARD_W},
    {SDL_SCANCODE_X, PF_KEYBOARD_X},
    {SDL_SCANCODE_Y, PF_KEYBOARD_Y},
    {SDL_SCANCODE_Z, PF_KEYBOARD_Z},
    {SDL_SCANCODE_0, PF_KEYBOARD_0},
    {SDL_SCANCODE_1, PF_KEYBOARD_1},
    {SDL_SCANCODE_2, PF_KEYBOARD_2},
    {SDL_SCANCODE_3, PF_KEYBOARD_3},
    {SDL_SCANCODE_4, PF_KEYBOARD_4},
    {SDL_SCANCODE_5, PF_KEYBOARD_5},
    {SDL_SCANCODE_6, PF_KEYBOARD_6},
    {SDL_SCANCODE_7, PF_KEYBOARD_7},
    {SDL_SCANCODE_8, PF_KEYBOARD_8},
    {SDL_SCANCODE_9, PF_KEYBOARD_9},
    {SDL_SCANCODE_ESCAPE, PF_KEYBOARD_ESCAPE},
    {SDL_SCANCODE_F1, PF_KEYBOARD_F1},
    {SDL_SCANCODE_F2, PF_KEYBOARD_F2},
    {SDL_SCANCODE_F3, PF_KEYBOARD_F3},
    {SDL_SCANCODE_F4, PF_KEYBOARD_F4},
    {SDL_SCANCODE_F5, PF_KEYBOARD_F5},
    {SDL_SCANCODE_F6, PF_KEYBOARD_F6},
    {SDL_SCANCODE_F7, PF_KEYBOARD_F7},
    {SDL_SCANCODE_F8, PF_KEYBOARD_F8},
    {SDL_SCANCODE_F9, PF_KEYBOARD_F9},
    {SDL_SCANCODE_F10, PF_KEYBOARD_F10},
    {SDL_SCANCODE_F11, PF_KEYBOARD_F11},
    {SDL_SCANCODE_F12, PF_KEYBOARD_F12},
    {SDL_SCANCODE_PRINTSCREEN, PF_KEYBOARD_PRINT_SCREEN},
    {SDL_SCANCODE_SCROLLLOCK, PF_KEYBOARD_SCROLL_LOCK},
    {SDL_SCANCODE_PAUSE, PF_KEYBOARD_PAUSE},
    {SDL_SCANCODE_INSERT, PF_KEYBOARD_INSERT},
    {SDL_SCANCODE_HOME, PF_KEYBOARD_HOME},
    {SDL_SCANCODE_PAGEUP, PF_KEYBOARD_PAGE_UP},
    {SDL_SCANCODE_DELETE, PF_KEYBOARD_DELETE},
    {SDL_SCANCODE_END, PF_KEYBOARD_END},
    {SDL_SCANCODE_PAGEDOWN, PF_KEYBOARD_PAGE_DOWN},
    {SDL_SCANCODE_SPACE, PF_KEYBOARD_SPACE},
    {SDL_SCANCODE_BACKSPACE, PF_KEYBOARD_BACKSPACE},
    {SDL_SCANCODE_TAB, PF_KEYBOARD_TAB},
    {SDL_SCANCODE_RETURN, PF_KEYBOARD_ENTER},
    {SDL_SCANCODE_CAPSLOCK, PF_KEYBOARD_CAPS},
    {SDL_SCANCODE_NUMLOCKCLEAR, PF_KEYBOARD_NUM_LOCK},
    {SDL_SCANCODE_LSHIFT, "L" PF_KEYBOARD_SHIFT},
    {SDL_SCANCODE_RSHIFT, "R" PF_KEYBOARD_SHIFT},
};

std::string keyboard_input_to_string(SDL_Scancode key) {
    if (scancode_codepoints.find(key) != scancode_codepoints.end()) {
        return scancode_codepoints[key];
    }
    return std::to_string(key);
}

std::string controller_axis_to_string(int axis) {
    bool positive = axis > 0;
    SDL_GameControllerAxis actual_axis = SDL_GameControllerAxis(abs(axis) - 1);
    switch (actual_axis) {
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_LEFTX:
        return positive ? "\u21C0" : "\u21BC";
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_LEFTY:
        return positive ? "\u21C2" : "\u21BE";
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTX:
        return positive ? "\u21C1" : "\u21BD";
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_RIGHTY:
        return positive ? "\u21C3" : "\u21BF";
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_TRIGGERLEFT:
        return positive ? "\u2196" : "\u21DC";
    case SDL_GameControllerAxis::SDL_CONTROLLER_AXIS_TRIGGERRIGHT:
        return positive ? "\u2197" : "\u21DD";
    default:
        return "Axis " + std::to_string(actual_axis) + (positive ? '+' : '-');
    }
}

std::string dino::input::InputField::to_string() const {
    switch ((InputType)input_type) {
        case InputType::None:
            return "";
        case InputType::ControllerDigital:
            return controller_button_to_string((SDL_GameControllerButton)input_id);
        case InputType::ControllerAnalog:
            return controller_axis_to_string(input_id);
        case InputType::Keyboard:
            return keyboard_input_to_string((SDL_Scancode)input_id);
        default:
            return std::to_string(input_type) + "," + std::to_string(input_id);
    }
}

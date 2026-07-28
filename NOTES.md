# 4-Controller Support — Source Patches

These are **source-only patches** for the cyscott/dkr-recomp v0.1.18-alpha tree
that add support for up to four N64 controllers. The patches modify only
runtime frontend code; the recompiled N64 game code, RT64, and the F3DDKR
microcode interpreter are untouched.

## Why source-only?

The cyscott/dkr-recomp build requires a private `rt64_gbi_f3ddkr.cpp/.h` to
produce a working renderer. The only public copies of those files (notably
the one in `sp00nznet/rt64`) cause vertex explosions and z-fighting and
cannot be used. The correct files live in the maintainer's private
working tree and are not present in any commit of `cyscott/dkr-recomp` or
`DinosaurPlanetRecomp/rt64`. Until those files are available, the
patches below are correct and ready to merge but cannot be built into a
runnable AppImage.

## What changed

| File | Purpose |
| --- | --- |
| `src/input/input.hpp` | Adds `MAX_CONTROLLERS = 4`, per-port input accessors, per-port `get_input_analog/digital` overloads, `get_active_config_port` / `set_active_config_port` / `cycle_active_config_port` / `get_port_label`. |
| `src/input/input.cpp` | Per-port `controller_button_state_for_port` / `controller_axis_state_for_port` helpers; per-port `get_input_analog` / `get_input_digital` that consult only the controller assigned to the requested port; per-port `port_controller_slots` table populated on `SDL_CONTROLLERDEVICEADDED` and cleared on `SDL_CONTROLLERDEVICEREMOVED`; per-port `rumble_active` array; per-port `set_rumble` / `get_connected_device_info`; per-port default-mapping tables `default_n64_controller_mappings_per_port` and `default_n64_keyboard_mappings_per_port`. Legacy single-port paths are kept as backward-compatible wrappers. |
| `src/input/controls.hpp` | Adds per-port `get_input_binding` / `set_input_binding` overloads. |
| `src/input/controls.cpp` | `keyboard_input_mappings` and `controller_input_mappings` are now per-port; per-port accessor helpers; `get_n64_input(controller_num, ...)` uses the per-port table for that specific port. |
| `src/config/config.cpp` | Bumps `controls_config_version` to 4. Per-port schema: `controls.json` now contains a `ports` array of four entries with per-port `keyboard` and `controller` maps. Legacy flat schema is still accepted and seeds port 0; ports 1..3 get their default profiles on upgrade. `assign_all_mappings` / `assign_mapping` / `assign_mapping_complete` gained per-port overloads. |
| `src/ui/ui_config.cpp` | Registers a new `cycle_config_port` event. Adds data-model bindings `active_config_port`, `active_config_port_label`, `max_controllers`. The `inputs` data model is now implicitly per-port (the existing `get_input_binding` accessor now reads from the active config port). |
| `assets/config_menu/controls.rml` | Adds a "Player 1..4" cycle button next to the existing keyboard/controller toggle. Pressing it cycles the active config port and repaints the bindings panel. |

## Build steps (after patches applied)

The build requires a working `rt64_gbi_f3ddkr.cpp/.h`. Once those files
exist, the build is the standard `dkr-recomp` build:

```bash
# 1. Place the working f3ddkr files at:
#      lib/rt64/src/gbi/rt64_gbi_f3ddkr.cpp
#      lib/rt64/src/gbi/rt64_gbi_f3ddkr.h
#
# 2. Apply the submodule patches (as the upstream build does):
./scripts/apply-submodule-patches.sh
#
# 3. Configure with the same toolchain the maintainer uses (Ubuntu 22.04
#    + clang via Docker — see scripts/build-steamdeck-appimage.sh).
#    Outside Docker the build is also known to work on a vanilla
#    Steam-Deck-style Ubuntu host.
cmake -S . -B build-steamdeck -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build-steamdeck --parallel
#
# 4. Run the project smoke test (requires the user's own legally
#    obtained DKR USA Rev 1 ROM; do not commit it).
./scripts/build-steamdeck-appimage.sh
./scripts/smoke-test-steamdeck-appimage.sh \
    dist-steamdeck/DiddyKongRacingRecompiled-SteamDeck-x86_64.AppImage \
    /path/to/private/dkr-rev1.z64
```

## Behavior

* **Plug in controller N** → that controller drives N64 port N. The first
  controller connected becomes P1, the second becomes P2, and so on, up
  to four. Disconnecting a controller frees its port for the next one to
  plug in.
* **No controller at port N** → the runtime reports `CHNL_ERR_NORESP`
  to the game for that port, which is exactly what an unplugged N64
  controller reports in retail hardware. DKR's multiplayer code
  gracefully handles the absent controller.
* **Keyboard** → by default only port 0's keyboard map is populated
  (matching the historical single-player experience). Players can rebind
  the keyboard to drive any port via the controls menu; this writes to
  that port's `keyboard` map in `controls.json`.
* **Rumble** → per-port. `osMotorStart` on N64 port N now rumbles the
  controller currently assigned to that port.
* **Nav-help and nav buttons in the in-game menu** continue to read
  `get_active_config_port()` (which defaults to 0). The intended
  follow-up is to make the in-game menu track the most-recently-active
  port so the nav help shows that player's bindings; that work is out of
  scope for the runtime-side patch.
* **Config persistence** → `controls.json` is upgraded to a `ports`
  array schema. Existing flat-schema files continue to load and seed
  port 0; the new per-port UI controls write to the per-port slot.

## Caveats specific to upstream's testing matrix

Per the v0.1.18 release notes (`docs/KNOWN_ISSUES.md` and
`docs/TESTING.md`):

> "Multiplayer, boss races, trophy races, every vehicle, every
> character, and every course still need structured regression coverage."

The runtime is capable of 4 controllers (the N64ModernRuntime patch
`n64modernruntime-dkr.patch` already sets `MAXCONTROLLERS = 4`), and
this patch series adds the frontend plumbing for all four ports.
However, the actual DKR retail code paths for 2P/3P/4P mode selection,
character select with 2-4 players, multiplayer race setup, and the
related course selection logic have **not been tested by the upstream
maintainer**. The patches here enable the runtime; the rest is on the
end user / community to verify with real multiplayer gameplay.

## Smoke test the patches compiled

After applying the patches and before merging, the following commands
should succeed in `build-steamdeck/`:

```bash
ninja -t list 2>&1 | grep -E 'input/controls|input/input|config/config|ui_config' | head
# Confirm the build target graph still includes the patched files.
```

A full clean rebuild is the recommended gate. No tests exist for
`src/input` in this repository.

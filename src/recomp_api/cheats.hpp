#pragma once

#include "recomp.h"

namespace dino::cheats {
    // No-op placeholder. The unlock progression cheats were removed because
    // they required hooking the recomp's recompiled set_magic_code_flags
    // (which never fired reliably from init_title_screen_variables) and
    // patching the EEPROM read path (which caused heap corruption). The
    // Music 3P/4P and High LOD cheats are implemented as instruction
    // patches in dkr.us.v80.toml; their UI toggles are in
    // src/ui/ui_config.cpp and the recomp exposes no per-frame hook point
    // for them, so the toggles are label-only for now.
}

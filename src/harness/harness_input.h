#pragma once

#include <cstdint>

// Harness input injection. Each function acquires wlserver_lock, calls the
// appropriate wlserver_* function, and releases it. Callers must NOT hold
// wlserver_lock on entry.
//
// vk_code is a Win32 virtual-key code (e.g. VK_F1 = 0x70).
// Returns false if vk_code has no known evdev mapping.

namespace gamescope::Harness
{

bool HarnessKeyDown( uint32_t vk_code );
bool HarnessKeyUp( uint32_t vk_code );

// Press then release with ~KEY_PRESS_DELAY_MS between them.
bool HarnessKey( uint32_t vk_code );

bool HarnessMove( double x, double y );

// Warp to (x,y) then press + release BTN_LEFT with ~KEY_PRESS_DELAY_MS between.
bool HarnessClick( double x, double y );

} // namespace gamescope::Harness

// Win32 VK -> Linux evdev keycode mapping and wlserver input injection.
//
// The mapping assumes a US-ANSI keyboard layout. wlserver_key() delivers
// raw evdev scancodes; wlroots applies the XKB keymap afterwards (see
// wlserver.cpp:1978). Enforce XKB_DEFAULT_LAYOUT=us in the launch wrapper
// to prevent layout-dependent mismatches (OQ-3).

#include "harness_input.h"

#include "../steamcompmgr.hpp" // get_time_in_milliseconds
#include "../wlserver.hpp"    // wlserver_lock/unlock, wlserver_key etc.

#include <linux/input-event-codes.h>
#include <cstdint>
#include <cstring>
#include <unistd.h>   // usleep

// Match the DLL convention from tools/aoe3_harness/dll/anw_hook.c.
static constexpr int KEY_PRESS_DELAY_MS = 30;

namespace gamescope::Harness
{

// ---------------------------------------------------------------------------
// VK -> evdev table
// ---------------------------------------------------------------------------
//
// Win32 VK codes are documented at:
//   https://learn.microsoft.com/en-us/windows/win32/inputdev/virtual-key-codes
//
// evdev scancodes are from <linux/input-event-codes.h>. The two namespaces
// are NOT the same and must not be confused.

struct VkEvdevEntry
{
    uint32_t vk;
    uint32_t evdev;
};

// Sorted by VK for readability; lookup is a linear scan (table is small and
// called at most a few times per second from the harness thread).
static constexpr VkEvdevEntry k_vkTable[] = {
    // Control characters
    { 0x08, KEY_BACKSPACE },  // VK_BACK
    { 0x09, KEY_TAB },        // VK_TAB
    { 0x0D, KEY_ENTER },      // VK_RETURN
    { 0x10, KEY_LEFTSHIFT },  // VK_SHIFT (left variant)
    { 0x11, KEY_LEFTCTRL },   // VK_CONTROL (left variant)
    { 0x12, KEY_LEFTALT },    // VK_MENU (left variant)
    { 0x13, KEY_PAUSE },      // VK_PAUSE
    { 0x1B, KEY_ESC },        // VK_ESCAPE
    { 0x20, KEY_SPACE },      // VK_SPACE
    { 0x21, KEY_PAGEUP },     // VK_PRIOR
    { 0x22, KEY_PAGEDOWN },   // VK_NEXT
    { 0x23, KEY_END },        // VK_END
    { 0x24, KEY_HOME },       // VK_HOME
    { 0x25, KEY_LEFT },       // VK_LEFT
    { 0x26, KEY_UP },         // VK_UP
    { 0x27, KEY_RIGHT },      // VK_RIGHT
    { 0x28, KEY_DOWN },       // VK_DOWN
    { 0x2C, KEY_SYSRQ },      // VK_SNAPSHOT (Print Screen)
    { 0x2D, KEY_INSERT },     // VK_INSERT
    { 0x2E, KEY_DELETE },     // VK_DELETE

    // Digits 0-9 (main row)
    { 0x30, KEY_0 },
    { 0x31, KEY_1 },
    { 0x32, KEY_2 },
    { 0x33, KEY_3 },
    { 0x34, KEY_4 },
    { 0x35, KEY_5 },
    { 0x36, KEY_6 },
    { 0x37, KEY_7 },
    { 0x38, KEY_8 },
    { 0x39, KEY_9 },

    // Letters A-Z (ANSI layout — evdev scancodes do not follow ASCII order)
    { 0x41, KEY_A },
    { 0x42, KEY_B },
    { 0x43, KEY_C },
    { 0x44, KEY_D },
    { 0x45, KEY_E },
    { 0x46, KEY_F },
    { 0x47, KEY_G },
    { 0x48, KEY_H },
    { 0x49, KEY_I },
    { 0x4A, KEY_J },
    { 0x4B, KEY_K },
    { 0x4C, KEY_L },
    { 0x4D, KEY_M },
    { 0x4E, KEY_N },
    { 0x4F, KEY_O },
    { 0x50, KEY_P },
    { 0x51, KEY_Q },
    { 0x52, KEY_R },
    { 0x53, KEY_S },
    { 0x54, KEY_T },
    { 0x55, KEY_U },
    { 0x56, KEY_V },
    { 0x57, KEY_W },
    { 0x58, KEY_X },
    { 0x59, KEY_Y },
    { 0x5A, KEY_Z },

    // Win / Super keys
    { 0x5B, KEY_LEFTMETA },   // VK_LWIN
    { 0x5C, KEY_RIGHTMETA },  // VK_RWIN

    // Numpad
    { 0x60, KEY_KP0 },
    { 0x61, KEY_KP1 },
    { 0x62, KEY_KP2 },
    { 0x63, KEY_KP3 },
    { 0x64, KEY_KP4 },
    { 0x65, KEY_KP5 },
    { 0x66, KEY_KP6 },
    { 0x67, KEY_KP7 },
    { 0x68, KEY_KP8 },
    { 0x69, KEY_KP9 },
    { 0x6A, KEY_KPASTERISK },  // VK_MULTIPLY
    { 0x6B, KEY_KPPLUS },      // VK_ADD
    { 0x6D, KEY_KPMINUS },     // VK_SUBTRACT
    { 0x6E, KEY_KPDOT },       // VK_DECIMAL
    { 0x6F, KEY_KPSLASH },     // VK_DIVIDE

    // Function keys F1-F12
    { 0x70, KEY_F1 },
    { 0x71, KEY_F2 },
    { 0x72, KEY_F3 },
    { 0x73, KEY_F4 },
    { 0x74, KEY_F5 },
    { 0x75, KEY_F6 },
    { 0x76, KEY_F7 },
    { 0x77, KEY_F8 },
    { 0x78, KEY_F9 },
    { 0x79, KEY_F10 },
    { 0x7A, KEY_F11 },
    { 0x7B, KEY_F12 },

    // Extended modifier keys (left / right explicit VKs)
    { 0xA0, KEY_LEFTSHIFT },   // VK_LSHIFT
    { 0xA1, KEY_RIGHTSHIFT },  // VK_RSHIFT
    { 0xA2, KEY_LEFTCTRL },    // VK_LCONTROL
    { 0xA3, KEY_RIGHTCTRL },   // VK_RCONTROL
    { 0xA4, KEY_LEFTALT },     // VK_LMENU
    { 0xA5, KEY_RIGHTALT },    // VK_RMENU
};

static constexpr size_t k_vkTableSize = sizeof( k_vkTable ) / sizeof( k_vkTable[0] );

static bool vk_to_evdev( uint32_t vk, uint32_t *evdev_out )
{
    for ( size_t i = 0; i < k_vkTableSize; ++i )
    {
        if ( k_vkTable[i].vk == vk )
        {
            *evdev_out = k_vkTable[i].evdev;
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool HarnessKeyDown( uint32_t vk_code )
{
    uint32_t evdev = 0;
    if ( !vk_to_evdev( vk_code, &evdev ) )
        return false;

    uint32_t time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_key( evdev, true, time_ms );
    wlserver_unlock();
    return true;
}

bool HarnessKeyUp( uint32_t vk_code )
{
    uint32_t evdev = 0;
    if ( !vk_to_evdev( vk_code, &evdev ) )
        return false;

    uint32_t time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_key( evdev, false, time_ms );
    wlserver_unlock();
    return true;
}

bool HarnessKey( uint32_t vk_code )
{
    uint32_t evdev = 0;
    if ( !vk_to_evdev( vk_code, &evdev ) )
        return false;

    uint32_t time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_key( evdev, true, time_ms );
    wlserver_unlock();

    usleep( (useconds_t)KEY_PRESS_DELAY_MS * 1000 );

    time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_key( evdev, false, time_ms );
    wlserver_unlock();
    return true;
}

bool HarnessMove( double x, double y )
{
    uint32_t time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_mousewarp( x, y, time_ms, true );
    wlserver_unlock();
    return true;
}

bool HarnessClick( double x, double y )
{
    uint32_t time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_mousewarp( x, y, time_ms, true );
    wlserver_mousebutton( BTN_LEFT, true, time_ms );
    wlserver_unlock();

    usleep( (useconds_t)KEY_PRESS_DELAY_MS * 1000 );

    time_ms = get_time_in_milliseconds();
    wlserver_lock();
    wlserver_mousebutton( BTN_LEFT, false, time_ms );
    wlserver_unlock();
    return true;
}

} // namespace gamescope::Harness

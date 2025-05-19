#include "stdafx.h"
#include "ThrustmasterT500RS.h"
#include "../Cell/lv2/sys_usbd.h"
#include "../../Input/pad_thread.h"
#include "../../util/types.hpp"
#include "Utilities/Timer.h"

LOG_CHANNEL(t500rs_log, "T500RS");

namespace
{
    constexpr u16 T500RS_VID = 0x044F;  // Thrustmaster vendor ID
    constexpr u16 T500RS_PID = 0xB65D;  // T500RS product ID (PS3 mode)
}

usb_device_t500rs::usb_device_t500rs(u32 controller_index, const std::array<u8, 7>& location)
    : usb_device_emulated(controller_index, location)
    , m_controller_index(controller_index)
{
    g_cfg_t500rs.load();
    m_enabled = g_cfg_t500rs.enabled;
    m_reverse_effects = g_cfg_t500rs.reverse_effects;
}

usb_device_t500rs::~usb_device_t500rs()
{
    if (m_house_keeping_thread)
    {
        m_house_keeping_thread->join();
    }

    std::lock_guard lock(m_sdl_handles_mutex);

    if (m_haptic_handle)
    {
        SDL_HapticClose(m_haptic_handle);
        m_haptic_handle = nullptr;
    }

    if (m_led_joystick_handle)
    {
        SDL_JoystickClose(m_led_joystick_handle);
        m_led_joystick_handle = nullptr;
    }

    for (auto& [_, joysticks] : m_joysticks)
    {
        for (SDL_Joystick* joystick : joysticks)
        {
            SDL_JoystickClose(joystick);
        }
    }
    m_joysticks.clear();
}

std::shared_ptr<usb_device> usb_device_t500rs::make_instance(u32 controller_index, const std::array<u8, 7>& location)
{
    return std::make_shared<usb_device_t500rs>(controller_index, location);
}

u16 usb_device_t500rs::get_num_emu_devices()
{
    return 1;
}

bool usb_device_t500rs::open_device()
{
    if (!m_enabled)
    {
        t500rs_log.notice("T500RS device disabled");
        return false;
    }

    t500rs_log.notice("Opening T500RS device");

    if (!usb_device_emulated::open_device())
    {
        return false;
    }

    // Initialize SDL if not already done
    if (SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != (SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC))
    {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0)
        {
            t500rs_log.error("SDL_InitSubSystem failed: %s", SDL_GetError());
            return false;
        }
    }

    // Start housekeeping thread for handling force feedback and LED updates
    m_house_keeping_thread = std::make_unique<named_thread<std::function<void()>>>("T500RS House Keeping Thread", [this]()
    {
        while (thread_ctrl::state() != thread_state::aborting)
        {
            if (m_enabled)
            {
                sdl_refresh();
            }
            thread_ctrl::wait_for(33'333); // ~30Hz refresh rate
        }
    });

    return true;
}

void usb_device_t500rs::sdl_refresh()
{
    std::lock_guard lock(m_sdl_handles_mutex);

    // Update force feedback effects if needed
    if (m_haptic_handle)
    {
        for (auto& slot : m_effect_slots)
        {
            if (slot.state == t500rs_ffb_state::downloaded)
            {
                if (slot.effect_id >= 0)
                {
                    SDL_HapticDestroyEffect(m_haptic_handle, slot.effect_id);
                }

                slot.effect_id = SDL_HapticNewEffect(m_haptic_handle, &slot.last_effect);
                if (slot.effect_id >= 0)
                {
                    SDL_HapticRunEffect(m_haptic_handle, slot.effect_id, 1);
                    slot.state = t500rs_ffb_state::playing;
                }
            }
        }
    }
}

void usb_device_t500rs::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer)
{
    transfer->fake = true;

    // Handle T500RS-specific control transfers here
    // This would include force feedback commands, LED control, etc.
    
    // Example of handling a force feedback command:
    if (bmRequestType == 0x21 && bRequest == 0x09)
    {
        // Force feedback data
        if (buf_size >= 8)
        {
            // Process force feedback data
            // This is a simplified example - actual implementation would need to parse the specific T500RS protocol
            SDL_HapticEffect effect{};
            effect.type = SDL_HAPTIC_CONSTANT;
            effect.constant.level = static_cast<s16>(buf[2] | (buf[3] << 8));
            if (m_reverse_effects)
            {
                effect.constant.level = -effect.constant.level;
            }
            
            // Update the effect slot
            size_t slot_index = buf[0] & 0x03;
            if (slot_index < m_effect_slots.size())
            {
                auto& slot = m_effect_slots[slot_index];
                slot.last_effect = effect;
                slot.state = t500rs_ffb_state::downloaded;
                slot.last_update = get_system_time();
            }
        }
    }

    // Complete the transfer
    transfer->status = HC_CC_NOERR;
}

void usb_device_t500rs::interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer)
{
    transfer->fake = true;

    if (endpoint == 0x81) // IN endpoint
    {
        // Fill the buffer with the current state of the wheel
        // This is a simplified example - actual implementation would need to match the T500RS protocol
        if (buf_size >= 8)
        {
            memset(buf, 0, buf_size);
            
            // Basic structure for T500RS input report
            // buf[0] = report ID
            // buf[1-2] = wheel position (0-1080 degrees)
            // buf[3] = throttle
            // buf[4] = brake
            // buf[5] = clutch
            // buf[6-7] = buttons

            // Example of setting wheel position (centered)
            buf[1] = 0x80;
            buf[2] = 0x80;
        }
    }

    transfer->status = HC_CC_NOERR;
} 
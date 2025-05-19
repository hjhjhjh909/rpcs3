#include "stdafx.h"
#include "ThrustmasterT500RS.h"
#include "../Cell/lv2/sys_usbd.h"
#include "../../Input/pad_thread.h"
#include "../../util/types.hpp"
#include "Utilities/Timer.h"
#include "ThrustmasterT500RSConfig.h" // Include the config header

LOG_CHANNEL(t500rs_log, "T500RS");

namespace
{
    constexpr u16 T500RS_VID = 0x044F;  // Thrustmaster vendor ID
    constexpr u16 T500RS_PID = 0xB65D;  // T500RS product ID (PS3 mode)

    // Helper function to convert SDL axis value to T500RS pedal value (0-255)
    static u8 sdl_to_t500rs_pedal(std::map<u64, std::vector<SDL_Joystick*>>& joysticks, const t500rs_sdl_mapping& mapping)
    {
        s16 sdl_val = fetch_sdl_axis_avg(joysticks, mapping);
        return (sdl_val + 0x8000) * 0xFF / 0xFFFF;
    }

    // Helper function to convert SDL button press to bool
    static bool sdl_to_t500rs_button(std::map<u64, std::vector<SDL_Joystick*>>& joysticks, const t500rs_sdl_mapping& mapping)
    {
        return sdl_to_logitech_g27_button(joysticks, mapping); // Reuse the function.  SDL button event to bool.
    }

    // Helper function to convert SDL axis value to T500RS wheel position
    static u16 map_sdl_to_t500rs_wheel_pos(s16 sdl_pos)
    {
        // Convert from SDL axis range (-32768 to 32767) to T500RS wheel position (0-1080 degrees)
        const float normalized = (sdl_pos + 32768.0f) / 65535.0f;
        const float degrees = normalized * 900.0f; // T500RS has 1080 degree rotation
        return static_cast<u16>(degrees);
    }
}

// Global configuration instance
t500rs_config g_cfg_t500rs;

usb_device_t500rs::usb_device_t500rs(u32 controller_index, const std::array<u8, 7>& location)
    : usb_device_emulated(controller_index, location)
    , m_controller_index(controller_index)
{
    g_cfg_t500rs.load(); // Load configuration from file
    m_enabled = g_cfg_t500rs.enabled.get();
    m_reverse_effects = g_cfg_t500rs.reverse_effects.get();
    m_wheel_range = 900; //default
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
        t500rs_log.notice("Thrustmaster FFB device disabled"); // Changed Wheel name
        return false;
    }

    t500rs_log.notice("Opening Thrustmaster FFB device");  // Changed Wheel name

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
    
     // Enumerate and open joysticks.  This is similar to the G27 implementation.
    int joystick_count = 0;
    if (SDL_JoystickID* joystick_ids = SDL_GetJoysticks(&joystick_count))
    {
        for (int i = 0; i < joystick_count; i++)
        {
            SDL_Joystick* cur_joystick = SDL_OpenJoystick(joystick_ids[i]);
            if (!cur_joystick)
            {
                t500rs_log.error("Failed opening joystick %d, %s", joystick_ids[i], SDL_GetError());
                continue;
            }
            const u16 cur_vendor_id = SDL_GetJoystickVendor(cur_joystick);
            const u16 cur_product_id = SDL_GetJoystickProduct(cur_joystick);
            
            //Use the VID and PID to identify the device
            if(cur_vendor_id == T500RS_VID && cur_product_id == T500RS_PID){ // Use the defined VID
                 m_led_joystick_handle = cur_joystick; //save
                 
                 SDL_Haptic* cur_haptic = SDL_OpenHapticFromJoystick(cur_joystick);
                if (cur_haptic == nullptr)
                {
                    t500rs_log.error("Failed opening haptic device from selected ffb device %04x:%04x, %s", cur_vendor_id, cur_product_id, SDL_GetError());
                }
                else
                {
                    m_haptic_handle = cur_haptic;
                }
            }
            
            const emulated_g27_device_type_id joystick_type_id_struct =
            {
                .product_id = static_cast<u64>(cur_product_id),
                .vendor_id = static_cast<u64>(cur_vendor_id),
                .num_axes = static_cast<u64>(SDL_GetNumJoystickAxes(cur_joystick)),
                .num_hats = static_cast<u64>(SDL_GetNumJoystickHats(cur_joystick)),
                .num_buttons = static_cast<u64>(SDL_GetNumJoystickButtons(cur_joystick))
            };
            const u64 joystick_type_id = joystick_type_id_struct.as_u64();
            auto joysticks_of_type = m_joysticks.find(joystick_type_id);
            if (joysticks_of_type == m_joysticks.end())
            {
                m_joysticks[joystick_type_id] = { cur_joystick };
            }
            else
            {
                joysticks_of_type->second.push_back(cur_joystick);
            }
        }
        SDL_free(joystick_ids);
    }
    else
    {
        t500rs_log.error("Failed fetching joystick list, %s", SDL_GetError());
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
                 else
                {
                     t500rs_log.error("SDL_HapticNewEffect failed: %s", SDL_GetError());
                }
            }
        }
    }
    
    //handle LED
    if(m_led_joystick_handle){
       //SDL_SetJoystickLED(m_led_joystick_handle, r, g, b); //not implemented
    }
}



void usb_device_t500rs::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer)
{
    transfer->fake = true;
    transfer->status = HC_CC_NOERR;
    
     if (bmRequestType == 0x21 && bRequest == 0x09) // Example: FF command
    {
        if (buf_size >= 8) // Adjust based on T500RS FF command size
        {
            // 1. Parse T500RS FF Command
            u8 effect_type = buf[1];
            // Extract other parameters (strength, duration, etc.)

            // 2. Convert to SDL Haptic Effect
            SDL_HapticEffect effect;
            memset(&effect, 0, sizeof(SDL_HapticEffect)); //important
            switch (effect_type)
            {
                case 0x01: // Example: Constant Force
                    effect.type = SDL_HAPTIC_CONSTANT;
                    effect.constant.direction.type = SDL_HAPTIC_POLAR;
                    effect.constant.direction.dir[0] = 0; // Angle
                    effect.constant.length = SDL_HAPTIC_INFINITY;
                    effect.constant.level = static_cast<s16>(buf[2] | (buf[3] << 8)); // Example
                    if(m_reverse_effects){
                       effect.constant.level = -effect.constant.level;
                    }
                    break;
                case 0x02: // Example: Spring Effect
                    effect.type = SDL_HAPTIC_SPRING;
                    effect.condition.direction.type = SDL_HAPTIC_POLAR;
                    effect.condition.direction.dir[0] = 0;
                    effect.condition.length = SDL_HAPTIC_INFINITY;
                    effect.condition.right_coeff[0] = static_cast<s16>(buf[4]);
                    effect.condition.left_coeff[0] = static_cast<s16>(buf[5]);
                    break;
                // Handle other effect types
                default:
                  t500rs_log.error("Unsupported effect type: %02x", effect_type);
                  transfer->status = HC_CC_ERROR;
                  return;
            }

            // 3.  Find a free slot and store the effect.
            size_t slot_index = buf[0] & 0x03; // Example slot
            if (slot_index < m_effect_slots.size())
            {
                auto& slot = m_effect_slots[slot_index];
                slot.last_effect = effect;
                slot.state = t500rs_ffb_state::downloaded; // Mark it as ready to play
                slot.last_update = get_system_time();
            }
            else
            {
                t500rs_log.error("Invalid effect slot: %d", slot_index);
                transfer->status = HC_CC_ERROR;
                return;
            }
        }
        else
        {
          t500rs_log.error("FF data too small: %d", buf_size);
          transfer->status = HC_CC_ERROR;
          return;
        }
    }
}

void usb_device_t500rs::interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer)
{
    transfer->fake = true;
    transfer->status = HC_CC_NOERR;

    if (endpoint == 0x81) // IN endpoint
    {
        if (buf_size < 8) // Adjust based on actual T500RS report size
        {
            t500rs_log.error("Input buffer too small: %u, expected 8", buf_size);
            return;
        }
        memset(buf, 0, buf_size);

        sdl_instance::get_instance().pump_events();
        m_sdl_handles_mutex.lock();

        // 1. Get SDL Joystick State
        s16 wheel_position_sdl = fetch_sdl_axis_avg(m_joysticks, g_cfg_t500rs.steering);
        u8 throttle_sdl = sdl_to_t500rs_pedal(m_joysticks, g_cfg_t500rs.throttle);
        u8 brake_sdl = sdl_to_t500rs_pedal(m_joysticks, g_cfg_t500rs.brake);
        u8 clutch_sdl = sdl_to_t500rs_pedal(m_joysticks, g_cfg_t500rs.clutch);
        bool shift_up_sdl = sdl_to_t500rs_button(m_joysticks, g_cfg_t500rs.shift_up);
        bool shift_down_sdl = sdl_to_t500rs_button(m_joysticks, g_cfg_t500rs.shift_down);
        bool horn_sdl = sdl_to_t500rs_button(m_joysticks, g_cfg_t500rs.horn);

        m_sdl_handles_mutex.unlock();

        // 2. Map SDL State to T500RS Report Format
        // Example (assuming 16-bit wheel position):
        u16 wheel_position_t500rs = map_sdl_to_t500rs_wheel_pos(wheel_position_sdl);
        buf[1] = wheel_position_t500rs & 0xFF;
        buf[2] = (wheel_position_t500rs >> 8) & 0xFF;

        buf[3] = throttle_sdl;
        buf[4] = brake_sdl;
        buf[5] = clutch_sdl;

        // 3. Map buttons.  You'll need the correct bit positions for the T500RS.  This is an example.
        u16 buttons = 0;
        if (shift_up_sdl) buttons |= (1 << 0);
        if (shift_down_sdl) buttons |= (1 << 1);
        if (horn_sdl) buttons |= (1 << 2);
        buf[6] = buttons & 0xFF;
        buf[7] = (buttons >> 8) & 0xFF;
    }
}


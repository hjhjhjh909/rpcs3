
#include "stdafx.h"

#ifdef HAVE_SDL3

#include "stdafx.h"
#include "ThrustmasterT500RS.h"
#include "util/types.hpp"
#include "Utilities/Timer.h"

LOG_CHANNEL(thrustmaster_ffb2, "ThrustmasterFFB2"); // New log channel - changed name

namespace
{
    // Helper function to convert SDL axis value to a centered and normalized value (-1 to 1)
    static float map_sdl_axis_to_float(s16 sdl_val)
    {
        return static_cast<float>(sdl_val) / 32767.0f;
    }

    // Helper function to convert SDL axis value to a Thrustmaster pedal value (0-255)
    static u8 sdl_to_thrustmaster_pedal(std::map<u64, std::vector<SDL_Joystick*>>& joysticks, const thrustmaster_sdl_mapping& mapping)
    {
        s16 sdl_val = fetch_sdl_axis_avg(joysticks, mapping);
        return (sdl_val + 32768) * 255 / 65535;
    }

    // Helper function to convert SDL button press to bool
    static bool sdl_to_thrustmaster_button(std::map<u64, std::vector<SDL_Joystick*>>& joysticks, const thrustmaster_sdl_mapping& mapping)
    {
        if (joysticks.empty()) return false;
        for (auto& [_, device_list] : joysticks)
        {
            if (device_list.empty()) continue;
            for (SDL_Joystick* joystick : device_list)
            {
                if (!joystick) continue;
                for (const auto& [input_id, mapped_inputs] : mapping.input_map)
                {
                    if (mapped_inputs.empty()) continue;
                    for (const auto& mapped_input : mapped_inputs)
                    {
                        if (mapped_input.type == input_type::button)
                        {
                            if (SDL_JoystickGetButton(joystick, mapped_input.index))
                            {
                                return true;
                            }
                        }
                    }
                }
            }
        }
        return false;
    }

    // Helper function to convert SDL axis value to Thrustmaster wheel position (0-0xFFFF)
    static u16 map_sdl_to_thrustmaster_wheel_pos(s16 sdl_pos, u16 wheel_range)
    {
        float normalized = (sdl_pos + 32768.0f) / 65535.0f;
        u16 position = static_cast<u16>(normalized * wheel_range);
        return position;
    }

    // Function to convert Thrustmaster FFB effect to a byte array (similar to G27, but adapted for Thrustmaster)
    static std::vector<u8> convert_thrustmaster_effect_to_bytes(const u8* effect_data, u16 buf_size, bool reverse)
    {
        std::vector<u8> bytes;
        if (buf_size < 8)
        {
            thrustmaster_ffb2.error("FFB effect data too small: %u, expected at least 8", buf_size); // Changed log channel
            return bytes; // Return empty vector
        }

        u8 effect_type = effect_data[1];
        s16 magnitude = static_cast<s16>(effect_data[2] | (effect_data[3] << 8));
        s16 direction = static_cast<s16>(effect_data[4] | (effect_data[5] << 8)); // Direction is often 16-bit
        u32 duration = static_cast<u32>(effect_data[6] | (effect_data[7] << 8) | (effect_data[8] << 16) | (effect_data[9] << 24)); // Duration can be longer

        if (reverse)
        {
            magnitude = -magnitude;
        }

        // Thrustmaster-specific FFB data formatting.  This is the crucial part that needs to be adapted.
        // The following is a placeholder example.  YOU MUST DETERMINE THE ACTUAL FORMAT.
        bytes.push_back(0x01); // Example: Message type for FFB
        bytes.push_back(effect_type);
        bytes.push_back(magnitude & 0xFF);
        bytes.push_back((magnitude >> 8) & 0xFF);
        bytes.push_back(direction & 0xFF);
        bytes.push_back((direction >> 8) & 0xFF);
        bytes.push_back(duration & 0xFF);
        bytes.push_back((duration >> 8) & 0xFF);
        bytes.push_back((duration >> 16) & 0xFF);
        bytes.push_back((duration >> 24) & 0xFF);
        // Add more Thrustmaster-specific data as needed.  This is device dependent!

        return bytes;
    }
} // namespace

// Global configuration instance
#include "ThrustmasterFFB2Config.h" // New config - changed name
thrustmaster_ffb2_config g_cfg_thrustmaster_ffb2; // New config - changed name

usb_device_thrustmaster_ffb2::usb_device_thrustmaster_ffb2(u32 controller_index, const std::array<u8, 7>& location) // Changed class name
    : usb_device_emulated(controller_index, location)
    , m_controller_index(controller_index)
{
    g_cfg_thrustmaster_ffb2.load(); // Changed config name
    m_enabled = g_cfg_thrustmaster_ffb2.enabled.get(); // Changed config name
    m_reverse_effects = g_cfg_thrustmaster_ffb2.reverse_effects.get(); // Changed config name
    m_wheel_range = g_cfg_thrustmaster_ffb2.wheel_range.get(); // Changed config name
    std::cout << "Thrustmaster FFB2 device created\n";
}

usb_device_thrustmaster_ffb2::~usb_device_thrustmaster_ffb2() // Changed class name
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
    std::cout << "Thrustmaster FFB2 device destroyed\n";
}

std::shared_ptr<usb_device> usb_device_thrustmaster_ffb2::make_instance(u32 controller_index, const std::array<u8, 7>& location) // Changed class name
{
    return std::make_shared<usb_device_thrustmaster_ffb2>(controller_index, location); // Changed class name
}

u16 usb_device_thrustmaster_ffb2::get_num_emu_devices() // Changed class name
{
    return 1;
}

bool usb_device_thrustmaster_ffb2::open_device() // Changed class name
{
    if (!m_enabled)
    {
        thrustmaster_ffb2.notice("Thrustmaster FFB2 device disabled"); // Changed log channel
        return false;
    }

    thrustmaster_ffb2.notice("Opening Thrustmaster FFB2 device"); // Changed log channel

    if (!usb_device_emulated::open_device())
    {
        return false;
    }

    if (SDL_WasInit(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != (SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC))
    {
        if (SDL_InitSubSystem(SDL_INIT_JOYSTICK | SDL_INIT_HAPTIC) != 0)
        {
            thrustmaster_ffb2.error("SDL_InitSubSystem failed: %s", SDL_GetError()); // Changed log channel
            return false;
        }
    }

    int joystick_count = 0;
    if (SDL_JoystickID* joystick_ids = SDL_GetJoysticks(&joystick_count))
    {
        for (int i = 0; i < joystick_count; i++)
        {
            SDL_Joystick* cur_joystick = SDL_OpenJoystick(joystick_ids[i]);
            if (!cur_joystick)
            {
                thrustmaster_ffb2.error("Failed opening joystick %d, %s", joystick_ids[i], SDL_GetError()); // Changed log channel
                continue;
            }
            const u16 cur_vendor_id = SDL_GetJoystickVendor(cur_joystick);
            const u16 cur_product_id = SDL_GetJoystickProduct(cur_joystick);

            //  Match Vendor and Product ID.
             if (cur_vendor_id == TMX_WHEEL_VID && (cur_product_id == TMX_WHEEL_PID || cur_product_id == T150_WHEEL_PID))
            {
                m_led_joystick_handle = cur_joystick; //same handle

                SDL_Haptic* cur_haptic = SDL_OpenHapticFromJoystick(cur_joystick);
                if (cur_haptic == nullptr)
                {
                    thrustmaster_ffb2.error("Failed opening haptic device from selected ffb device %04x:%04x, %s", cur_vendor_id, cur_product_id, SDL_GetError());
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
        thrustmaster_ffb2.error("Failed fetching joystick list, %s", SDL_GetError()); // Changed log channel
    }

    m_house_keeping_thread = std::make_unique<named_thread<std::function<void()>>>("Thrustmaster FFB2 House Keeping Thread", [this]() // Changed thread name
    {
        while (thread_ctrl::state() != thread_state::aborting)
        {
            if (m_enabled)
            {
                sdl_refresh();
            }
            thread_ctrl::wait_for(33'333);
        }
    });

    return true;
}

void usb_device_thrustmaster_ffb2::sdl_refresh() // Changed class name
{
    std::lock_guard lock(m_sdl_handles_mutex);

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
                    thrustmaster_ffb2.error("SDL_HapticNewEffect failed: %s", SDL_GetError()); // Changed log channel
                }
            }
        }
    }

    if (m_led_joystick_handle)
    {
       // SDL_SetJoystickLED(m_led_joystick_handle, r, g, b);  // Not implemented
    }
}

void usb_device_thrustmaster_ffb2::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer) // Changed class name
{
    transfer->fake = true;
    transfer->status = HC_CC_NOERR;

    if (bmRequestType == 0x21 && bRequest == 0x09)
    {
        if (buf_size >= 8)
        {
            SDL_HapticEffect effect = convert_t500rs_effect_to_sdl(buf, buf_size, m_reverse_effects.get());
            if (effect.type == 0)
            {
                transfer->status = HC_CC_ERROR;
                return;
            }

            size_t slot_index = buf[0] & 0x03;
            if (slot_index < m_effect_slots.size())
            {
                auto& slot = m_effect_slots[slot_index];
                slot.last_effect = effect;
                slot.state = t500rs_ffb_state::downloaded;
                slot.last_update = get_system_time();
            }
            else
            {
                thrustmaster_ffb2.error("Invalid effect slot: %d", slot_index); // Changed log channel
                transfer->status = HC_CC_ERROR;
                return;
            }
        }
        else
        {
            thrustmaster_ffb2.error("FF data too small: %d", buf_size); // Changed log channel
            transfer->status = HC_CC_ERROR;
            return;
        }
    }
}

void usb_device_thrustmaster_ffb2::interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer) // Changed class name
{
    transfer->fake = true;
    transfer->status = HC_CC_NOERR;

    if (endpoint == 0x81)
    {
        if (buf_size < 8)
        {
            thrustmaster_ffb2.error("Input buffer too small: %u, expected 8", buf_size); // Changed log channel
            return;
        }
        memset(buf, 0, buf_size);

        sdl_instance::get_instance().pump_events();
        m_sdl_handles_mutex.lock();

        s16 wheel_position_sdl = fetch_sdl_axis_avg(m_joysticks, g_cfg_thrustmaster_ffb2.steering); // Changed config name
        u8 throttle_sdl = sdl_to_thrustmaster_pedal(m_joysticks, g_cfg_thrustmaster_ffb2.throttle); // Changed config name
        u8 brake_sdl = sdl_to_thrustmaster_pedal(m_joysticks, g_cfg_thrustmaster_ffb2.brake); // Changed config name
        u8 clutch_sdl = sdl_to_thrustmaster_pedal(m_joysticks, g_cfg_thrustmaster_ffb2.clutch); // Changed config name
        bool shift_up_sdl = sdl_to_thrustmaster_button(m_joysticks, g_cfg_thrustmaster_ffb2.shift_up); // Changed config name
        bool shift_down_sdl = sdl_to_thrustmaster_button(m_joysticks, g_cfg_thrustmaster_ffb2.shift_down); // Changed config name
        bool horn_sdl = sdl_to_thrustmaster_button(m_joysticks, g_cfg_thrustmaster_ffb2.horn);     // Changed config name

        m_sdl_handles_mutex.unlock();

        u16 wheel_position_thrustmaster = map_sdl_to_thrustmaster_wheel_pos(wheel_position_sdl, m_wheel_range);
        buf[1] = wheel_position_thrustmaster & 0xFF;
        buf[2] = (wheel_position_thrustmaster >> 8) & 0xFF;
        buf[3] = throttle_sdl;
        buf[4] = brake_sdl;
        buf[5] = clutch_sdl;

        u16 buttons = 0;
        if (shift_up_sdl) buttons |= (1 << 0);
        if (shift_down_sdl) buttons |= (1 << 1);
        if (horn_sdl) buttons |= (1 << 2);
        buf[6] = buttons & 0xFF;
        buf[7] = (buttons >> 8) & 0xFF;
    }
}



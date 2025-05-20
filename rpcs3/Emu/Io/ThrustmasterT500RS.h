#pragma once

#include "usb_device.h"
#include "../../Utilities/Thread.h"
#include "ThrustmasterT500RSConfig.h"
#include "../../Input/sdl_pad_handler.h"

#ifndef _MSC_VER
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
#include "SDL3/SDL.h"
#ifndef _MSC_VER
#pragma GCC diagnostic pop
#endif

#include <map>
#include <vector>

enum class t500rs_ffb_state
{
    inactive,
    downloaded,
    playing
};

struct t500rs_ffb_slot
{
    t500rs_ffb_state state = t500rs_ffb_state::inactive;
    u64 last_update = 0;
    SDL_HapticEffect last_effect {};
    int effect_id = -1;
};

struct t500rs_sdl_mapping
{
    sdl_mapping steering {};
    sdl_mapping throttle {};
    sdl_mapping brake {};
    sdl_mapping clutch {};
    sdl_mapping shift_up {};
    sdl_mapping shift_down {};

    sdl_mapping up {};
    sdl_mapping down {};
    sdl_mapping left {};
    sdl_mapping right {};

    sdl_mapping triangle {};
    sdl_mapping cross {};
    sdl_mapping square {};
    sdl_mapping circle {};

    sdl_mapping l2 {};
    sdl_mapping l3 {};
    sdl_mapping r2 {};
    sdl_mapping r3 {};

    sdl_mapping select {};
    sdl_mapping start {};
};

class usb_device_t500rs : public usb_device_emulated
{
public:
    usb_device_t500rs(u32 controller_index, const std::array<u8, 7>& location);
    ~usb_device_t500rs();

    static std::shared_ptr<usb_device> make_instance(u32 controller_index, const std::array<u8, 7>& location);
    static u16 get_num_emu_devices();

    void control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer) override;
    void interrupt_transfer(u32 buf_size, u8* buf, u32 endpoint, UsbTransfer* transfer) override;
    bool open_device() override;

private:
    void sdl_refresh();

    u32 m_controller_index = 0;

    t500rs_sdl_mapping m_mapping {};
    bool m_reverse_effects = false;

    std::mutex m_sdl_handles_mutex;
    SDL_Joystick* m_led_joystick_handle = nullptr;
    SDL_Haptic* m_haptic_handle = nullptr;
    std::map<u64, std::vector<SDL_Joystick*>> m_joysticks;
    bool m_fixed_loop = false;
    u16 m_wheel_range = 900; // T500RS uses 900 degrees in PS3 mode
    std::array<t500rs_ffb_slot, 4> m_effect_slots {};
    SDL_HapticEffect m_default_spring_effect {};
    int m_default_spring_effect_id = -1;

    bool m_enabled = false;

    std::unique_ptr<named_thread<std::function<void()>>> m_house_keeping_thread;
};


// Thrustmaster T500RS

#include "stdafx.h"

#ifdef HAVE_SDL3

#include "ThrustmasterT500RS.h"
#include "Emu/Cell/lv2/sys_usbd.h"
#include "Emu/system_config.h"
#include "Input/pad_thread.h"
#include "Input/sdl_instance.h"

LOG_CHANNEL(thrustmaster_t500rs_log, "THRMSTR500RS");

static const SDL_HapticDirection STEERING_DIRECTION =
{
    .type = SDL_HAPTIC_STEERING_AXIS,
    .dir = {0, 0, 0}
};

usb_device_thrustmaster_t500rs::usb_device_thrustmaster_t500rs(u32 controller_index, const std::array<u8, 7>& location)
    : usb_device_emulated(location), m_controller_index(controller_index)
{
    // USB descriptor: update vendor/product for T500RS if different
    device = UsbDescriptorNode(USB_DESCRIPTOR_DEVICE, UsbDeviceDescriptor{0x0200, 0, 0, 0, 16, 0x044f, 0xb65d, 0x1350, 1, 2, 0, 1});

    // TODO: Update raw_config to match the T500RS, for now we copy G27 as placeholder
    static constexpr u8 raw_config[] = { /* copy the config array from G27 or adapt for T500RS */ };
    auto& conf = device.add_node(UsbDescriptorNode(raw_config[0], raw_config[1], &raw_config[2]));
    for (unsigned int index = raw_config[0]; index < sizeof(raw_config);)
    {
        conf.add_node(UsbDescriptorNode(raw_config[index], raw_config[index + 1], &raw_config[index + 2]));
        index += raw_config[index];
    }

    m_default_spring_effect.type = SDL_HAPTIC_SPRING;
    m_default_spring_effect.condition.direction = STEERING_DIRECTION;
    m_default_spring_effect.condition.length = SDL_HAPTIC_INFINITY;
    for (int i = 0; i < 1; i++)
    {
        m_default_spring_effect.condition.right_sat[i] = 0x7FFF;
        m_default_spring_effect.condition.left_sat[i] = 0x7FFF;
        m_default_spring_effect.condition.right_coeff[i] = 0x7FFF;
        m_default_spring_effect.condition.left_coeff[i] = 0x7FFF;
    }

    g_cfg_thrustmaster_t500rs.load();
    m_enabled = g_cfg_thrustmaster_t500rs.enabled.get() && sdl_instance::get_instance().initialize();

    if (!m_enabled)
        return;

    m_house_keeping_thread = std::make_unique<named_thread<std::function<void()>>>("Thrustmaster T500RS", [this]()
    {
        while (thread_ctrl::state() != thread_state::aborting)
        {
            sdl_refresh();
            thread_ctrl::wait_for(5'000'000);
        }
    });
}

bool usb_device_thrustmaster_t500rs::open_device()
{
    return m_enabled;
}

usb_device_thrustmaster_t500rs::~usb_device_thrustmaster_t500rs()
{
    m_house_keeping_thread.reset();

    const std::lock_guard lock(m_sdl_handles_mutex);
    if (m_haptic_handle)
    {
        SDL_CloseHaptic(m_haptic_handle);
        m_haptic_handle = nullptr;
    }
    clear_sdl_joysticks(m_joysticks);
}

std::shared_ptr<usb_device> usb_device_thrustmaster_t500rs::make_instance(u32 controller_index, const std::array<u8, 7>& location)
{
    return std::make_shared<usb_device_thrustmaster_t500rs>(controller_index, location);
}

u16 usb_device_thrustmaster_t500rs::get_num_emu_devices()
{
    return 1;
}

void usb_device_thrustmaster_t500rs::control_transfer(u8 bmRequestType, u8 bRequest, u16 wValue, u16 wIndex, u16 wLength, u32 buf_size, u8* buf, UsbTransfer* transfer)
{
    transfer->fake = true;
    transfer->expected_count = buf_size;
    transfer->expected_result = HC_CC_NOERR;
    transfer->expected_time = get_timestamp() + 100;

    usb_device_emulated::control_transfer(bmRequestType, bRequest, wValue, wIndex, wLength, buf_size, buf, transfer);
}

#endif



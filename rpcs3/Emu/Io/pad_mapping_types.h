#pragma once

enum class sdl_mapping_type
{
    button = 0,
    hat,
    axis,
};

enum class hat_component
{
    none = 0,
    up,
    down,
    left,
    right
};

struct joystick_state
{
    std::vector<s16> axes;
    std::vector<bool> buttons;
    std::vector<hat_component> hats;
}; 
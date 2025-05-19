#include "stdafx.h"

#ifdef HAVE_SDL3

#include "emulated_thrustmaster_t500rs_settings_dialog.h"
#include "Emu/Io/ThrustmasterT500RS.h"
#include "Input/sdl_instance.h"
#include "qt_utils.h"
#include "Emu/Io/ThrustmasterT500RSConfig.h"
#include "../../Utilities/Config.h"
#include "../../Utilities/File.h"
#include "../../util/logs.hpp"

#include <QDialogButtonBox>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QScrollBar>
#include <QTimer>
#include <QSlider>
#include <QComboBox>
#include <QIcon>

LOG_CHANNEL(t500rs_cfg_log, "T500RS");
LOG_CHANNEL(t500rs_log, "T500RS");

static const QString DEFAULT_STATUS = " ";

enum class mapping_device
{
    NONE = -1,

    // Axis
    STEERING = 0,
    THROTTLE,
    BRAKE,
    CLUTCH,

    // Buttons
    FIRST_BUTTON,
    SHIFT_UP = FIRST_BUTTON,
    SHIFT_DOWN,

    UP,
    DOWN,
    LEFT,
    RIGHT,

    TRIANGLE,
    CROSS,
    SQUARE,
    CIRCLE,

    L2,
    L3,
    R2,
    R3,

    SELECT,
    START,

    // Enum count
    COUNT
};

static QString device_name(mapping_device device)
{
    switch (device)
    {
    case mapping_device::STEERING: return tr("Steering");
    case mapping_device::THROTTLE: return tr("Throttle");
    case mapping_device::BRAKE: return tr("Brake");
    case mapping_device::CLUTCH: return tr("Clutch");
    case mapping_device::SHIFT_UP: return tr("Shift Up");
    case mapping_device::SHIFT_DOWN: return tr("Shift Down");
    case mapping_device::UP: return tr("D-Pad Up");
    case mapping_device::DOWN: return tr("D-Pad Down");
    case mapping_device::LEFT: return tr("D-Pad Left");
    case mapping_device::RIGHT: return tr("D-Pad Right");
    case mapping_device::TRIANGLE: return tr("Triangle");
    case mapping_device::CROSS: return tr("Cross");
    case mapping_device::SQUARE: return tr("Square");
    case mapping_device::CIRCLE: return tr("Circle");
    case mapping_device::L2: return tr("L2");
    case mapping_device::L3: return tr("L3");
    case mapping_device::R2: return tr("R2");
    case mapping_device::R3: return tr("R3");
    case mapping_device::SELECT: return tr("Select");
    case mapping_device::START: return tr("Start");
    default: return tr("Unknown");
    }
}

class DeviceChoice : public QWidget
{
    Q_OBJECT

public:
    DeviceChoice(QWidget* parent, const QString& label) : QWidget(parent)
    {
        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        QLabel* label_widget = new QLabel(label, this);
        layout->addWidget(label_widget);

        m_combo = new QComboBox(this);
        m_combo->addItem(tr("None"), static_cast<int>(mapping_device::NONE));
        layout->addWidget(m_combo);

        setLayout(layout);
    }

    void set_device_choice(mapping_device device)
    {
        const int index = m_combo->findData(static_cast<int>(device));
        if (index >= 0)
        {
            m_combo->setCurrentIndex(index);
        }
    }

    mapping_device get_device_choice() const
    {
        return static_cast<mapping_device>(m_combo->currentData().toInt());
    }

    void set_enable(bool enable)
    {
        m_combo->setEnabled(enable);
    }

private:
    QComboBox* m_combo = nullptr;
};

class Mapping : public QWidget
{
    Q_OBJECT

public:
    Mapping(QWidget* parent, emulated_thrustmaster_t500rs_settings_dialog* dialog, bool is_axis, const QString& name, bool flip_axis_display)
        : QWidget(parent), m_dialog(dialog), m_is_axis(is_axis), m_flip_axis_display(flip_axis_display)
    {
        QHBoxLayout* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);

        QLabel* name_label = new QLabel(name, this);
        name_label->setMinimumWidth(140);
        layout->addWidget(name_label);

        m_device_combo = new QComboBox(this);
        m_device_combo->setMinimumWidth(200);
        layout->addWidget(m_device_combo);

        m_type_combo = new QComboBox(this);
        m_type_combo->addItem(tr("Button"), static_cast<int>(sdl_mapping_type::button));
        m_type_combo->addItem(tr("Hat"), static_cast<int>(sdl_mapping_type::hat));
        m_type_combo->addItem(tr("Axis"), static_cast<int>(sdl_mapping_type::axis));
        m_type_combo->setMinimumWidth(100);
        layout->addWidget(m_type_combo);

        m_id_combo = new QComboBox(this);
        m_id_combo->setMinimumWidth(100);
        layout->addWidget(m_id_combo);

        m_hat_combo = new QComboBox(this);
        m_hat_combo->addItem(tr("None"), static_cast<int>(hat_component::none));
        m_hat_combo->addItem(tr("Up"), static_cast<int>(hat_component::up));
        m_hat_combo->addItem(tr("Down"), static_cast<int>(hat_component::down));
        m_hat_combo->addItem(tr("Left"), static_cast<int>(hat_component::left));
        m_hat_combo->addItem(tr("Right"), static_cast<int>(hat_component::right));
        m_hat_combo->setMinimumWidth(100);
        layout->addWidget(m_hat_combo);

        m_reverse_checkbox = new QCheckBox(tr("Reverse"), this);
        layout->addWidget(m_reverse_checkbox);

        layout->addStretch();

        setLayout(layout);

        connect(m_device_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Mapping::on_device_combo_change);
        connect(m_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Mapping::on_type_combo_change);
        connect(m_id_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Mapping::on_id_combo_change);
        connect(m_hat_combo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &Mapping::on_hat_combo_change);
        connect(m_reverse_checkbox, &QCheckBox::stateChanged, this, &Mapping::on_reverse_checkbox_change);

        QTimer* timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, &Mapping::update_state);
        timer->start(100);
    }

    void set_mapping(const sdl_mapping& mapping)
    {
        m_mapping = mapping;
        update_device_combo();
        update_type_combo();
        update_id_combo();
        update_hat_combo();
        m_reverse_checkbox->setChecked(mapping.reverse);
    }

    const sdl_mapping& get_mapping() const
    {
        return m_mapping;
    }

    void set_enable(bool enable)
    {
        m_device_combo->setEnabled(enable);
        m_type_combo->setEnabled(enable);
        m_id_combo->setEnabled(enable);
        m_hat_combo->setEnabled(enable);
        m_reverse_checkbox->setEnabled(enable);
    }

private:
    void update_device_combo()
    {
        m_device_combo->clear();
        m_device_combo->addItem(tr("None"), 0);

        const auto& states = m_dialog->get_joystick_states();
        for (const auto& [device_type_id, state] : states)
        {
            m_device_combo->addItem(QString("Device %1").arg(device_type_id), device_type_id);
        }

        const int index = m_device_combo->findData(m_mapping.device_type_id);
        if (index >= 0)
        {
            m_device_combo->setCurrentIndex(index);
        }
    }

    void update_type_combo()
    {
        const int index = m_type_combo->findData(static_cast<int>(m_mapping.type));
        if (index >= 0)
        {
            m_type_combo->setCurrentIndex(index);
        }
    }

    void update_id_combo()
    {
        m_id_combo->clear();

        const auto& states = m_dialog->get_joystick_states();
        const auto it = states.find(m_mapping.device_type_id);
        if (it == states.end())
        {
            return;
        }

        const auto& state = it->second;

        switch (m_mapping.type)
        {
        case sdl_mapping_type::button:
            for (usz i = 0; i < state.buttons.size(); i++)
            {
                m_id_combo->addItem(QString("Button %1").arg(i), static_cast<u64>(i));
            }
            break;
        case sdl_mapping_type::hat:
            for (usz i = 0; i < state.hats.size(); i++)
            {
                m_id_combo->addItem(QString("Hat %1").arg(i), static_cast<u64>(i));
            }
            break;
        case sdl_mapping_type::axis:
            for (usz i = 0; i < state.axes.size(); i++)
            {
                m_id_combo->addItem(QString("Axis %1").arg(i), static_cast<u64>(i));
            }
            break;
        }

        const int index = m_id_combo->findData(m_mapping.id);
        if (index >= 0)
        {
            m_id_combo->setCurrentIndex(index);
        }
    }

    void update_hat_combo()
    {
        const int index = m_hat_combo->findData(static_cast<int>(m_mapping.hat));
        if (index >= 0)
        {
            m_hat_combo->setCurrentIndex(index);
        }
    }

    void on_device_combo_change(int index)
    {
        if (index < 0)
            return;

        m_mapping.device_type_id = m_device_combo->currentData().toULongLong();
        update_id_combo();
    }

    void on_type_combo_change(int index)
    {
        if (index < 0)
            return;

        m_mapping.type = static_cast<sdl_mapping_type>(m_type_combo->currentData().toInt());
        update_id_combo();
    }

    void on_id_combo_change(int index)
    {
        if (index < 0)
            return;

        m_mapping.id = m_id_combo->currentData().toULongLong();
    }

    void on_hat_combo_change(int index)
    {
        if (index < 0)
            return;

        m_mapping.hat = static_cast<hat_component>(m_hat_combo->currentData().toInt());
    }

    void on_reverse_checkbox_change(int state)
    {
        m_mapping.reverse = state == Qt::Checked;
    }

    void update_state()
    {
        const auto& states = m_dialog->get_joystick_states();
        const auto it = states.find(m_mapping.device_type_id);
        if (it == states.end())
        {
            m_dialog->set_state_text(DEFAULT_STATUS);
            return;
        }

        const auto& state = it->second;

        QString status;

        switch (m_mapping.type)
        {
        case sdl_mapping_type::button:
            if (m_mapping.id < state.buttons.size())
            {
                status = state.buttons[m_mapping.id] ? tr("Pressed") : tr("Released");
            }
            break;
        case sdl_mapping_type::hat:
            if (m_mapping.id < state.hats.size())
            {
                status = state.hats[m_mapping.id] == m_mapping.hat ? tr("Pressed") : tr("Released");
            }
            break;
        case sdl_mapping_type::axis:
            if (m_mapping.id < state.axes.size())
            {
                const s16 value = state.axes[m_mapping.id];
                status = QString("%1").arg(m_flip_axis_display ? -value : value);
            }
            break;
        }

        if (!status.isEmpty())
        {
            m_dialog->set_state_text(tr("%1: %2").arg(m_device_combo->currentText()).arg(status));
        }
    }

    emulated_thrustmaster_t500rs_settings_dialog* m_dialog = nullptr;
    bool m_is_axis = false;
    bool m_flip_axis_display = false;
    sdl_mapping m_mapping;

    QComboBox* m_device_combo = nullptr;
    QComboBox* m_type_combo = nullptr;
    QComboBox* m_id_combo = nullptr;
    QComboBox* m_hat_combo = nullptr;
    QCheckBox* m_reverse_checkbox = nullptr;
};

emulated_thrustmaster_t500rs_settings_dialog::emulated_thrustmaster_t500rs_settings_dialog(QWidget* parent)
    : QDialog(parent)
{
    setObjectName("emulated_thrustmaster_t500rs_settings_dialog");
    setWindowTitle(tr("Thrustmaster T500RS Settings"));
    setWindowIcon(QIcon(":/rpcs3.ico"));
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_StyledBackground);
    setModal(true);

    QVBoxLayout* v_layout = new QVBoxLayout(this);

    QDialogButtonBox* buttons = new QDialogButtonBox(this);
    buttons->setStandardButtons(QDialogButtonBox::Apply | QDialogButtonBox::Cancel | QDialogButtonBox::Save | QDialogButtonBox::RestoreDefaults);

    g_cfg_t500rs.load();

    connect(buttons, &QDialogButtonBox::clicked, this, [this, buttons](QAbstractButton* button)
    {
        if (button == buttons->button(QDialogButtonBox::Apply))
        {
            save_ui_state_to_config();
            g_cfg_t500rs.save();
            load_ui_state_from_config();
        }
        else if (button == buttons->button(QDialogButtonBox::Save))
        {
            save_ui_state_to_config();
            g_cfg_t500rs.save();
            accept();
        }
        else if (button == buttons->button(QDialogButtonBox::RestoreDefaults))
        {
            if (QMessageBox::question(this, tr("Confirm Reset"), tr("Reset all?")) != QMessageBox::Yes)
                return;
            g_cfg_t500rs.reset();
            load_ui_state_from_config();
        }
        else if (button == buttons->button(QDialogButtonBox::Cancel))
        {
            reject();
        }
    });

    QLabel* warning = new QLabel(tr("Warning: Force feedback output were meant for T500RS, on different wheels please adjust force strength accordingly in your wheel software."), this);
    warning->setStyleSheet(QString("color: %0;").arg(gui::utils::get_label_color("emulated_t500rs_warning_label", Qt::red, Qt::red).name()));
    warning->setWordWrap(true);
    v_layout->addWidget(warning);

    QLabel* mapping_note = new QLabel(tr("Note: Please DO NOT map your wheel onto gamepads, only map it here. If your wheel was mapped onto gamepads, go to gamepad settings and unmap it."), this);
    mapping_note->setWordWrap(true);
    v_layout->addWidget(mapping_note);

    m_enabled = new QCheckBox(tr("Enable T500RS Emulator"), this);
    v_layout->addWidget(m_enabled);

    m_reverse_effects = new QCheckBox(tr("Reverse Force Feedback Effects"), this);
    v_layout->addWidget(m_reverse_effects);

    m_state_text = new QLabel(DEFAULT_STATUS, this);
    v_layout->addWidget(m_state_text);

    m_ffb_device = new DeviceChoice(this, tr("Use the device with the following mapping for force feedback:"));
    m_led_device = new DeviceChoice(this, tr("Use the device with the following mapping for LED:"));

    m_mapping_scroll_area = new QScrollArea(this);
    QWidget* mapping_widget = new QWidget(m_mapping_scroll_area);
    QVBoxLayout* mapping_layout = new QVBoxLayout(mapping_widget);
    mapping_widget->setLayout(mapping_layout);
    m_mapping_scroll_area->setWidget(mapping_widget);
    m_mapping_scroll_area->setWidgetResizable(true);
    m_mapping_scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_mapping_scroll_area->setMinimumHeight(400);
    m_mapping_scroll_area->setMinimumWidth(700);

    v_layout->addWidget(m_mapping_scroll_area);

    QLabel* axis_label = new QLabel(tr("Axes:"), mapping_widget);
    mapping_layout->addWidget(axis_label);

    const auto add_axis = [this, mapping_widget, mapping_layout](mapping_device dev, bool flip_axis_display)
    {
        m_mappings[dev] = new Mapping(mapping_widget, this, true, device_name(dev), flip_axis_display);
        mapping_layout->addWidget(m_mappings[dev]);
    };

    const auto add_button = [this, mapping_widget, mapping_layout](mapping_device dev)
    {
        m_mappings[dev] = new Mapping(mapping_widget, this, false, device_name(dev), false);
        mapping_layout->addWidget(m_mappings[dev]);
    };

    add_axis(mapping_device::STEERING, false);
    add_axis(mapping_device::THROTTLE, true);
    add_axis(mapping_device::BRAKE, true);
    add_axis(mapping_device::CLUTCH, true);

    QLabel* button_label = new QLabel(tr("Buttons:"), mapping_widget);
    mapping_layout->addWidget(button_label);

    for (int i = static_cast<int>(mapping_device::FIRST_BUTTON); i < static_cast<int>(mapping_device::COUNT); i++)
    {
        add_button(static_cast<mapping_device>(i));
    }

    v_layout->addSpacing(20);

    v_layout->addWidget(m_ffb_device);
    v_layout->addWidget(m_led_device);

    v_layout->addWidget(buttons);
    setLayout(v_layout);

    m_sdl_initialized = sdl_instance::get_instance().initialize();

    load_ui_state_from_config();

    if (m_sdl_initialized)
        get_joystick_states();
}

emulated_thrustmaster_t500rs_settings_dialog::~emulated_thrustmaster_t500rs_settings_dialog()
{
    for (SDL_Joystick* joystick_handle : m_joystick_handles)
    {
        if (joystick_handle)
            SDL_CloseJoystick(joystick_handle);
    }
}

void emulated_thrustmaster_t500rs_settings_dialog::set_state_text(const QString& text)
{
    m_state_text->setText(text);
}

const std::map<u64, joystick_state>& emulated_thrustmaster_t500rs_settings_dialog::get_joystick_states()
{
    if (!m_sdl_initialized)
    {
        return m_last_joystick_states;
    }

    const u64 now = SDL_GetTicks();

    if (now - m_last_joystick_states_update < 25)
    {
        return m_last_joystick_states;
    }

    m_last_joystick_states_update = now;

    std::map<u64, joystick_state> new_joystick_states;
    std::vector<SDL_Joystick*> new_joystick_handles;

    sdl_instance::get_instance().pump_events();

    int joystick_count = 0;
    if (SDL_JoystickID* joystick_ids = SDL_GetJoysticks(&joystick_count))
    {
        for (int i = 0; i < joystick_count; i++)
        {
            SDL_Joystick* cur_joystick = SDL_OpenJoystick(joystick_ids[i]);
            if (!cur_joystick)
            {
                continue;
            }
            new_joystick_handles.push_back(cur_joystick);

            const int num_axes = SDL_GetNumJoystickAxes(cur_joystick);
            const int num_buttons = SDL_GetNumJoystickButtons(cur_joystick);
            const int num_hats = SDL_GetNumJoystickHats(cur_joystick);
            const emulated_t500rs_device_type_id device_type_id_struct =
            {
                .product_id = static_cast<u64>(SDL_GetJoystickProduct(cur_joystick)),
                .vendor_id = static_cast<u64>(SDL_GetJoystickVendor(cur_joystick)),
                .num_axes = static_cast<u64>(num_axes),
                .num_hats = static_cast<u64>(num_hats),
                .num_buttons = static_cast<u64>(num_buttons)
            };
            const u64 device_type_id = device_type_id_struct.as_u64();

            joystick_state& state = new_joystick_states[device_type_id];
            state.axes.resize(num_axes);
            state.buttons.resize(num_buttons);
            state.hats.resize(num_hats);

            for (int j = 0; j < num_axes; j++)
            {
                state.axes[j] = SDL_GetJoystickAxis(cur_joystick, j);
            }

            for (int j = 0; j < num_buttons; j++)
            {
                state.buttons[j] = SDL_GetJoystickButton(cur_joystick, j);
            }

            for (int j = 0; j < num_hats; j++)
            {
                const u8 hat = SDL_GetJoystickHat(cur_joystick, j);

                if (hat & SDL_HAT_UP)
                {
                    state.hats[j] = hat_component::up;
                }
                else if (hat & SDL_HAT_DOWN)
                {
                    state.hats[j] = hat_component::down;
                }
                else if (hat & SDL_HAT_LEFT)
                {
                    state.hats[j] = hat_component::left;
                }
                else if (hat & SDL_HAT_RIGHT)
                {
                    state.hats[j] = hat_component::right;
                }
                else
                {
                    state.hats[j] = hat_component::none;
                }
            }
        }
    }

    for (SDL_Joystick* joystick_handle : m_joystick_handles)
    {
        if (joystick_handle)
            SDL_CloseJoystick(joystick_handle);
    }

    m_joystick_handles = std::move(new_joystick_handles);
    m_last_joystick_states = std::move(new_joystick_states);

    return m_last_joystick_states;
}

void emulated_thrustmaster_t500rs_settings_dialog::set_enable(bool enable)
{
    const int slider_position = m_mapping_scroll_area->verticalScrollBar()->sliderPosition();

    for (auto& [dev, mapping] : m_mappings)
    {
        mapping->set_enable(enable);
    }

    m_enabled->setEnabled(enable);
    m_reverse_effects->setEnabled(enable);

    m_ffb_device->set_enable(enable);
    m_led_device->set_enable(enable);

    m_mapping_scroll_area->verticalScrollBar()->setEnabled(enable);
    m_mapping_scroll_area->verticalScrollBar()->setSliderPosition(slider_position);
}

void emulated_thrustmaster_t500rs_settings_dialog::load_ui_state_from_config()
{
    const auto load_mapping = [this](mapping_device device_choice)
    {
        const emulated_t500rs_mapping& mapping = device_cfg(device_choice);
        const sdl_mapping m =
        {
            .device_type_id = mapping.device_type_id.get(),
            .type = mapping.type.get(),
            .id = mapping.id.get(),
            .hat = mapping.hat.get(),
            .reverse = mapping.reverse.get(),
            .positive_axis = false
        };

        ::at32(m_mappings, device_choice)->set_mapping(m);

        const u64 ffb_device_type_id = g_cfg_t500rs.ffb_device_type_id.get();
        const u64 led_device_type_id = g_cfg_t500rs.led_device_type_id.get();

        if (ffb_device_type_id == m.device_type_id && m_ffb_device->get_device_choice() == mapping_device::NONE)
        {
            m_ffb_device->set_device_choice(device_choice);
        }
        if (led_device_type_id == m.device_type_id && m_led_device->get_device_choice() == mapping_device::NONE)
        {
            m_led_device->set_device_choice(device_choice);
        }
    };

    for (int i = 0; i < static_cast<int>(mapping_device::COUNT); i++)
    {
        load_mapping(static_cast<mapping_device>(i));
    }

    m_enabled->setChecked(g_cfg_t500rs.enabled.get());
    m_reverse_effects->setChecked(g_cfg_t500rs.reverse_effects.get());
}

void emulated_thrustmaster_t500rs_settings_dialog::save_ui_state_to_config()
{
    const auto save_mapping = [this](mapping_device device_choice)
    {
        emulated_t500rs_mapping& mapping = device_cfg(device_choice);
        const sdl_mapping& m = ::at32(m_mappings, device_choice)->get_mapping();
        mapping.device_type_id.set(m.device_type_id);
        mapping.type.set(m.type);
        mapping.id.set(m.id);
        mapping.hat.set(m.hat);
        mapping.reverse.set(m.reverse);

        if (m_ffb_device->get_device_choice() == device_choice)
        {
            g_cfg_t500rs.ffb_device_type_id.set(m.device_type_id);
        }
        if (m_led_device->get_device_choice() == device_choice)
        {
            g_cfg_t500rs.led_device_type_id.set(m.device_type_id);
        }
    };

    for (int i = 0; i < static_cast<int>(mapping_device::COUNT); i++)
    {
        save_mapping(static_cast<mapping_device>(i));
    }

    g_cfg_t500rs.enabled.set(m_enabled->isChecked());
    g_cfg_t500rs.reverse_effects.set(m_reverse_effects->isChecked());

    if (m_ffb_device->get_device_choice() == mapping_device::NONE)
    {
        g_cfg_t500rs.ffb_device_type_id.set(0);
    }

    if (m_led_device->get_device_choice() == mapping_device::NONE)
    {
        g_cfg_t500rs.led_device_type_id.set(0);
    }
}

#include "emulated_thrustmaster_t500rs_settings_dialog.moc"
#endif 
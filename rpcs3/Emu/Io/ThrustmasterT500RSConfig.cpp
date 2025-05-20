#include "stdafx.h"

#ifdef HAVE_SDL3

#include "Utilities/File.h"
#include "ThrustmasterT500RSConfig.h"


LOG_CHANNEL(cfg_log, "CFG");

emulated_t500rs_config g_cfg_t500rs;

emulated_t500rs_config::emulated_t500rs_config()
    : m_path(fs::get_config_dir(true) + "ThrustmasterT500RS.yml")
{
}

void emulated_t500rs_config::reset()
{
    from_default();
}

void emulated_t500rs_config::save(bool lock_mutex)
{
    if (lock_mutex)
        std::lock_guard lock(m_mutex);

    cfg_log.notice("Saving T500RS config: '%s'", m_path);

    if (!cfg::node::save(m_path))
    {
        cfg_log.error("Failed to save T500RS config to '%s' (error=%s)", m_path, fs::g_tls_error);
    }
}

bool emulated_t500rs_config::load()
{
    std::lock_guard lock(m_mutex);

    cfg_log.notice("Loading T500RS config: %s", m_path);

    from_default();

    if (fs::file cfg_file{m_path, fs::read})
    {
        std::string content;
        content.resize(cfg_file.size());
        if (cfg_file.read(content.data(), content.size()) == content.size())
        {
            return from_string(content);
        }
    }
    else
    {
        save(false);
    }

    return true;
}

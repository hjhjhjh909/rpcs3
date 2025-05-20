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
    const std::lock_guard lock(m_mutex);
    cfg::node::from_default();
}

void emulated_t500rs_config::save(bool lock_mutex)
{
    std::unique_lock lock(m_mutex, std::defer_lock);
    if (lock_mutex)
    {
        lock.lock();
    }
    cfg_log.notice("Saving ThrustmasterT500RS config: '%s'", m_path);

    if (!fs::create_path(fs::get_parent_dir(m_path)))
    {
        cfg_log.fatal("Failed to create path: '%s' (%s)", m_path, fs::g_tls_error);
    }

    if (!cfg::node::save(m_path))
    {
        cfg_log.error("Failed to save ThrustmasterT500RS config to '%s' (error=%s)", m_path, fs::g_tls_error);
    }
}

bool emulated_t500rs_config::load()
{
    const std::lock_guard lock(m_mutex);

    cfg_log.notice("Loading ThrustmasterT500RS config: %s", m_path);

    from_default();

    if (fs::file cfg_file{m_path, fs::read})
    {
        if (const std::string content = cfg_file.to_string(); !content.empty())
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

#endif

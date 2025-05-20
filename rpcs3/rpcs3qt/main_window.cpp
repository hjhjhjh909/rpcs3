#include "main_window.h"
#include "qt_utils.h"
#include "vfs_dialog.h"
#include "save_manager_dialog.h"
#include "trophy_manager_dialog.h"
#include "savestate_manager_dialog.h"
#include "user_manager_dialog.h"
#include "screenshot_manager_dialog.h"
#include "kernel_explorer.h"
#include "game_list_frame.h"
#include "debugger_frame.h"
#include "log_frame.h"
#include "settings_dialog.h"
#include "rpcn_settings_dialog.h"
#include "auto_pause_settings_dialog.h"
#include "cg_disasm_window.h"
#include "log_viewer.h"
#include "memory_viewer_panel.h"
#include "rsx_debugger.h"
#include "about_dialog.h"
#include "pad_settings_dialog.h"
#include "progress_dialog.h"
#include "skylander_dialog.h"
#include "infinity_dialog.h"
#include "dimensions_dialog.h"
#include "cheat_manager.h"
#include "patch_manager_dialog.h"
#include "patch_creator_dialog.h"
#include "pkg_install_dialog.h"
#include "category.h"
#include "gui_settings.h"
#include "input_dialog.h"
#include "camera_settings_dialog.h"
#include "ps_move_tracker_dialog.h"
#include "ipc_settings_dialog.h"
#include "shortcut_utils.h"
#include "config_checker.h"
#include "shortcut_dialog.h"
#include "system_cmd_dialog.h"
#include "emulated_pad_settings_dialog.h"
#include "emulated_logitech_g27_settings_dialog.h"
#include "basic_mouse_settings_dialog.h"
#include "vfs_tool_dialog.h"
#include "welcome_dialog.h"
#include "music_player_dialog.h"
#include "emulated_thrustmaster_t500rs_settings_dialog.h"

#include <thread>
#include <unordered_set>

#include <QScreen>
#include <QDirIterator>
#include <QMimeData>
#include <QMessageBox>
#include <QFileDialog>
#include <QFontDatabase>
#include <QBuffer>
#include <QTemporaryFile>
#include <QDesktopServices>

#include "rpcs3_version.h"
#include "Emu/IdManager.h"
#include "Emu/VFS.h"
#include "Emu/vfs_config.h"
#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/system_config.h"
#include "Emu/savestate_utils.hpp"
#include "Emu/Cell/timers.hpp"

#include "Crypto/unpkg.h"
#include "Crypto/unself.h"
#include "Crypto/unzip.h"
#include "Crypto/decrypt_binaries.h"

#include "Loader/PUP.h"
#include "Loader/TAR.h"
#include "Loader/PSF.h"
#include "Loader/mself.hpp"

#include "Utilities/Thread.h"
#include "util/sysinfo.hpp"
#include "util/serialization_ext.hpp"

#include "Input/gui_pad_thread.h"

#include "ui_main_window.h"

#include <QEventLoop>
#include <QTimer>

#ifdef _WIN32
#include "raw_mouse_settings_dialog.h"
#endif

#if defined(__linux__) || defined(__APPLE__) || (defined(_WIN32) && defined(ARCH_X64))
#define RPCS3_UPDATE_SUPPORTED
#endif

LOG_CHANNEL(gui_log, "GUI");

extern atomic_t<bool> g_user_asked_for_frame_capture;

class CPUDisAsm;
std::shared_ptr<CPUDisAsm> make_basic_ppu_disasm();

inline std::string sstr(const QString& _in) { return _in.toStdString(); }

extern void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	ensure(wrapped_op);

	if (thread_ctrl::is_main())
	{
		// NOTE:
		// I noticed that calling this from an Emu callback can cause the
		// caller to get stuck for a while during newly opened Qt dialogs.
		// Adding a timeout here doesn't seem to do anything in that case.
		QEventLoop* event_loop = nullptr;

		std::shared_ptr<std::function<void()>> check_iteration;
		check_iteration = std::make_shared<std::function<void()>>([&]()
		{
			if (wrapped_op())
			{
				event_loop->exit(0);
			}
			else
			{
				QTimer::singleShot(repeat_duration_ms, *check_iteration);
			}
		});

		while (!wrapped_op())
		{
			// Init event loop
			event_loop = new QEventLoop();

			// Queue event initially
			QTimer::singleShot(0, *check_iteration);

			// Event loop
			event_loop->exec();

			// Cleanup
			event_loop->deleteLater();
		}
	}
	else
	{
		while (!wrapped_op())
		{
			if (repeat_duration_ms == 0)
			{
				std::this_thread::yield();
			}
			else if (thread_ctrl::get_current())
			{
				thread_ctrl::wait_for(repeat_duration_ms * 1000);
			}
			else
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
			}
		}
	}
}

main_window::main_window(std::shared_ptr<gui_settings> gui_settings, std::shared_ptr<emu_settings> emu_settings, std::shared_ptr<persistent_settings> persistent_settings, QWidget *parent)
	: QMainWindow(parent)
	, ui(new Ui::main_window)
	, m_gui_settings(gui_settings)
	, m_emu_settings(std::move(emu_settings))
	, m_persistent_settings(std::move(persistent_settings))
	, m_updater(nullptr, gui_settings)
{
	Q_INIT_RESOURCE(resources);

	// We have to setup the ui before using a translation
	ui->setupUi(this);

	setAttribute(Qt::WA_DeleteOnClose);

	// Add T500RS settings to the USB Devices menu
	QMenu* usb_menu = menuBar()->addMenu(tr("&Configuration"));
	QMenu* usb_devices_menu = usb_menu->addMenu(tr("&USB Devices"));

	QAction* t500rs_settings_action = new QAction(tr("Thrustmaster T500RS Settings"), this);
	connect(t500rs_settings_action, &QAction::triggered, this, &main_window::show_t500rs_settings_dialog);
	usb_devices_menu->addAction(t500rs_settings_action);
}

main_window::~main_window()
{
}

/* An init method is used so that RPCS3App can create the necessary connects before calling init (specifically the stylesheet connect).
 * Simplifies logic a bit.
 */
bool main_window::Init([[maybe_unused]] bool with_cli_boot)
{
	setAcceptDrops(true);

	// add toolbar widgets (crappy Qt designer is not able to)
	ui->toolBar->setObjectName("mw_toolbar");
	ui->sizeSlider->setRange(0, gui::gl_max_slider_pos);
	ui->toolBar->addWidget(ui->sizeSliderContainer);
	ui->toolBar->addWidget(ui->mw_searchbar);

	CreateActions();
	CreateDockWindows();
	CreateConnects();

	setMinimumSize(350, minimumSizeHint().height());    // seems fine on win 10
	setWindowTitle(QString::fromStdString("RPCS3 " + rpcs3::get_verbose_version()));

	Q_EMIT RequestGlobalStylesheetChange();
	ConfigureGuiFromSettings();

	m_shortcut_handler = new shortcut_handler(gui::shortcuts::shortcut_handler_id::main_window, this, m_gui_settings);
	connect(m_shortcut_handler, &shortcut_handler::shortcut_activated, this, &main_window::handle_shortcut);

	show(); // needs to be done before creating the thumbnail toolbar

	// enable play options if a recent game exists
	const bool enable_play_last = !m_recent_game.actions.isEmpty() && m_recent_game.actions.first();

	const QString start_tooltip = enable_play_last ? tr("Play %0").arg(m_recent_game.actions.first()->text()) : tr("Play");

	if (enable_play_last)
	{
		ui->sysPauseAct->setText(tr("&Play last played game"));
		ui->sysPauseAct->setIcon(m_icon_play);
		ui->toolbar_start->setToolTip(start_tooltip);
	}

	ui->sysPauseAct->setEnabled(enable_play_last);
	ui->toolbar_start->setEnabled(enable_play_last);

	// RPCS3 Updater

	QMenu* download_menu = new QMenu(tr("Update Available!"));

	QAction* download_action = new QAction(tr("Download Update"), download_menu);
	connect(download_action, &QAction::triggered, this, [this]
	{
		m_updater.update(false);
	});

	download_menu->addAction(download_action);

#ifdef _WIN32
	// Use a menu at the top right corner to indicate the new version.
	QMenuBar *corner_bar = new QMenuBar(ui->menuBar);
	m_download_menu_action = corner_bar->addMenu(download_menu);
	ui->menuBar->setCornerWidget(corner_bar);
	ui->menuBar->cornerWidget()->setVisible(false);
#else
	// Append a menu to the right of the regular menus to indicate the new version.
	// Some distros just can't handle corner widgets at the moment.
	m_download_menu_action = ui->menuBar->addMenu(download_menu);
#endif

	ensure(m_download_menu_action);
	m_download_menu_action->setVisible(false);

	connect(&m_updater, &update_manager::signal_update_available, this, [this](bool update_available)
	{
		if (m_download_menu_action)
		{
			m_download_menu_action->setVisible(update_available);
		}
		if (ui->menuBar && ui->menuBar->cornerWidget())
		{
			ui->menuBar->cornerWidget()->setVisible(update_available);
		}
	});

#ifdef RPCS3_UPDATE_SUPPORTED
	if (const auto update_value = m_gui_settings->GetValue(gui::m_check_upd_start).toString(); update_value != gui::update_off)
	{
		const bool in_background = with_cli_boot || update_value == gui::update_bkg;
		const bool auto_accept   = !in_background && update_value == gui::update_auto;
		m_updater.check_for_updates(true, in_background, auto_accept, this);
	}
#endif

	// Disable vsh if not present.
	ui->bootVSHAct->setEnabled(fs::is_file(g_cfg_vfs.get_dev_flash() + "vsh/module/vsh.self"));

	// Focus to search bar by default
	ui->mw_searchbar->setFocus();

	// Refresh gamelist last
	m_game_list_frame->Refresh(true);

	update_gui_pad_thread();

	return true;
}

void main_window::update_gui_pad_thread()
{
	const bool enabled = m_gui_settings->GetValue(gui::nav_enabled).toBool();

	if (enabled && Emu.IsStopped())
	{
#if defined(_WIN32) || defined(__linux__) || defined(__APPLE__)
		if (!m_gui_pad_thread)
		{
			m_gui_pad_thread = std::make_unique<gui_pad_thread>();
		}

		m_gui_pad_thread->update_settings(m_gui_settings);
#endif
	}
	else
	{
		m_gui_pad_thread.reset();
	}
}

QString main_window::GetCurrentTitle()
{
	QString title = qstr(Emu.GetTitleAndTitleID());
	if (title.isEmpty())
	{
		title = qstr(Emu.GetLastBoot());
	}
	return title;
}

// returns appIcon
QIcon main_window::GetAppIcon() const
{
	return m_app_icon;
}

void main_window::OnMissingFw()
{
	const QString title = tr("Missing Firmware Detected!");
	const QString message = tr("Commercial games require the firmware (PS3UPDAT.PUP file) to be installed."
	                           "\n<br>For information about how to obtain the required firmware read the <a %0 href=\"https://rpcs3.net/quickstart\">quickstart guide</a>.").arg(gui::utils::get_link_style());

	QMessageBox* mb = new QMessageBox(QMessageBox::Question, title, message, QMessageBox::Ok | QMessageBox::Cancel, this, Qt::Dialog | Qt::MSWindowsFixedSizeDialogHint | Qt::WindowStaysOnTopHint);
	mb->setTextFormat(Qt::RichText);

	mb->button(QMessageBox::Ok)->setText(tr("Locate PS3UPDAT.PUP"));
	mb->setAttribute(Qt::WA_DeleteOnClose);
	mb->open();

	connect(mb, &QDialog::accepted, this, [this]()
	{
		QTimer::singleShot(1, [this](){ InstallPup(); }); // singleShot to avoid a Qt bug that causes a deletion of the sender during long slots.
	});
}

void main_window::ResizeIcons(int index)
{
	if (ui->sizeSlider->value() != index)
	{
		ui->sizeSlider->setSliderPosition(index);
		return; // ResizeIcons will be triggered again by setSliderPosition, so return here
	}

	if (m_save_slider_pos)
	{
		m_save_slider_pos = false;
		m_gui_settings->SetValue(m_is_list_mode ? gui::gl_iconSize : gui::gl_iconSizeGrid, index);

		// this will also fire when we used the actions, but i didn't want to add another boolean member
		SetIconSizeActions(index);
	}

	m_game_list_frame->ResizeIcons(index);
}

void main_window::handle_shortcut(gui::shortcuts::shortcut shortcut_key, const QKeySequence& key_sequence)
{
	gui_log.notice("Main window registered shortcut: %s (%s)", shortcut_key, key_sequence.toString());

	const system_state status = Emu.GetStatus();

	switch (shortcut_key)
	{
	case gui::shortcuts::shortcut::mw_toggle_fullscreen:
	{
		ui->toolbar_fullscreen->trigger();
		break;
	}
	case gui::shortcuts::shortcut::mw_exit_fullscreen:
	{
		if (isFullScreen())
			ui->toolbar_fullscreen->trigger();
		break;
	}
	case gui::shortcuts::shortcut::mw_refresh:
	{
		m_game_list_frame->Refresh(true);
		break;
	}
	case gui::shortcuts::shortcut::mw_pause:
	{
		if (status == system_state::running)
			Emu.Pause();
		break;
	}
	case gui::shortcuts::shortcut::mw_start:
	{
		if (status == system_state::paused)
			Emu.Resume();
		else if (status == system_state::ready)
			Emu.Run(true);
		break;
	}
	case gui::shortcuts::shortcut::mw_restart:
	{
		if (!Emu.GetBoot().empty())
			Emu.Restart();
		break;
	}
	case gui::shortcuts::shortcut::mw_stop:
	{
		if (status != system_state::stopped)
			Emu.GracefulShutdown(false, true);
		break;
	}
	default:
	{
		break;
	}
	}
}

void main_window::OnPlayOrPause()
{
	gui_log.notice("User triggered OnPlayOrPause");

	switch (Emu.GetStatus())
	{
	case system_state::ready: Emu.Run(true); return;
	case system_state::paused: Emu.Resume(); return;
	case system_state::running: Emu.Pause(); return;
	case system_state::stopped:
	{
		if (m_selected_game)
		{
			gui_log.notice("Booting from OnPlayOrPause...");
			Boot(m_selected_game->info.path, m_selected_game->info.serial);
		}
		else if (const std::string path = Emu.GetLastBoot(); !path.empty())
		{
			if (const auto error = Emu.Load(); error != game_boot_result::no_errors)
			{
				gui_log.error("Boot failed: reason: %s, path: %s", error, path);
				show_boot_error(error);
			}
		}
		else if (!m_recent_game.actions.isEmpty())
		{
			BootRecentAction(m_recent_game.actions.first(), false);
		}

		return;
	}
	case system_state::starting: break;
	default: fmt::throw_exception("Unreachable");
	}
}

void main_window::show_boot_error(game_boot_result status)
{
	QString message;
	switch (status)
	{
	case game_boot_result::nothing_to_boot:
		message = tr("No bootable content was found.");
		break;
	case game_boot_result::wrong_disc_location:
		message = tr("Disc could not be mounted properly. Make sure the disc is not in the dev_hdd0/game folder.");
		break;
	case game_boot_result::invalid_file_or_folder:
		message = tr("The selected file or folder is invalid or corrupted.");
		break;
	case game_boot_result::invalid_bdvd_folder:
		message = tr("The virtual dev_bdvd folder does not exist or is not empty.");
		break;
	case game_boot_result::install_failed:
		message = tr("Additional content could not be installed.");
		break;
	case game_boot_result::decryption_error:
		message = tr("Digital content could not be decrypted. This is usually caused by a missing or invalid license (RAP) file.");
		break;
	case game_boot_result::file_creation_error:
		message = tr("The emulator could not create files required for booting.");
		break;
	case game_boot_result::unsupported_disc_type:
		message = tr("This disc type is not supported yet.");
		break;
	case game_boot_result::savestate_corrupted:
		message = tr("Savestate data is corrupted or it's not an RPCS3 savestate.");
		break;
	case game_boot_result::savestate_version_unsupported:
		message = tr("Savestate versioning data differs from your RPCS3 build.");
		break;
	case game_boot_result::still_running:
		message = tr("A game or PS3 application is still running or has yet to be fully stopped.");
		break;
	case game_boot_result::firmware_version:
		message = tr("The game or PS3 application needs a more recent firmware version.");
		break;
	case game_boot_result::firmware_missing: // Handled elsewhere
	case game_boot_result::already_added: // Handled elsewhere
	case game_boot_result::currently_restricted:
	case game_boot_result::no_errors:
		return;
	case game_boot_result::generic_error:
		message = tr("Unknown error.");
		break;
	}
	const QString link = tr("<br /><br />For information on setting up the emulator and dumping your PS3 games, read the <a %0 href=\"https://rpcs3.net/quickstart\">quickstart guide</a>.").arg(gui::utils::get_link_style());

	QMessageBox* msg = new QMessageBox(this);
	msg->setWindowTitle(tr("Boot Failed"));
	msg->setIcon(QMessageBox::Critical);
	msg->setTextFormat(Qt::RichText);
	msg->setStandardButtons(QMessageBox::Ok);
	msg->setText(tr("Booting failed: %1 %2").arg(message).arg(link));
	msg->setAttribute(Qt::WA_DeleteOnClose);
	msg->open();
}

void main_window::Boot(const std::string& path, const std::string& title_id, bool direct, bool refresh_list, cfg_mode config_mode, const std::string& config_path)
{
	if (Emu.IsBootingRestricted())
	{
		return;
	}

	if (!m_gui_settings->GetBootConfirmation(this, gui::ib_confirm_boot))
	{
		return;
	}

	Emu.GracefulShutdown(false);

	m_app_icon = gui::utils::get_app_icon_from_path(path, title_id);

	if (const auto error = Emu.BootGame(path, title_id, direct, config_mode, config_path); error != game_boot_result::no_errors)
	{
		gui_log.error("Boot failed: reason: %s, path: %s", error, path);
		show_boot_error(error);
		return;
	}

	if (is_savestate_compatible(path))
	{
		gui_log.success("Boot of savestate successful.");
		AddRecentAction(gui::Recent_Game(QString::fromStdString(path), QString::fromStdString(Emu.GetTitleAndTitleID())), true);
	}
	else
	{
		gui_log.success("Boot successful.");
		AddRecentAction(gui::Recent_Game(QString::fromStdString(Emu.GetBoot()), QString::fromStdString(Emu.GetTitleAndTitleID())), false);
	}

	if (refresh_list)
	{
		m_game_list_frame->Refresh(true);
	}
}

void main_window::BootElf()
{
	bool stopped = false;

	if (Emu.IsRunning())
	{
		Emu.Pause();
		stopped = true;
	}

	const QString path_last_elf = m_gui_settings->GetValue(gui::fd_boot_elf).toString();
	const QString file_path = QFileDialog::getOpenFileName(this, tr("Select (S)ELF To Boot"), path_last_elf, tr(
		"(S)ELF files (*BOOT.BIN *.elf *.self);;"
		"ELF files (BOOT.BIN *.elf);;"
		"SELF files (EBOOT.BIN *.self);;"
		"BOOT files (*BOOT.BIN);;"
		"BIN files (*.bin);;"
		"All executable files (*.SAVESTAT.zst *.SAVESTAT.gz *.SAVESTAT *.sprx *.SPRX *.self *.SELF *.bin *.BIN *.prx *.PRX *.elf *.ELF *.o *.O);;"
		"All files (*.*)"),
		Q_NULLPTR, QFileDialog::DontResolveSymlinks);

	if (file_path.isEmpty())
	{
		if (stopped)
		{
			Emu.Resume();
		}
		return;
	}

	// If we resolved the filepath earlier we would end up setting the last opened dir to the unwanted
	// game folder in case of having e.g. a Game Folder with collected links to elf files.
	// Don't set last path earlier in case of cancelled dialog
	m_gui_settings->SetValue(gui::fd_boot_elf, file_path);
	const std::string path = sstr(QFileInfo(file_path).absoluteFilePath());

	gui_log.notice("Booting from BootElf...");
	Boot(path, "", true, true);
}

void main_window::BootTest()
{
	bool stopped = false;

	if (Emu.IsRunning())
	{
		Emu.Pause();
		stopped = true;
	}

#ifdef _WIN32
	const QString path_tests = QString::fromStdString(fs::get_config_dir()) + "/test/";
#elif defined(__linux__)
	const QString path_tests = QCoreApplication::applicationDirPath() + "/../share/rpcs3/test/";
#else
	const QString path_tests = QCoreApplication::applicationDirPath() + "/../Resources/test/";
#endif

	const QString file_path = QFileDialog::getOpenFileName(this, tr("Select (S)ELF To Boot"), path_tests, tr(
		"(S)ELF files (*.elf *.self);;"
		"ELF files (*.elf);;"
		"SELF files (*.self);;"
		"All files (*.*)"),
		Q_NULLPTR, QFileDialog::DontResolveSymlinks);

	if (file_path.isEmpty())
	{
		if (stopped)
		{
			Emu.Resume();
		}
		return;
	}

	const std::string path = sstr(QFileInfo(file_path).absoluteFilePath());

	gui_log.notice("Booting from BootTest...");
	Boot(path, "", true);
}

void main_window::BootSavestate()
{
	bool stopped = false;

	if (Emu.IsRunning())
	{
		Emu.Pause();
		stopped = true;
	}

	const QString file_path = QFileDialog::getOpenFileName(this, tr("Select Savestate To Boot"), qstr(fs::get_config_dir() + "savestates/"), tr(
		"Savestate files (*.SAVESTAT *.SAVESTAT.zst *.SAVESTAT.gz);;"
		"All files (*.*)"),
		Q_NULLPTR, QFileDialog::DontResolveSymlinks);

	if (file_path.isEmpty())
	{
		if (stopped)
		{
			Emu.Resume();
		}
		return;
	}

	const std::string path = sstr(QFileInfo(file_path).absoluteFilePath());

	gui_log.notice("Booting from BootSavestate...");
	Boot(path, "", true);
}

void main_window::BootGame()
{
	bool stopped = false;

	if (Emu.IsRunning())
	{
		Emu.Pause();
		stopped = true;
	}

	const QString path_last_game = m_gui_settings->GetValue(gui::fd_boot_game).toString();
	const QString dir_path = QFileDialog::getExistingDirectory(this, tr("Select Game Folder"), path_last_game, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (dir_path.isEmpty())
	{
		if (stopped)
		{
			Emu.Resume();
		}
		return;
	}

	m_gui_settings->SetValue(gui::fd_boot_game, QFileInfo(dir_path).path());

	gui_log.notice("Booting from BootGame...");
	Boot(sstr(dir_path), "", false, true);
}

void main_window::BootVSH()
{
	gui_log.notice("Booting from BootVSH...");
	Boot(g_cfg_vfs.get_dev_flash() + "/vsh/module/vsh.self");
}

void main_window::BootRsxCapture(std::string path)
{
	if (path.empty())
	{
		bool is_stopped = false;

		if (Emu.IsRunning())
		{
			Emu.Pause();
			is_stopped = true;
		}

		const QString file_path = QFileDialog::getOpenFileName(this, tr("Select RSX Capture"), qstr(fs::get_config_dir() + "captures/"), tr("RRC files (*.rrc *.RRC *.rrc.gz *.RRC.GZ);;All files (*.*)"));

		if (file_path.isEmpty())
		{
			if (is_stopped)
			{
				Emu.Resume();
			}
			return;
		}
		path = sstr(file_path);
	}

	if (!m_gui_settings->GetBootConfirmation(this))
	{
		return;
	}

	Emu.GracefulShutdown(false);

	if (!Emu.BootRsxCapture(path))
	{
		gui_log.error("Capture Boot Failed. path: %s", path);
	}
	else
	{
		gui_log.success("Capture Boot Success. path: %s", path);
	}
}

bool main_window::InstallFileInExData(const std::string& extension, const QString& path, const std::string& filename)
{
	if (path.isEmpty() || filename.empty() || extension.empty())
	{
		return false;
	}

	// Copy file atomically with thread/process-safe error checking for file size
	const std::string to_path = rpcs3::utils::get_hdd0_dir() + "/home/" + Emu.GetUsr() + "/exdata/" + filename.substr(0, filename.find_last_of('.'));
	fs::pending_file to(to_path + "." + extension);
	fs::file from(sstr(path));

	if (!to.file || !from)
	{
		return false;
	}

	to.file.write(from.to_vector<u8>());
	from.close();

	if (to.file.size() < 0x10)
	{
		// Not a RAP file
		return false;
	}

#ifdef _WIN32
	// In the case of an unexpected crash during the operation, the temporary file can be used as the deleted file
	// See below
	to.file.sync();

	// In case we want to rename upper-case file to lower-case
	// Windows will ignore such rename operation if the file exists
	// So delete it
	fs::remove_file(to_path + "." + fmt::to_upper(extension));
#endif

	return to.commit();
}

bool main_window::InstallPackages(QStringList file_paths, bool from_boot)
{
	if (file_paths.isEmpty())
	{
		ensure(!from_boot);

		// If this function was called without a path, ask the user for files to install.
		const QString path_last_pkg = m_gui_settings->GetValue(gui::fd_install_pkg).toString();
		const QStringList paths = QFileDialog::getOpenFileNames(this, tr("Select packages and/or rap files to install"),
			path_last_pkg, tr("All relevant (*.pkg *.PKG *.rap *.RAP *.edat *.EDAT);;Package files (*.pkg *.PKG);;Rap files (*.rap *.RAP);;Edat files (*.edat *.EDAT);;All files (*.*)"));

		if (paths.isEmpty())
		{
			return true;
		}

		file_paths.append(paths);
		const QFileInfo file_info(file_paths[0]);
		m_gui_settings->SetValue(gui::fd_install_pkg, file_info.path());
	}

	if (file_paths.count() == 1)
	{
		const QString file_path = file_paths.front();
		const QFileInfo file_info(file_path);

		if (file_info.isDir())
		{
			gui_log.notice("PKG: Trying to install packages from dir: '%s'", file_path);

			const QDir dir(file_path);
			const QStringList dir_file_paths = gui::utils::get_dir_entries(dir, {}, true);

			if (dir_file_paths.empty())
			{
				gui_log.notice("PKG: Could not find any files in dir: '%s'", file_path);
				return true;
			}

			return InstallPackages(dir_file_paths, from_boot);
		}

		if (file_info.suffix().compare("pkg", Qt::CaseInsensitive) == 0)
		{
			compat::package_info info = game_compatibility::GetPkgInfo(file_path, m_game_list_frame ? m_game_list_frame->GetGameCompatibility() : nullptr);

			if (!info.is_valid)
			{
				QMessageBox::warning(this, tr("Invalid package!"), tr("The selected package is invalid!\n\nPath:\n%0").arg(file_path));
				return false;
			}

			if (info.type != compat::package_type::other)
			{
				if (info.type == compat::package_type::dlc)
				{
					info.local_cat = tr("\nDLC", "Block for package type (DLC)");
				}
				else
				{
					info.local_cat = tr("\nUpdate", "Block for package type (Update)");
				}
			}
			else if (!info.local_cat.isEmpty())
			{
				info.local_cat = tr("\n%0", "Block for package type").arg(info.local_cat);
			}

			if (!info.title_id.isEmpty())
			{
				info.title_id = tr("\n%0", "Block for Title ID").arg(info.title_id);
			}

			if (!info.version.isEmpty())
			{
				info.version = tr("\nVersion %0", "Block for Version").arg(info.version);
			}

			if (!info.changelog.isEmpty())
			{
				info.changelog = tr("Changelog:\n%0", "Block for Changelog").arg(info.changelog);
			}

			const QString info_string = QStringLiteral("%0\n\n%1%2%3%4").arg(file_info.fileName()).arg(info.title).arg(info.local_cat).arg(info.title_id).arg(info.version);
			QString message = tr("Do you want to install this package?\n\n%0").arg(info_string);

			QMessageBox mb(QMessageBox::Icon::Question, tr("PKG Decrypter / Installer"), message, QMessageBox::Yes | QMessageBox::No, this);
			mb.setDefaultButton(QMessageBox::No);

			if (!info.changelog.isEmpty())
			{
				mb.setInformativeText(tr("To see the changelog, please click \"Show Details\"."));
				mb.setDetailedText(info.changelog);

				// Smartass hack to make the unresizeable message box wide enough for the changelog
				const int log_width = QLabel(info.changelog).sizeHint().width();
				while (QLabel(message).sizeHint().width() < log_width)
				{
					message += "          ";
				}

				mb.setText(message);
			}

			if (mb.exec() != QMessageBox::Yes)
			{
				gui_log.notice("PKG: Cancelled installation from drop.\n%s\n%s", info_string, info.changelog);
				return true;
			}
		}
	}

	// Install rap files if available
	int installed_rap_and_edat_count = 0;

	const auto install_filetype = [&installed_rap_and_edat_count, &file_paths](const std::string extension)
	{
		const QString pattern = QString(".*\\.%1").arg(QString::fromStdString(extension));
		for (const QString& file : file_paths.filter(QRegularExpression(pattern, QRegularExpression::PatternOption::CaseInsensitiveOption)))
		{
			const QFileInfo file_info(file);
			const std::string filename = sstr(file_info.fileName());

			if (InstallFileInExData(extension, file, filename))
			{
				gui_log.success("Successfully copied %s file: %s", extension, filename);
				installed_rap_and_edat_count++;
			}
			else
			{
				gui_log.error("Could not copy %s file: %s", extension, filename);
			}
		}
	};

	if (!from_boot)
	{
		if (!m_gui_settings->GetBootConfirmation(this))
		{
			// Last chance to cancel the operation
			return true;
		}

		if (!Emu.IsStopped())
		{
			Emu.GracefulShutdown(false);
		}

		install_filetype("rap");
		install_filetype("edat");
	}

	if (installed_rap_and_edat_count > 0)
	{
		// Refresh game list since we probably unlocked some games now.
		m_game_list_frame->Refresh(true);
	}

	// Find remaining package files
	file_paths = file_paths.filter(QRegularExpression(".*\\.pkg", QRegularExpression::PatternOption::CaseInsensitiveOption));

	if (file_paths.isEmpty())
	{
		return true;
	}

	if (from_boot)
	{
		return HandlePackageInstallation(file_paths, true);
	}

	// Handle further installations with a timeout. Otherwise the source explorer instance is not usable during the following file processing.
	QTimer::singleShot(0, [this, paths = std::move(file_paths)]()
	{
		HandlePackageInstallation(paths, false);
	});

	return true;
}

bool main_window::HandlePackageInstallation(QStringList file_paths, bool from_boot)
{
	if (file_paths.empty())
	{
		return false;
	}

	std::vector<compat::package_info> packages;

	game_compatibility* compat = m_game_list_frame ? m_game_list_frame->GetGameCompatibility() : nullptr;

	if (file_paths.size() > 1)
	{
		// Let the user choose the packages to install and select the order in which they shall be installed.
		pkg_install_dialog dlg(file_paths, compat, this);
		connect(&dlg, &QDialog::accepted, this, [&packages, &dlg]()
		{
			packages = dlg.GetPathsToInstall();
		});
		dlg.exec();
	}
	else
	{
		packages.push_back(game_compatibility::GetPkgInfo(file_paths.front(), compat));
	}

	if (packages.empty())
	{
		return true;
	}

	if (!from_boot)
	{
		if (!m_gui_settings->GetBootConfirmation(this))
		{
			return true;
		}

		Emu.GracefulShutdown(false);
	}

	std::vector<std::string> path_vec;
	for (const compat::package_info& pkg : packages)
	{
		path_vec.push_back(pkg.path.toStdString());
	}
	gui_log.notice("About to install packages:\n%s", fmt::merge(path_vec, "\n"));

	progress_dialog pdlg(tr("RPCS3 Package Installer"), tr("Installing package, please wait..."), tr("Cancel"), 0, 1000, false, this);
	pdlg.setAutoClose(false);
	pdlg.show();

	package_install_result result = {};

	auto get_app_info = [](compat::package_info& package)
	{
		QString app_info = package.title; // This should always be non-empty

		if (!package.title_id.isEmpty() || !package.version.isEmpty())
		{
			app_info += QStringLiteral("\n");

			if (!package.title_id.isEmpty())
			{
				app_info += package.title_id;
			}

			if (!package.version.isEmpty())
			{
				if (!package.title_id.isEmpty())
				{
					app_info += " ";
				}

				app_info += tr("v.%0", "Package version for install progress dialog").arg(package.version);
			}
		}

		return app_info;
	};

	bool cancelled = false;

	std::deque<package_reader> readers;

	for (const compat::package_info& info : packages)
	{
		readers.emplace_back(sstr(info.path));
	}

	std::deque<std::string> bootable_paths;

	// Run PKG unpacking asynchronously
	named_thread worker("PKG Installer", [&readers, &result, &bootable_paths]
	{
		result = package_reader::extract_data(readers, bootable_paths);
		return result.error == package_install_result::error_type::no_error;
	});

	pdlg.show();

	// Wait for the completion
	int reader_it = 0;
	int set_text = -1;

	qt_events_aware_op(5, [&, readers_size = ::narrow<int>(readers.size())]()
	{
		if (reader_it == readers_size || result.error != package_install_result::error_type::no_error)
		{
			// Exit loop
			return true;
		}

		if (pdlg.wasCanceled())
		{
			cancelled = true;

			for (package_reader& reader : readers)
			{
				reader.abort_extract();
			}

			// Exit loop
			return true;
		}

		// Update progress window
		const int progress = readers[reader_it].get_progress(pdlg.maximum());
		pdlg.SetValue(progress);

		if (set_text != reader_it)
		{
			pdlg.setLabelText(tr("Installing package (%0/%1), please wait...\n\n%2").arg(reader_it + 1).arg(readers_size).arg(get_app_info(packages[reader_it])));
			set_text = reader_it;
		}

		if (progress == pdlg.maximum())
		{
			reader_it++;
		}

		// Process events
		return false;
	});

	const bool success = worker();

	if (success)
	{
		pdlg.SetValue(pdlg.maximum());

		const u64 start_time = get_system_time();

		for (usz i = 0; i < packages.size(); i++)
		{
			const compat::package_info& package = ::at32(packages, i);
			const package_reader& reader = ::at32(readers, i);

			switch (reader.get_result())
			{
			case package_reader::result::success:
			{
				gui_log.success("Successfully installed %s (title_id=%s, title=%s, version=%s).", package.path, package.title_id, package.title, package.version);
				break;
			}
			case package_reader::result::not_started:
			case package_reader::result::started:
			case package_reader::result::aborted:
			{
				gui_log.notice("Aborted installation of %s (title_id=%s, title=%s, version=%s).", package.path, package.title_id, package.title, package.version);
				break;
			}
			case package_reader::result::error:
			{
				gui_log.error("Failed to install %s (title_id=%s, title=%s, version=%s).", package.path, package.title_id, package.title, package.version);
				break;
			}
			case package_reader::result::aborted_dirty:
			case package_reader::result::error_dirty:
			{
				gui_log.error("Partially installed %s (title_id=%s, title=%s, version=%s).", package.path, package.title_id, package.title, package.version);
				break;
			}
			}
		}

		std::map<std::string, QString> bootable_paths_installed; // -> title id

		for (usz index = 0; index < bootable_paths.size(); index++)
		{
			if (bootable_paths[index].empty())
			{
				continue;
			}

			bootable_paths_installed[bootable_paths[index]] = packages[index].title_id;
		}

		// Need to test here due to potential std::move later
		const bool installed_a_whole_package_without_new_software = bootable_paths_installed.empty() && !cancelled;

		if (!bootable_paths_installed.empty())
		{
			m_game_list_frame->AddRefreshedSlot([this, paths = std::move(bootable_paths_installed)](std::set<std::string>& claimed_paths) mutable
			{
				// Try to claim operaions on ID
				for (auto it = paths.begin(); it != paths.end();)
				{
					std::string resolved_path = Emu.GetCallbacks().resolve_path(it->first);

					if (resolved_path.empty() || claimed_paths.count(resolved_path))
					{
						it = paths.erase(it);
					}
					else
					{
						claimed_paths.emplace(std::move(resolved_path));
						it++;
					}
				}

				ShowOptionalGamePreparations(tr("Success!"), tr("Successfully installed software from package(s)!"), std::move(paths));
			});
		}

		m_game_list_frame->Refresh(true);

		std::this_thread::sleep_for(std::chrono::microseconds(100'000 - std::min<usz>(100'000, get_system_time() - start_time)));
		pdlg.hide();

		if (installed_a_whole_package_without_new_software)
		{
			m_gui_settings->ShowInfoBox(tr("Success!"), tr("Successfully installed software from package(s)!"), gui::ib_pkg_success, this);
		}
	}
	else
	{
		pdlg.hide();
		pdlg.SignalFailure();

		if (!cancelled)
		{
			const compat::package_info* package = nullptr;

			for (usz i = 0; i < readers.size() && !package; i++)
			{
				// Figure out what package failed the installation
				switch (readers[i].get_result())
				{
				case package_reader::result::success:
				case package_reader::result::not_started:
				case package_reader::result::started:
				case package_reader::result::aborted:
				case package_reader::result::aborted_dirty:
					break;
				case package_reader::result::error:
				case package_reader::result::error_dirty:
					package = &packages[i];
					break;
				}
			}

			ensure(package);

			if (result.error == package_install_result::error_type::app_version)
			{
				gui_log.error("Cannot install %s.", package->path);
				const bool has_expected = !result.version.expected.empty();
				const bool has_found = !result.version.found.empty();
				if (has_expected && has_found)
				{
					QMessageBox::warning(this, tr("Warning!"), tr("Package cannot be installed on top of the current data.\nUpdate is for version %1, but you have version %2.\n\nTried to install: %3")
							.arg(QString::fromStdString(result.version.expected)).arg(QString::fromStdString(result.version.found)).arg(package->path));
				}
				else if (has_expected)
				{
					QMessageBox::warning(this, tr("Warning!"), tr("Package cannot be installed on top of the current data.\nUpdate is for version %1, but you don't have any data installed.\n\nTried to install: %2")
							.arg(QString::fromStdString(result.version.expected)).arg(package->path));
				}
				else
				{
					// probably unreachable
					const QString found = has_found ? tr("version %1").arg(QString::fromStdString(result.version.found)) : tr("no data installed");
					QMessageBox::warning(this, tr("Warning!"), tr("Package cannot be installed on top of the current data.\nUpdate is for unknown version, but you have version %1.\n\nTried to install: %2")
							.arg(QString::fromStdString(result.version.expected)).arg(found).arg(package->path));
				}
			}
			else
			{
				gui_log.error("Failed to install %s.", package->path);
				QMessageBox::critical(this, tr("Failure!"), tr("Failed to install software from package:\n%1!"
					"\nThis is very likely caused by external interference from a faulty anti-virus software."
					"\nPlease add RPCS3 to your anti-virus\' whitelist or use better anti-virus software.").arg(package->path));
			}
		}
	}

	return success;
}

void main_window::ExtractMSELF()
{
	const QString path_last_mself = m_gui_settings->GetValue(gui::fd_ext_mself).toString();
	const QString file_path = QFileDialog::getOpenFileName(this, tr("Select MSELF To extract"), path_last_mself, tr("All mself files (*.mself *.MSELF);;All files (*.*)"));

	if (file_path.isEmpty())
	{
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Extraction Directory"), QString{}, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (!dir.isEmpty())
	{
		m_gui_settings->SetValue(gui::fd_ext_mself, QFileInfo(file_path).path());
		extract_mself(sstr(file_path), sstr(dir) + '/');
	}
}

void main_window::InstallPup(QString file_path)
{
	if (file_path.isEmpty())
	{
		const QString path_last_pup = m_gui_settings->GetValue(gui::fd_install_pup).toString();
		file_path = QFileDialog::getOpenFileName(this, tr("Select PS3UPDAT.PUP To Install"), path_last_pup, tr("PS3 update file (PS3UPDAT.PUP);;All pup files (*.pup *.PUP);;All files (*.*)"));
	}
	else
	{
		if (QMessageBox::question(this, tr("RPCS3 Firmware Installer"), tr("Install firmware: %1?").arg(file_path),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		{
			gui_log.notice("Firmware: Cancelled installation from drop. File: %s", file_path);
			return;
		}
	}

	if (!file_path.isEmpty())
	{
		// Handle the actual installation with a timeout. Otherwise the source explorer instance is not usable during the following file processing.
		QTimer::singleShot(0, [this, file_path]()
		{
			HandlePupInstallation(file_path);
		});
	}
}

void main_window::ExtractPup()
{
	const QString path_last_pup = m_gui_settings->GetValue(gui::fd_install_pup).toString();
	const QString file_path = QFileDialog::getOpenFileName(this, tr("Select PS3UPDAT.PUP To extract"), path_last_pup, tr("PS3 update file (PS3UPDAT.PUP);;All pup files (*.pup *.PUP);;All files (*.*)"));

	if (file_path.isEmpty())
	{
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Extraction Directory"), QString{}, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (!dir.isEmpty())
	{
		HandlePupInstallation(file_path, dir);
	}
}

void main_window::ExtractTar()
{
	if (!m_gui_settings->GetBootConfirmation(this))
	{
		return;
	}

	Emu.GracefulShutdown(false);

	const QString path_last_tar = m_gui_settings->GetValue(gui::fd_ext_tar).toString();
	QStringList files = QFileDialog::getOpenFileNames(this, tr("Select TAR To extract"), path_last_tar, tr("All tar files (*.tar *.TAR *.tar.aa.* *.TAR.AA.*);;All files (*.*)"));

	if (files.isEmpty())
	{
		return;
	}

	const QString dir = QFileDialog::getExistingDirectory(this, tr("Extraction Directory"), QString{}, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

	if (dir.isEmpty())
	{
		return;
	}

	m_gui_settings->SetValue(gui::fd_ext_tar, QFileInfo(files[0]).path());

	progress_dialog pdlg(tr("TAR Extraction"), tr("Extracting encrypted TARs\nPlease wait..."), tr("Cancel"), 0, files.size(), false, this);
	pdlg.show();

	QString error;

	auto files_it = files.begin();
	int pdlg_progress = 0;

	qt_events_aware_op(0, [&]()
	{
		if (pdlg.wasCanceled() || files_it == files.end())
		{
			// Exit loop
			return true;
		}

		const QString& file = *files_it;

		// Do not abort on failure here, in case the user selected a wrong file in multi-selection while the rest are valid
		if (!extract_tar(sstr(file), sstr(dir) + '/'))
		{
			if (error.isEmpty())
			{
				error = tr("The following TAR file(s) could not be extracted:");
			}

			error += "\n";
			error += file;
		}

		pdlg_progress++;
		pdlg.SetValue(pdlg_progress);

		files_it++;
		return false;
	});

	if (!error.isEmpty())
	{
		pdlg.hide();
		QMessageBox::critical(this, tr("TAR extraction failed"), error);
	}
}

void main_window::HandlePupInstallation(const QString& file_path, const QString& dir_path)
{
	const auto critical = [this](QString str)
	{
		Emu.CallFromMainThread([this, str = std::move(str)]()
		{
			QMessageBox::critical(this, tr("Firmware Installation Failed"), str);
		}, nullptr, false);
	};

	if (file_path.isEmpty())
	{
		gui_log.error("Error while installing firmware: provided path is empty.");
		critical(tr("Firmware installation failed: The provided path is empty."));
		return;
	}

	if (!m_gui_settings->GetBootConfirmation(this))
	{
		return;
	}

	Emu.GracefulShutdown(false);

	m_gui_settings->SetValue(gui::fd_install_pup, QFileInfo(file_path).path());

	const std::string path = sstr(file_path);

	fs::file pup_f(path);
	if (!pup_f)
	{
		gui_log.error("Error opening PUP file %s (%s)", path, fs::g_tls_error);
		critical(tr("Firmware installation failed: The selected firmware file couldn't be opened."));
		return;
	}

	pup_object pup(std::move(pup_f));

	switch (pup.operator pup_error())
	{
	case pup_error::header_read:
	{
		gui_log.error("%s", pup.get_formatted_error());
		critical(tr("Firmware installation failed: The provided file is empty."));
		return;
	}
	case pup_error::header_magic:
	{
		gui_log.error("Error while installing firmware: provided file is not a PUP file.");
		critical(tr("Firmware installation failed: The provided file is not a PUP file."));
		return;
	}
	case pup_error::expected_size:
	{
		gui_log.error("%s", pup.get_formatted_error());
		critical(tr("Firmware installation failed: The provided file is incomplete. Try redownloading it."));
		return;
	}
	case pup_error::header_file_count:
	case pup_error::file_entries:
	case pup_error::stream:
	{
		std::string error = "Error while installing firmware: PUP file is invalid.";

		if (!pup.get_formatted_error().empty())
		{
			fmt::append(error, "\n%s", pup.get_formatted_error());
		}

		gui_log.error("%s", error);
		critical(tr("Firmware installation failed: The provided file is corrupted."));
		return;
	}
	case pup_error::hash_mismatch:
	{
		gui_log.error("Error while installing firmware: Hash check failed.");
		critical(tr("Firmware installation failed: The provided file's contents are corrupted."));
		return;
	}
	case pup_error::ok: break;
	}

	fs::file update_files_f = pup.get_file(0x300);

	const usz update_files_size = update_files_f ? update_files_f.size() : 0;

	if (!update_files_size)
	{
		gui_log.error("Error while installing firmware: Couldn't find installation packages database.");
		critical(tr("Firmware installation failed: The provided file's contents are corrupted."));
		return;
	}

	fs::device_stat dev_stat{};
	if (!fs::statfs(g_cfg_vfs.get_dev_flash(), dev_stat))
	{
		gui_log.error("Error while installing firmware: Couldn't retrieve available disk space. ('%s')", g_cfg_vfs.get_dev_flash());
		critical(tr("Firmware installation failed: Couldn't retrieve available disk space."));
		return;
	}

	if (dev_stat.avail_free < update_files_size)
	{
		gui_log.error("Error while installing firmware: Out of disk space. ('%s', needed: %d bytes)", g_cfg_vfs.get_dev_flash(), update_files_size - dev_stat.avail_free);
		critical(tr("Firmware installation failed: Out of disk space."));
		return;
	}

	tar_object update_files(update_files_f);

	if (!dir_path.isEmpty())
	{
		// Extract only mode, extract direct TAR entries to a user directory

		if (!vfs::mount("/pup_extract", sstr(dir_path) + '/'))
		{
			gui_log.error("Error while extracting firmware: Failed to mount '%s'", dir_path);
			critical(tr("Firmware extraction failed: VFS mounting failed."));
			return;
		}

		if (!update_files.extract("/pup_extract", true))
		{
			gui_log.error("Error while installing firmware: TAR contents are invalid.");
			critical(tr("Firmware installation failed: Firmware contents could not be extracted."));
		}

		gui_log.success("Extracted PUP file to %s", dir_path);
		return;
	}

	// In regular installation we select specfic entries from the main TAR which are prefixed with "dev_flash_"
	// Those entries are TAR as well, we extract their packed files from them and that's what installed in /dev_flash

	auto update_filenames = update_files.get_filenames();

	update_filenames.erase(std::remove_if(
		update_filenames.begin(), update_filenames.end(), [](const std::string& s) { return s.find("dev_flash_") == umax; }),
		update_filenames.end());

	if (update_filenames.empty())
	{
		gui_log.error("Error while installing firmware: No dev_flash_* packages were found.");
		critical(tr("Firmware installation failed: The provided file's contents are corrupted."));
		return;
	}

	static constexpr std::string_view cur_version = "4.92";

	std::string version_string;

	if (fs::file version = pup.get_file(0x100))
	{
		version_string = version.to_string();
	}

	if (const usz version_pos = version_string.find('\n'); version_pos != umax)
	{
		version_string.erase(version_pos);
	}

	if (version_string.empty())
	{
		gui_log.error("Error while installing firmware: No version data was found.");
		critical(tr("Firmware installation failed: The provided file's contents are corrupted."));
		return;
	}

	if (version_string < cur_version &&
		QMessageBox::question(this, tr("RPCS3 Firmware Installer"), tr("Old firmware detected.\nThe newest firmware version is %1 and you are trying to install version %2\nContinue installation?").arg(QString::fromUtf8(cur_version.data(), ::size32(cur_version)), qstr(version_string)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::No)
	{
		return;
	}

	if (const std::string installed = utils::get_firmware_version(); !installed.empty())
	{
		gui_log.warning("Reinstalling firmware: old=%s, new=%s", installed, version_string);

		if (QMessageBox::question(this, tr("RPCS3 Firmware Installer"), tr("Firmware of version %1 has already been installed.\nOverwrite current installation with version %2?").arg(qstr(installed), qstr(version_string)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes) == QMessageBox::No)
		{
			gui_log.warning("Reinstallation of firmware aborted.");
			return;
		}
	}

	// Remove possibly PS3 fonts from database
	QFontDatabase::removeAllApplicationFonts();

	progress_dialog pdlg(tr("RPCS3 Firmware Installer"), tr("Installing firmware version %1\nPlease wait...").arg(qstr(version_string)), tr("Cancel"), 0, static_cast<int>(update_filenames.size()), false, this);
	pdlg.show();

	// Used by tar_object::extract() as destination directory
	vfs::mount("/dev_flash", g_cfg_vfs.get_dev_flash());

	// Synchronization variable
	atomic_t<uint> progress(0);
	{
		// Run asynchronously
		named_thread worker("Firmware Installer", [&]
		{
			for (const auto& update_filename : update_filenames)
			{
				auto update_file_stream = update_files.get_file(update_filename);

				if (update_file_stream->m_file_handler)
				{
					// Forcefully read all the data
					update_file_stream->m_file_handler->handle_file_op(*update_file_stream, 0, update_file_stream->get_size(umax), nullptr);
				}

				fs::file update_file = fs::make_stream(std::move(update_file_stream->data));

				SCEDecrypter self_dec(update_file);
				self_dec.LoadHeaders();
				self_dec.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV);
				self_dec.DecryptData();

				auto dev_flash_tar_f = self_dec.MakeFile();
				if (dev_flash_tar_f.size() < 3)
				{
					gui_log.error("Error while installing firmware: PUP contents are invalid.");
					critical(tr("Firmware installation failed: Firmware could not be decompressed"));
					progress = -1;
					return;
				}

				tar_object dev_flash_tar(dev_flash_tar_f[2]);
				if (!dev_flash_tar.extract())
				{
					gui_log.error("Error while installing firmware: TAR contents are invalid. (package=%s)", update_filename);
					critical(tr("The firmware contents could not be extracted."
						"\nThis is very likely caused by external interference from a faulty anti-virus software."
						"\nPlease add RPCS3 to your anti-virus\' whitelist or use better anti-virus software."));

					progress = -1;
					return;
				}

				if (!progress.try_inc(::narrow<uint>(update_filenames.size())))
				{
					// Installation was cancelled
					return;
				}
			}
		});

		// Wait for the completion
		qt_events_aware_op(5, [&]()
		{
			const uint value = progress.load();

			if (value >= update_filenames.size())
			{
				return true;
			}

			if (pdlg.wasCanceled())
			{
				progress = -1;
				return true;
			}

			// Update progress window
			pdlg.SetValue(static_cast<int>(value));
			return false;
		});

		// Join thread
		worker();
	}

	update_files_f.close();

	if (progress == update_filenames.size())
	{
		pdlg.SetValue(pdlg.maximum());
		std::this_thread::sleep_for(100ms);
	}

	// Update with newly installed PS3 fonts
	Q_EMIT RequestGlobalStylesheetChange();

	// Unmount
	Emu.Init();

	if (progress == update_filenames.size())
	{
		ui->bootVSHAct->setEnabled(fs::is_file(g_cfg_vfs.get_dev_flash() + "/vsh/module/vsh.self"));

		gui_log.success("Successfully installed PS3 firmware version %s.", version_string);
		m_gui_settings->ShowInfoBox(tr("Success!"), tr("Successfully installed PS3 firmware and LLE Modules!"), gui::ib_pup_success, this);

		CreateFirmwareCache();
	}
}

void main_window::DecryptSPRXLibraries()
{
	QString path_last_sprx = m_gui_settings->GetValue(gui::fd_decrypt_sprx).toString();

	if (!fs::is_dir(sstr(path_last_sprx)))
	{
		// Default: redirect to userland firmware SPRX directory
		path_last_sprx = qstr(g_cfg_vfs.get_dev_flash() + "sys/external");
	}

	const QStringList modules = QFileDialog::getOpenFileNames(this, tr("Select binary files"), path_last_sprx, tr("All Binaries (*.bin *.BIN *.self *.SELF *.sprx *.SPRX *.sdat *.SDAT *.edat *.EDAT);;"
		"BIN files (*.bin *.BIN);;SELF files (*.self *.SELF);;SPRX files (*.sprx *.SPRX);;SDAT/EDAT files (*.sdat *.SDAT *.edat *.EDAT);;All files (*.*)"));

	if (modules.isEmpty())
	{
		return;
	}

	m_gui_settings->SetValue(gui::fd_decrypt_sprx, QFileInfo(modules.first()).path());

	std::vector<std::string> vec_modules;
	for (const QString& mod : modules)
	{
		vec_modules.push_back(mod.toStdString());
	}

	auto iterate = std::make_shared<std::function<void(usz, usz)>>();
	const auto decrypter = std::make_shared<decrypt_binaries_t>(std::move(vec_modules));

	*iterate = [this, iterate, decrypter](usz mod_index, usz repeat_count)
	{
		const std::string& path = (*decrypter)[mod_index];
		const std::string filename = path.substr(path.find_last_of(fs::delim) + 1);

		const QString hint = tr("Hint: KLIC (KLicense key) is a 16-byte long string. (32 hexadecimal characters, can be prefixed with \"KLIC=0x\" from the log message)"
			"\nAnd is logged with some sceNpDrm* functions when the game/application which owns \"%0\" is running.").arg(qstr(filename));

		if (repeat_count >= 2)
		{
			gui_log.error("Failed to decrypt %s with specified KLIC, retrying.\n%s", path, hint);
		}

		input_dialog* dlg = new input_dialog(39, "", tr("Enter KLIC of %0").arg(qstr(filename)),
			repeat_count >= 2 ? tr("Decryption failed with provided KLIC.\n%0").arg(hint) : tr("Hexadecimal value."), "KLIC=0x00000000000000000000000000000000", this);

		QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
		mono.setPointSize(8);
		dlg->set_input_font(mono, true, '0');
		dlg->set_clear_button_enabled(false);
		dlg->set_button_enabled(QDialogButtonBox::StandardButton::Ok, false);
		dlg->set_validator(new QRegularExpressionValidator(QRegularExpression("^((((((K?L)?I)?C)?=)?0)?x)?[a-fA-F0-9]{0,32}$"), this)); // HEX only (with additional KLIC=0x prefix for convenience)
		dlg->setAttribute(Qt::WA_DeleteOnClose);

		connect(dlg, &input_dialog::text_changed, dlg, [dlg](const QString& text)
		{
			dlg->set_button_enabled(QDialogButtonBox::StandardButton::Ok, text.size() - (text.indexOf('x') + 1) == 32);
		});

		connect(dlg, &QDialog::accepted, this, [this, iterate, dlg, mod_index, decrypter, repeat_count]()
		{
			std::string text = sstr(dlg->get_input_text());

			if (usz new_index = decrypter->decrypt(std::move(text)); !decrypter->done())
			{
				QTimer::singleShot(0, [iterate, mod_index, repeat_count, new_index]()
				{
					// Increase repeat count if "stuck" on the same file
					(*iterate)(new_index, new_index == mod_index ? repeat_count + 1 : 0);
				});
			}
		});

		connect(dlg, &QDialog::rejected, this, []()
		{
			gui_log.notice("User has cancelled entering KLIC.");
		});

		dlg->show();
	};

	if (usz new_index = decrypter->decrypt(); !decrypter->done())
	{
		(*iterate)(new_index, new_index == 0 ? 1 : 0);
	}
}

/** Needed so that when a backup occurs of window state in gui_settings, the state is current.
* Also, so that on close, the window state is preserved.
*/
void main_window::SaveWindowState() const
{
	// Save gui settings
	m_gui_settings->SetValue(gui::mw_geometry, saveGeometry(), false);
	m_gui_settings->SetValue(gui::mw_windowState, saveState(), false);

	// NOTE:
	//
	// This method is also invoked in case the gui_application::Init() method failed ("false" was returned)
	// to initialize some modules leaving other modules uninitialized (NULL pointed).
	// So, the following checks on NULL pointer are provided before accessing the related module's object

	if (m_mw)
	{
		m_gui_settings->SetValue(gui::mw_mwState, m_mw->saveState(), true);
	}

	if (m_game_list_frame)
	{
		// Save column settings
		m_game_list_frame->SaveSettings();
	}

	if (m_debugger_frame)
	{
		// Save splitter state
		m_debugger_frame->SaveSettings();
	}
}

void main_window::RepaintThumbnailIcons()
{
	const QColor color = gui::utils::get_foreground_color();
	[[maybe_unused]] const QColor new_color = gui::utils::get_label_color("thumbnail_icon_color", color, color);

	[[maybe_unused]] const auto icon = [&new_color](const QString& path)
	{
		return gui::utils::get_colorized_icon(QPixmap::fromImage(gui::utils::get_opaque_image_area(path)), Qt::black, new_color);
	};
}

void main_window::RepaintToolBarIcons()
{
	const QColor color = gui::utils::get_foreground_color();

	std::map<QIcon::Mode, QColor> new_colors{};
	new_colors[QIcon::Normal] = gui::utils::get_label_color("toolbar_icon_color", color, color);
	new_colors[QIcon::Disabled] = gui::utils::get_label_color("toolbar_icon_color_disabled", Qt::gray, Qt::lightGray);
	new_colors[QIcon::Active] = gui::utils::get_label_color("toolbar_icon_color_active", color, color);
	new_colors[QIcon::Selected] = gui::utils::get_label_color("toolbar_icon_color_selected", color, color);

	const auto icon = [&new_colors](const QString& path)
	{
		return gui::utils::get_colorized_icon(QIcon(path), Qt::black, new_colors);
	};

	m_icon_play           = icon(":/Icons/play.png");
	m_icon_pause          = icon(":/Icons/pause.png");
	m_icon_restart        = icon(":/Icons/restart.png");
	m_icon_fullscreen_on  = icon(":/Icons/fullscreen.png");
	m_icon_fullscreen_off = icon(":/Icons/exit_fullscreen.png");

	ui->toolbar_config  ->setIcon(icon(":/Icons/configure.png"));
	ui->toolbar_controls->setIcon(icon(":/Icons/controllers.png"));
	ui->toolbar_open    ->setIcon(icon(":/Icons/open.png"));
	ui->toolbar_grid    ->setIcon(icon(":/Icons/grid.png"));
	ui->toolbar_list    ->setIcon(icon(":/Icons/list.png"));
	ui->toolbar_refresh ->setIcon(icon(":/Icons/refresh.png"));
	ui->toolbar_stop    ->setIcon(icon(":/Icons/stop.png"));

	ui->sysStopAct->setIcon(icon(":/Icons/stop.png"));
	ui->sysRebootAct->setIcon(m_icon_restart);

	if (Emu.IsRunning())
	{
		ui->toolbar_start->setIcon(m_icon_pause);
		ui->sysPauseAct->setIcon(m_icon_pause);
	}
	else if (Emu.IsStopped() && !Emu.GetBoot().empty())
	{
		ui->toolbar_start->setIcon(m_icon_restart);
		ui->sysPauseAct->setIcon(m_icon_restart);
	}
	else
	{
		ui->toolbar_start->setIcon(m_icon_play);
		ui->sysPauseAct->setIcon(m_icon_play);
	}

	if (isFullScreen())
	{
		ui->toolbar_fullscreen->setIcon(m_icon_fullscreen_off);
	}
	else
	{
		ui->toolbar_fullscreen->setIcon(m_icon_fullscreen_on);
	}

	const QColor& new_color = new_colors[QIcon::Normal];
	ui->sizeSlider->setStyleSheet(ui->sizeSlider->styleSheet().append("QSlider::handle:horizontal{ background: rgba(%1, %2, %3, %4); }")
		.arg(new_color.red()).arg(new_color.green()).arg(new_color.blue()).arg(new_color.alpha()));

	// resize toolbar elements

	const int tool_bar_height = ui->toolBar->sizeHint().height();

	for (const auto& act : ui->toolBar->actions())
	{
		if (act->isSeparator())
		{
			continue;
		}

		ui->toolBar->widgetForAction(act)->setMinimumWidth(tool_bar_height);
	}

	ui->sizeSliderContainer->setFixedWidth(tool_bar_height * 4);
	ui->mw_searchbar->setFixedWidth(tool_bar_height * 5);
}

void main_window::OnEmuRun(bool /*start_playtime*/)
{
	const QString title = GetCurrentTitle();
	const QString restart_tooltip = tr("Restart %0").arg(title);
	const QString pause_tooltip = tr("Pause %0").arg(title);
	const QString stop_tooltip = tr("Stop %0").arg(title);

	m_debugger_frame->EnableButtons(true);

	ui->sysPauseAct->setText(tr("&Pause"));
	ui->sysPauseAct->setIcon(m_icon_pause);
	ui->toolbar_start->setIcon(m_icon_pause);
	ui->toolbar_start->setText(tr("Pause"));
	ui->toolbar_start->setToolTip(pause_tooltip);
	ui->toolbar_stop->setToolTip(stop_tooltip);

	EnableMenus(true);

	update_gui_pad_thread();
}

void main_window::OnEmuResume() const
{
	const QString title = GetCurrentTitle();
	const QString restart_tooltip = tr("Restart %0").arg(title);
	const QString pause_tooltip = tr("Pause %0").arg(title);
	const QString stop_tooltip = tr("Stop %0").arg(title);

	ui->sysPauseAct->setText(tr("&Pause"));
	ui->sysPauseAct->setIcon(m_icon_pause);
	ui->toolbar_start->setIcon(m_icon_pause);
	ui->toolbar_start->setText(tr("Pause"));
	ui->toolbar_start->setToolTip(pause_tooltip);
	ui->toolbar_stop->setToolTip(stop_tooltip);
}

void main_window::OnEmuPause() const
{
	const QString title = GetCurrentTitle();
	const QString resume_tooltip = tr("Resume %0").arg(title);

	ui->sysPauseAct->setText(tr("&Resume"));
	ui->sysPauseAct->setIcon(m_icon_play);
	ui->toolbar_start->setIcon(m_icon_play);
	ui->toolbar_start->setText(tr("Play"));
	ui->toolbar_start->setToolTip(resume_tooltip);

	// Refresh game list in order to update time played
	if (m_game_list_frame)
	{
		m_game_list_frame->Refresh();
	}
}

void main_window::OnEmuStop()
{
	const QString title = GetCurrentTitle();
	const QString play_tooltip = tr("Play %0").arg(title);

	ui->sysPauseAct->setText(tr("&Play"));
	ui->sysPauseAct->setIcon(m_icon_play);

	EnableMenus(false);

	if (title.isEmpty())
	{
		ui->toolbar_start->setIcon(m_icon_play);
		ui->toolbar_start->setText(tr("Play"));
		ui->toolbar_start->setToolTip(play_tooltip);
	}
	else
	{
		const QString restart_tooltip = tr("Restart %0").arg(title);

		ui->toolbar_start->setEnabled(true);
		ui->toolbar_start->setIcon(m_icon_restart);
		ui->toolbar_start->setText(tr("Restart"));
		ui->toolbar_start->setToolTip(restart_tooltip);
		ui->sysRebootAct->setEnabled(true);
	}

	ui->batchRemoveShaderCachesAct->setEnabled(true);
	ui->batchRemovePPUCachesAct->setEnabled(true);
	ui->batchRemoveSPUCachesAct->setEnabled(true);
	ui->removeHDD1CachesAct->setEnabled(true);
	ui->removeAllCachesAct->setEnabled(true);
	ui->removeSavestatesAct->setEnabled(true);
	ui->cleanUpGameListAct->setEnabled(true);

	ui->actionManage_Users->setEnabled(true);
	ui->confCamerasAct->setEnabled(true);
	ui->actionPS_Move_Tracker->setEnabled(true);

	// Refresh game list in order to update time played
	if (m_game_list_frame && m_is_list_mode)
	{
		m_game_list_frame->Refresh();
	}

	// Close kernel explorer if running
	if (m_kernel_explorer)
	{
		m_kernel_explorer->close();
	}

	// Close systen command dialog if running
	if (m_system_cmd_dialog)
	{
		m_system_cmd_dialog->close();
	}

	update_gui_pad_thread();
}

void main_window::OnEmuReady() const
{
	const QString title = GetCurrentTitle();
	const QString play_tooltip = tr("Play %0").arg(title);

	m_debugger_frame->EnableButtons(true);

	ui->sysPauseAct->setText(tr("&Play"));
	ui->sysPauseAct->setIcon(m_icon_play);
	ui->toolbar_start->setIcon(m_icon_play);
	ui->toolbar_start->setText(tr("Play"));
	ui->toolbar_start->setToolTip(play_tooltip);

	EnableMenus(true);

	ui->actionManage_Users->setEnabled(false);
	ui->confCamerasAct->setEnabled(false);
	ui->actionPS_Move_Tracker->setEnabled(false);

	ui->batchRemoveShaderCachesAct->setEnabled(false);
	ui->batchRemovePPUCachesAct->setEnabled(false);
	ui->batchRemoveSPUCachesAct->setEnabled(false);
	ui->removeHDD1CachesAct->setEnabled(false);
	ui->removeAllCachesAct->setEnabled(false);
	ui->removeSavestatesAct->setEnabled(false);
	ui->cleanUpGameListAct->setEnabled(false);
}

void main_window::EnableMenus(bool enabled) const
{
	// Toolbar
	ui->toolbar_start->setEnabled(enabled);
	ui->toolbar_stop->setEnabled(enabled);

	// Emulation
	ui->sysPauseAct->setEnabled(enabled);
	ui->sysStopAct->setEnabled(enabled);
	ui->sysRebootAct->setEnabled(enabled);

	// Tools
	ui->toolskernel_explorerAct->setEnabled(enabled);
	ui->toolsmemory_viewerAct->setEnabled(enabled);
	ui->toolsRsxDebuggerAct->setEnabled(enabled);
	ui->toolsSystemCommandsAct->setEnabled(enabled);
	ui->actionCreate_RSX_Capture->setEnabled(enabled);
	ui->actionCreate_Savestate->setEnabled(enabled);
}

void main_window::OnAddBreakpoint(u32 addr) const
{
	if (m_debugger_frame)
	{
		m_debugger_frame->PerformAddBreakpointRequest(addr);
	}
}

void main_window::OnEnableDiscEject(bool enabled) const
{
	ui->ejectDiscAct->setEnabled(enabled);
}

void main_window::OnEnableDiscInsert(bool enabled) const
{
	ui->insertDiscAct->setEnabled(enabled);
}

void main_window::BootRecentAction(const QAction* act, bool is_savestate)
{
	if (Emu.IsRunning())
	{
		return;
	}

	recent_game_wrapper& rgw = is_savestate ? m_recent_save : m_recent_game;
	QMenu* menu = is_savestate ? ui->bootRecentSavestatesMenu : ui->bootRecentMenu;

	void main_window::show_t500rs_settings_dialog()
	{
		emulated_thrustmaster_t500rs_settings_dialog dialog(this);
		dialog.exec();
	}
}

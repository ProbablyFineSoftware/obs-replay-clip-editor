/*
Replay Clip Editor for OBS Studio
Copyright (C) 2026 Terra Firma Entertainment <terrafirmaentertainment@gmail.com>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>
#include <util/config-file.h>

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>

#include "PreviewPlayer.hpp"
#include "ReplayClipEditorDialog.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ReplayClipEditorDialog *dialog = nullptr;
static obs_hotkey_id open_hotkey_id = OBS_INVALID_HOTKEY_ID;
static obs_hotkey_id clip_hotkey_id = OBS_INVALID_HOTKEY_ID;
static bool pending_clip_that = false;

static ReplayClipEditorDialog *ensure_dialog()
{
	if (!dialog) {
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new ReplayClipEditorDialog(main_window);
	}
	return dialog;
}

static void open_editor()
{
	ensure_dialog()->showBrowser();
}

static QString replay_folder_from_config()
{
	config_t *cfg = obs_frontend_get_profile_config();
	if (!cfg)
		return QString();
	const char *mode = config_get_string(cfg, "Output", "Mode");
	const char *path = (mode && strcmp(mode, "Advanced") == 0) ? config_get_string(cfg, "AdvOut", "RecFilePath")
								   : config_get_string(cfg, "SimpleOutput", "FilePath");
	return path ? QString::fromUtf8(path) : QString();
}

static const char *kWorkDirName = "ReplayClipEditor";

/* One-time migration: remove the old hidden-style temp folder. */
static void cleanup_legacy_temp_dir()
{
	QString folder = replay_folder_from_config();
	if (folder.isEmpty())
		return;
	QDir legacy(folder + QStringLiteral("/.replay-clip-editor-temp"));
	if (legacy.exists())
		legacy.removeRecursively();
}

/* Steam-style one-shot: save the replay buffer and jump straight into the
 * editor with the result. Runs on the UI thread. */
void request_clip_that()
{
	if (!obs_frontend_replay_buffer_active()) {
		/* Arm it for next time (no-op when disabled in settings),
		 * tell the user, and fall back to the clip browser. */
		obs_frontend_replay_buffer_start();
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		QMessageBox::information(main_window, QString::fromUtf8(obs_module_text("ReplayClipEditor.Title")),
					 QString::fromUtf8(obs_module_text("ReplayClipEditor.ClipThat.NotActive")));
		open_editor();
		return;
	}
	pending_clip_that = true;
	/* Show the window right away — muxing a long buffer takes a while. */
	ensure_dialog()->showSavingReplay();
	obs_frontend_replay_buffer_save();
}

static void open_editor_with_clip(const QString &path)
{
	ensure_dialog()->openClipExternal(path);
}

/* Add a dedicated "Replay Clip Editor" menu to the OBS main menu bar,
 * inserted just before the Help menu. */
static void add_main_menu()
{
	static bool added = false;
	if (added)
		return;
	auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	if (!main_window)
		return;
	QMenuBar *bar = main_window->menuBar();
	if (!bar)
		return;
	added = true;

	QMenu *menu = new QMenu(QString::fromUtf8(obs_module_text("ReplayClipEditor.Menu")), bar);
	menu->addAction(QString::fromUtf8(obs_module_text("ReplayClipEditor.Menu.OpenBrowser")), [] { open_editor(); });
	menu->addAction(QString::fromUtf8(obs_module_text("ReplayClipEditor.Menu.OpenEditor")),
			[] { request_clip_that(); });

	const QList<QAction *> acts = bar->actions();
	if (!acts.isEmpty())
		bar->insertMenu(acts.last(), menu); /* before Help */
	else
		bar->addMenu(menu);
}

static void on_open_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	/* Hotkeys fire outside the UI thread; hop over to it. */
	QMetaObject::invokeMethod(qApp, [] { open_editor(); }, Qt::QueuedConnection);
}

static void on_clip_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	QMetaObject::invokeMethod(qApp, [] { request_clip_that(); }, Qt::QueuedConnection);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
	if (event == OBS_FRONTEND_EVENT_FINISHED_LOADING) {
		add_main_menu();
		cleanup_legacy_temp_dir();
		/* optional auto-start of the replay buffer */
		char *path = obs_module_config_path("settings.json");
		obs_data_t *data = path ? obs_data_create_from_json_file(path) : nullptr;
		bfree(path);
		if (data) {
			if (obs_data_get_bool(data, "auto_start_replay_buffer") && !obs_frontend_replay_buffer_active())
				obs_frontend_replay_buffer_start();
			obs_data_release(data);
		}
		return;
	}

	if (event == OBS_FRONTEND_EVENT_REPLAY_BUFFER_SAVED && pending_clip_that) {
		pending_clip_that = false;

		QString path;
		obs_output_t *output = obs_frontend_get_replay_buffer_output();
		if (output) {
			calldata_t cd = {};
			proc_handler_t *ph = obs_output_get_proc_handler(output);
			if (ph && proc_handler_call(ph, "get_last_replay", &cd))
				path = QString::fromUtf8(calldata_string(&cd, "path"));
			calldata_free(&cd);
			obs_output_release(output);
		}

		if (!path.isEmpty()) {
			/* Move the working copy into a normal subfolder of the
			 * clips folder, always under the same name — each new
			 * Clip That replaces the previous one (same volume, so
			 * this is an instant rename). */
			QFileInfo fi(path);
			QString workDir = fi.absolutePath() + QStringLiteral("/") + QString::fromUtf8(kWorkDirName);
			if (QDir().mkpath(workDir)) {
				QString workPath = workDir + QStringLiteral("/Replay.") + fi.suffix();
				if (QFile::exists(workPath))
					QFile::remove(workPath);
				if (QFile::rename(path, workPath))
					path = workPath;
			}
			QMetaObject::invokeMethod(qApp, [path] { open_editor_with_clip(path); }, Qt::QueuedConnection);
		} else {
			obs_log(LOG_WARNING, "Clip That: replay saved but no path reported");
		}
		return;
	}

	if (event == OBS_FRONTEND_EVENT_EXIT || event == OBS_FRONTEND_EVENT_SCRIPTING_SHUTDOWN) {
		/* The dialog owns an obs_display and an obs_source; they must be
		 * torn down while the core is still alive. */
		if (dialog) {
			dialog->shutdown();
			delete dialog;
			dialog = nullptr;
		}
	}
}

bool obs_module_load(void)
{
	register_preview_source();

	/* The menu bar is not ready yet at load; add_main_menu() runs on the
	 * frontend-finished-loading event instead. */

	open_hotkey_id = obs_hotkey_register_frontend(
		"replay-clip-editor.open", obs_module_text("ReplayClipEditor.Hotkey.Open"), on_open_hotkey, nullptr);
	clip_hotkey_id = obs_hotkey_register_frontend("replay-clip-editor.clip-that",
						      obs_module_text("ReplayClipEditor.Hotkey.ClipThat"),
						      on_clip_hotkey, nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (open_hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(open_hotkey_id);
	if (clip_hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(clip_hotkey_id);
	obs_log(LOG_INFO, "plugin unloaded");
}

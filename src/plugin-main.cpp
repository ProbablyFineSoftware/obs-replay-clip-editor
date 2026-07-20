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

#include <QApplication>
#include <QMainWindow>
#include <QMetaObject>

#include "PreviewPlayer.hpp"
#include "ReplayClipEditorDialog.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

static ReplayClipEditorDialog *dialog = nullptr;
static obs_hotkey_id open_hotkey_id = OBS_INVALID_HOTKEY_ID;

static void open_editor()
{
	if (!dialog) {
		auto *main_window = static_cast<QMainWindow *>(obs_frontend_get_main_window());
		dialog = new ReplayClipEditorDialog(main_window);
	}
	dialog->showBrowser();
}

static void on_hotkey(void *, obs_hotkey_id, obs_hotkey_t *, bool pressed)
{
	if (!pressed)
		return;
	/* Hotkeys fire outside the UI thread; hop over to it. */
	QMetaObject::invokeMethod(qApp, [] { open_editor(); }, Qt::QueuedConnection);
}

static void on_frontend_event(enum obs_frontend_event event, void *)
{
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

	obs_frontend_add_tools_menu_item(obs_module_text("ReplayClipEditor.MenuItem"),
					 [](void *) { open_editor(); }, nullptr);

	open_hotkey_id = obs_hotkey_register_frontend("replay-clip-editor.open",
						      obs_module_text("ReplayClipEditor.Hotkey.Open"), on_hotkey,
						      nullptr);

	obs_frontend_add_event_callback(on_frontend_event, nullptr);

	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(on_frontend_event, nullptr);
	if (open_hotkey_id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(open_hotkey_id);
	obs_log(LOG_INFO, "plugin unloaded");
}

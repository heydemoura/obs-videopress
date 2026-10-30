// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock-theme.hpp"
#include "recording-watcher.hpp"
#include "upload-controller.hpp"
#include "videopress-dock.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <settings-store.hpp>

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QDockWidget>
#include <QMainWindow>
#include <QPointer>
#include <QSslSocket>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *kDockId = "videopress";

// Lives until process exit: the dock belongs to the OBS main window, which may outlive
// obs_module_unload(), and keeps a pointer to the settings.
videopress::SettingsStore *g_settings = nullptr;
RecordingWatcher *g_watcher = nullptr;
QPointer<VideoPressDock> g_dock;
QPointer<UploadController> g_uploads;

QString moduleConfigPath(const char *file)
{
	char *path = obs_module_config_path(file);
	const QString result = QString::fromUtf8(path);
	bfree(path);
	return result;
}

// OBS ships Qt without a TLS backend because OBS itself does HTTPS through curl. The
// plugin bundles one (Contents/PlugIns/tls) and must point Qt at it before the first HTTPS
// request, or every request fails with "TLS initialization failed".
void registerBundledQtPlugins()
{
	QDir dir = QFileInfo(QString::fromUtf8(obs_get_module_binary_path(obs_current_module()))).dir();
#if defined(__APPLE__)
	// obs-videopress.plugin/Contents/MacOS -> Contents/PlugIns
	if (dir.cdUp() && dir.cd(QStringLiteral("PlugIns")))
		QCoreApplication::addLibraryPath(dir.absolutePath());
#elif defined(_WIN32)
	// obs-videopress/bin/64bit -> bin/64bit/qt-plugins
	if (dir.cd(QStringLiteral("qt-plugins")))
		QCoreApplication::addLibraryPath(dir.absolutePath());
#else
	// Linux: OBS uses the distribution's Qt, which ships its TLS backends.
	Q_UNUSED(dir);
#endif
	const QString backend = QSslSocket::activeBackend();
	if (backend.isEmpty() || !QSslSocket::supportsSsl())
		obs_log(LOG_WARNING, "no Qt TLS backend available; HTTPS requests will fail (backends: %s)",
			QSslSocket::availableBackends().join(QStringLiteral(", ")).toUtf8().constData());
	else
		obs_log(LOG_INFO, "Qt TLS backend: %s", backend.toUtf8().constData());
}

void saveSettings()
{
	QString error;
	if (g_settings && !g_settings->save(&error))
		obs_log(LOG_WARNING, "could not save settings: %s", error.toUtf8().constData());
}

void showDock()
{
	if (!g_dock)
		return;
	if (auto *dock = qobject_cast<QDockWidget *>(g_dock->parentWidget())) {
		dock->show();
		dock->raise();
	}
}

void updatePause()
{
	if (!g_uploads || !g_settings)
		return;
	char *profile = obs_frontend_get_current_profile();
	const bool pauseWhileStreaming = g_settings->profile(QString::fromUtf8(profile)).pauseWhileStreaming;
	bfree(profile);
	g_uploads->setPaused(pauseWhileStreaming && obs_frontend_streaming_active());
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		updatePause();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
		if (g_dock)
			g_dock->reloadProfile();
		updatePause();
		break;
	case OBS_FRONTEND_EVENT_THEME_CHANGED:
		if (g_dock)
			g_dock->applyTheme();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		saveSettings();
		break;
	default:
		break;
	}
}

} // namespace

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("VideoPress.Name");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("VideoPress.Description");
}

bool obs_module_load(void)
{
	registerBundledQtPlugins();

	g_settings = new videopress::SettingsStore(moduleConfigPath("settings.json"));
	QString error;
	if (!g_settings->load(&error))
		obs_log(LOG_WARNING, "could not read settings, using defaults: %s", error.toUtf8().constData());

	auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
	DockTheme::loadBundledFonts();

	auto *dock = new VideoPressDock(g_settings, saveSettings, mainWindow);
	if (!obs_frontend_add_dock_by_id(kDockId, obs_module_text("VideoPress.Dock"), dock)) {
		obs_log(LOG_ERROR, "could not add the VideoPress dock");
		delete dock;
		return true;
	}
	g_dock = dock;
	g_uploads = dock->uploads();

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("VideoPress.Menu")));
	QObject::connect(action, &QAction::triggered, [] { showDock(); });

	g_watcher = new RecordingWatcher();
	QObject::connect(g_watcher, &RecordingWatcher::recordingReady,
			 [](const QString &path, qint64 durationMs, const QString &resolutionLabel) {
				 if (g_dock)
					 g_dock->handleRecording(path, durationMs, resolutionLabel);
			 });

	obs_frontend_add_event_callback(onFrontendEvent, nullptr);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	delete g_watcher;
	g_watcher = nullptr;
	saveSettings();
	obs_log(LOG_INFO, "plugin unloaded");
}

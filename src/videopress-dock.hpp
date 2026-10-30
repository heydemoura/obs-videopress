// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dock-theme.hpp"

#include <QHash>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QSet>
#include <QTimer>
#include <QUrl>
#include <QWidget>

#include <functional>

class BlueprintFrame;
class DropOverlay;
class QLabel;
class QLineEdit;
class QMimeData;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
class RecordingRow;
class SelectBox;
class UploadController;

namespace videopress {
class SettingsStore;
}

// The VideoPress dock, as designed in the claude.ai/design project "VideoPress Dock"
// (states 1a–1e): site picker, the recordings list with selection and per-row status,
// drag-and-drop uploads, and a footer with the auto-upload toggle and the batch action.
class VideoPressDock : public QWidget {
	Q_OBJECT

public:
	// Owns the upload queue, so uploads stop when the OBS main window (and this dock) goes.
	VideoPressDock(videopress::SettingsStore *settings, std::function<void()> saveSettings,
		       QWidget *parent = nullptr);

	UploadController *uploads() const { return m_uploads; }

	// A finished recording: upload it now or list it ticked, per the profile setting.
	void handleRecording(const QString &path, qint64 durationMs, const QString &resolutionLabel);
	// The OBS profile changed: show that profile's site and options.
	void reloadProfile();
	// OBS switched themes: recompute the tokens and restyle.
	void applyTheme();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void changeEvent(QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dragMoveEvent(QDragMoveEvent *event) override;
	void dragLeaveEvent(QDragLeaveEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	QString currentProfile() const;
	qint64 activeBlogId() const;
	QString titleFor(const QString &path) const;

	void refreshSites();
	void selectSite(qint64 blogId);
	void showSiteMenu();
	void addSite(const QString &address = QString());
	void removeSite();
	void continueFromEmptyState();
	void chooseFiles();
	void uploadFiles(const QStringList &paths);
	void setAutoUpload(bool enabled);

	void addRow(int jobId);
	void updateRow(int jobId);
	void removeRow(int jobId);
	void updateListState();
	void updateFooter();
	void positionDropOverlay();
	void requestPoster(int jobId, const QUrl &url);

	static QStringList videoFiles(const QMimeData *mime);

	videopress::SettingsStore *m_settings;
	UploadController *m_uploads;
	std::function<void()> m_saveSettings;
	DockTheme m_theme;
	bool m_applyingTheme = false;

	// No site yet: ask for the address right here.
	QWidget *m_sitesEmpty;
	QLabel *m_sitesEmptyTitle;
	QLineEdit *m_emptyAddress;
	QLabel *m_emptyError;
	QPushButton *m_emptyContinue;

	// Site field
	QWidget *m_siteField;
	QPushButton *m_sitePicker;
	QLabel *m_siteName;
	QLabel *m_siteDomain;
	QLabel *m_siteChevron;
	QPushButton *m_addSite;
	QPushButton *m_removeSite;

	// Recordings header and list
	QLabel *m_listTitle;
	QPushButton *m_uploadFile;
	QWidget *m_listFrame;
	QStackedWidget *m_listStack;
	QWidget *m_emptyList;
	QLabel *m_emptyIcon;
	QLabel *m_emptyTitle;
	QPushButton *m_chooseFile;
	QVBoxLayout *m_rows;
	DropOverlay *m_dropOverlay;
	QHash<int, RecordingRow *> m_rowWidgets;

	// Footer
	SelectBox *m_autoUpload;
	QLabel *m_autoLabel = nullptr;
	QLabel *m_footerText;
	BlueprintFrame *m_uploadFrame;
	QPushButton *m_uploadSelected;
	QPushButton *m_cancelAll;
	QLabel *m_hint;

	QNetworkAccessManager m_network;
	QHash<QUrl, QPixmap> m_posters;
	QSet<QUrl> m_posterRequests;
	QTimer m_relativeTimeTicker;
};

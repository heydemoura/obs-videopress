// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "dock-icons.hpp"
#include "dock-theme.hpp"
#include "upload-controller.hpp"

#include <QAbstractButton>
#include <QLabel>
#include <QPixmap>
#include <QWidget>

class QHBoxLayout;
class QProgressBar;
class QPushButton;

// Wraps a child in a 6 px margin and draws the design system's "+" registration marks at
// its corners: the frame primary buttons and framed objects wear.
class BlueprintFrame : public QWidget {
public:
	BlueprintFrame(QWidget *child, const DockTheme *theme, QWidget *parent = nullptr);
	QWidget *child() const { return m_child; }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QWidget *m_child;
	const DockTheme *m_theme;
};

// Square 16 px checkbox: hairline box, accent fill and check when ticked.
class SelectBox : public QAbstractButton {
public:
	SelectBox(const DockTheme *theme, QWidget *parent = nullptr);
	QSize sizeHint() const override { return QSize(16, 16); }

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	const DockTheme *m_theme;
};

// 64x36 slot: diagonal hatching until VideoPress has a poster, plus the duration badge.
class Thumbnail : public QWidget {
public:
	Thumbnail(const DockTheme *theme, QWidget *parent = nullptr);
	void setPoster(const QPixmap &poster);
	void setDuration(const QString &duration);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	const DockTheme *m_theme;
	QPixmap m_poster;
	QString m_duration;
};

// A one-line label that elides with "…" instead of growing the dock.
class ElidedLabel : public QLabel {
public:
	using QLabel::QLabel;
	void setFullText(const QString &text);
	QSize minimumSizeHint() const override;

protected:
	void resizeEvent(QResizeEvent *event) override;

private:
	void updateElided();
	QString m_full;
};

// The "Drop to upload" target shown while a file is dragged over the dock.
class DropOverlay : public QWidget {
public:
	DropOverlay(const DockTheme *theme, QWidget *parent = nullptr);
	void setDetails(const QString &file, const QString &site);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	const DockTheme *m_theme;
	QString m_file;
	QString m_site;
};

// One recording in the list, in any of its states (mockup 1b–1e).
class RecordingRow : public QWidget {
	Q_OBJECT

public:
	RecordingRow(const DockTheme *theme, QWidget *parent = nullptr);
	void setJob(const UploadController::Job &job);
	void setPoster(const QPixmap &poster);
	void refreshTheme();
	int jobId() const { return m_job.id; }

signals:
	void selectToggled(bool selected);
	void cancelClicked();
	void retryClicked();
	void copyLinkClicked();
	void openLinkClicked();
	void removeRequested();
	void showFileRequested();

protected:
	void paintEvent(QPaintEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private:
	void render();
	QString metaText() const;

	const DockTheme *m_theme;
	UploadController::Job m_job;

	SelectBox *m_select;
	QLabel *m_status;
	Thumbnail *m_thumb;
	ElidedLabel *m_title;
	ElidedLabel *m_meta;
	QLabel *m_tag;

	QWidget *m_progressRow;
	QProgressBar *m_progress;
	QLabel *m_progressText;
	QPushButton *m_cancel;

	QWidget *m_linkRow;
	ElidedLabel *m_link;
	QPushButton *m_copy;
	QPushButton *m_open;

	QWidget *m_errorRow;
	QLabel *m_error;
	QPushButton *m_retry;
};

namespace dock_format {
QString bytes(qint64 bytes);
QString duration(qint64 ms);
QString timeLeft(qint64 seconds);
QString relative(const QDateTime &when);
QString videoUrl(const QString &guid);
} // namespace dock_format

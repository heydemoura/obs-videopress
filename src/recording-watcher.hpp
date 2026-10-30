// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <obs-frontend-api.h>
#include <obs.h>

#include <QHash>
#include <QObject>
#include <QString>

class QTimer;

// Turns OBS recording events into "this file is finished and safe to upload".
//
// - A recording ends with OBS_FRONTEND_EVENT_RECORDING_STOPPED; obs_frontend_get_last_recording()
//   names the file.
// - With automatic file splitting, each finished part is announced only by the recording
//   output's file_changed(string next_file) signal. There is no frontend event for it.
// - "Automatically remux to mp4" starts *after* RECORDING_STOPPED and has no completion
//   event, so for MKV/FLV/TS sources we wait for the remuxed sibling before giving up and
//   using the original.
class RecordingWatcher : public QObject {
	Q_OBJECT

public:
	explicit RecordingWatcher(QObject *parent = nullptr);
	~RecordingWatcher() override;

signals:
	// durationMs is the wall-clock length of the file (0 if unknown); resolutionLabel is
	// OBS's output size and frame rate when recording started, e.g. "1080p60".
	void recordingReady(const QString &path, qint64 durationMs, const QString &resolutionLabel);

private:
	struct PendingFile {
		QString path;
		qint64 durationMs = 0;
		QString remuxCandidate; // empty unless we wait for an auto-remux result
		qint64 lastSize = -1;
		int stableTicks = 0;
		qint64 deadlineMs = 0;
	};

	static void onFrontendEvent(enum obs_frontend_event event, void *data);
	static void onFileChanged(void *data, calldata_t *calldata);

	void handleRecordingStarted();
	void handleRecordingStopped();
	void handleFileChanged(const QString &nextFile);

	void connectOutput();
	void disconnectOutput();
	void enqueue(const QString &path, bool recordingEnded, qint64 durationMs);
	void poll();

	obs_output_t *m_output = nullptr;
	QString m_currentPath;
	qint64 m_segmentStartedMs = 0; // when the current file started
	QString m_resolutionLabel;
	QHash<QString, PendingFile> m_pending;
	QTimer *m_pollTimer;
};

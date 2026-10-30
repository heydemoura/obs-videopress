// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <api-error.hpp>
#include <tus-upload.hpp>

#include <QDateTime>
#include <QElapsedTimer>
#include <QList>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QUrl>

namespace videopress {
class JetpackSiteClient;
class SettingsStore;
} // namespace videopress

// Where a job came from, and what OBS knew about the recording.
struct UploadSource {
	bool fromRecording = false;
	qint64 durationMs = 0;   // 0 when unknown
	QString resolutionLabel; // e.g. "1080p60"; empty when unknown
};

// Prototype upload queue: one upload at a time, in memory (a persistent queue comes
// later). A failed upload keeps its TUS resume state, so "Retry" continues where it
// stopped instead of starting over. After an upload it polls VideoPress until the video
// has finished processing.
class UploadController : public QObject {
	Q_OBJECT

public:
	enum class State { Pending, Queued, Uploading, Done, Failed, Cancelled };
	// After Done: VideoPress transcodes the video before it can play everywhere.
	enum class Processing { Unknown, Processing, Ready };

	using Source = UploadSource;

	struct Job {
		int id = 0;
		QString path;
		QString title;
		qint64 blogId = 0;
		State state = State::Pending;
		bool selected = false;
		Source source;
		qint64 bytesSent = 0;
		qint64 bytesTotal = 0;
		qint64 etaSeconds = -1; // -1 while the transfer rate is not known yet
		QString message;
		QString guid;
		QDateTime uploadedAt;
		Processing processing = Processing::Unknown;
		QUrl posterUrl;
		videopress::TusResumeState resume;
	};

	// The service's current upload size limit.
	static constexpr qint64 kMaxUploadBytes = 6LL * 1024 * 1024 * 1024;

	explicit UploadController(videopress::SettingsStore *settings, QObject *parent = nullptr);
	~UploadController() override;

	// start=false leaves the job Pending (and selected) until the user uploads it.
	int add(const QString &path, const QString &title, qint64 blogId, bool start, const Source &source = {});
	void start(int id, qint64 blogId);
	void startSelected(qint64 blogId);
	void cancel(int id);
	void cancelAll();
	void remove(int id);
	void setSelected(int id, bool selected);
	void setPaused(bool paused);
	bool isPaused() const { return m_paused; }

	const Job *job(int id) const;
	const QList<Job> &jobs() const { return m_jobs; }

	// Jobs the user can tick for upload: not yet sent, or stopped before finishing.
	static bool isSelectable(const Job &job);

#ifdef OBS_VIDEOPRESS_PREVIEW
	// Design previews only (never in plugin builds): show a job in any state.
	void injectForPreview(Job job)
	{
		job.id = m_nextId++;
		m_jobs.append(job);
		emit jobAdded(job.id);
	}
#endif

signals:
	void jobAdded(int id);
	void jobChanged(int id);
	void jobRemoved(int id);
	void pausedChanged(bool paused);

private:
	Job *find(int id);
	void pump();
	void begin(Job &job);
	void finishActive();
	void pollProcessing();

	videopress::SettingsStore *m_settings;
	QNetworkAccessManager m_network;
	QList<Job> m_jobs;
	int m_nextId = 1;
	int m_activeId = 0;
	bool m_paused = false;
	QPointer<videopress::TusUpload> m_upload;
	QPointer<videopress::JetpackSiteClient> m_client;

	// Transfer rate of the active upload, for the time-left estimate.
	QElapsedTimer m_rateClock;
	qint64 m_rateStartBytes = 0;

	QTimer m_processingPoll;
};

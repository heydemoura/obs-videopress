// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "recording-watcher.hpp"

#include <plugin-support.h>
#include <util/config-file.h>

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTimer>

namespace {

constexpr int kPollIntervalMs = 1000;
constexpr int kStableTicksRequired = 2;
constexpr qint64 kStabilityTimeoutMs = 10 * 60 * 1000;
constexpr qint64 kRemuxTimeoutMs = 15 * 60 * 1000;

QString takeObsString(char *value)
{
	QString result = value ? QString::fromUtf8(value) : QString();
	bfree(value);
	return result;
}

bool autoRemuxEnabled()
{
	config_t *config = obs_frontend_get_profile_config();
	return config && config_get_bool(config, "Video", "AutoRemux");
}

// The file OBS's auto-remux writes next to `path` (OBSBasic::AutoRemux), or empty when
// the container is uploaded as recorded.
QString remuxCandidateFor(const QString &path)
{
	const QFileInfo info(path);
	const QString suffix = info.suffix().toLower();
	if (suffix != QLatin1String("mkv") && suffix != QLatin1String("flv") && suffix != QLatin1String("ts"))
		return QString();
	if (!autoRemuxEnabled())
		return QString();
	return info.path() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".mp4");
}

} // namespace

RecordingWatcher::RecordingWatcher(QObject *parent) : QObject(parent), m_pollTimer(new QTimer(this))
{
	m_pollTimer->setInterval(kPollIntervalMs);
	connect(m_pollTimer, &QTimer::timeout, this, &RecordingWatcher::poll);
	obs_frontend_add_event_callback(&RecordingWatcher::onFrontendEvent, this);
}

RecordingWatcher::~RecordingWatcher()
{
	obs_frontend_remove_event_callback(&RecordingWatcher::onFrontendEvent, this);
	disconnectOutput();
}

void RecordingWatcher::onFrontendEvent(enum obs_frontend_event event, void *data)
{
	auto *self = static_cast<RecordingWatcher *>(data);
	switch (event) {
	case OBS_FRONTEND_EVENT_RECORDING_STARTED:
		self->handleRecordingStarted();
		break;
	case OBS_FRONTEND_EVENT_RECORDING_STOPPED:
		self->handleRecordingStopped();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		self->disconnectOutput();
		break;
	default:
		break;
	}
}

void RecordingWatcher::onFileChanged(void *data, calldata_t *calldata)
{
	// Runs on the muxer thread: hop to the UI thread before touching any state.
	auto *self = static_cast<RecordingWatcher *>(data);
	const QString nextFile = QString::fromUtf8(calldata_string(calldata, "next_file"));
	QMetaObject::invokeMethod(self, [self, nextFile] { self->handleFileChanged(nextFile); }, Qt::QueuedConnection);
}

void RecordingWatcher::handleRecordingStarted()
{
	m_currentPath = takeObsString(obs_frontend_get_last_recording());
	m_segmentStartedMs = QDateTime::currentMSecsSinceEpoch();
	m_resolutionLabel.clear();
	obs_video_info video;
	if (obs_get_video_info(&video) && video.fps_den > 0)
		m_resolutionLabel = QStringLiteral("%1p%2")
					    .arg(video.output_height)
					    .arg(qRound(double(video.fps_num) / double(video.fps_den)));
	connectOutput();
}

void RecordingWatcher::handleFileChanged(const QString &nextFile)
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (!m_currentPath.isEmpty() && m_currentPath != nextFile)
		enqueue(m_currentPath, false, m_segmentStartedMs ? now - m_segmentStartedMs : 0);
	m_currentPath = nextFile;
	m_segmentStartedMs = now;
}

void RecordingWatcher::handleRecordingStopped()
{
	disconnectOutput();
	const QString path = takeObsString(obs_frontend_get_last_recording());
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	if (!path.isEmpty())
		enqueue(path, true, m_segmentStartedMs ? now - m_segmentStartedMs : 0);
	m_currentPath.clear();
	m_segmentStartedMs = 0;
}

void RecordingWatcher::connectOutput()
{
	disconnectOutput();
	m_output = obs_frontend_get_recording_output();
	if (!m_output)
		return;
	signal_handler_connect(obs_output_get_signal_handler(m_output), "file_changed",
			       &RecordingWatcher::onFileChanged, this);
}

void RecordingWatcher::disconnectOutput()
{
	if (!m_output)
		return;
	signal_handler_disconnect(obs_output_get_signal_handler(m_output), "file_changed",
				  &RecordingWatcher::onFileChanged, this);
	obs_output_release(m_output);
	m_output = nullptr;
}

void RecordingWatcher::enqueue(const QString &path, bool recordingEnded, qint64 durationMs)
{
	PendingFile pending;
	pending.path = path;
	pending.durationMs = durationMs;
	// Split parts are never remuxed individually while recording continues.
	pending.remuxCandidate = recordingEnded ? remuxCandidateFor(path) : QString();
	pending.deadlineMs = QDateTime::currentMSecsSinceEpoch() +
			     (pending.remuxCandidate.isEmpty() ? kStabilityTimeoutMs : kRemuxTimeoutMs);
	m_pending.insert(path, pending);
	obs_log(LOG_INFO, "recording finished: %s%s", QFileInfo(path).fileName().toUtf8().constData(),
		pending.remuxCandidate.isEmpty() ? "" : " (waiting for auto-remux)");
	if (!m_pollTimer->isActive())
		m_pollTimer->start();
}

void RecordingWatcher::poll()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	QStringList ready;
	QStringList dropped;

	for (auto it = m_pending.begin(); it != m_pending.end(); ++it) {
		PendingFile &pending = it.value();
		const bool timedOut = now >= pending.deadlineMs;

		// Prefer the remuxed file once it exists; fall back to the original on timeout.
		if (!pending.remuxCandidate.isEmpty()) {
			if (QFileInfo::exists(pending.remuxCandidate)) {
				pending.path = pending.remuxCandidate;
				pending.remuxCandidate.clear();
				pending.lastSize = -1;
				pending.stableTicks = 0;
				pending.deadlineMs = now + kStabilityTimeoutMs;
			} else if (timedOut) {
				obs_log(LOG_WARNING, "auto-remux result not found, uploading the original recording");
				pending.remuxCandidate.clear();
				pending.deadlineMs = now + kStabilityTimeoutMs;
			}
			continue;
		}

		const QFileInfo info(pending.path);
		const qint64 size = info.exists() ? info.size() : -1;
		if (size > 0 && size == pending.lastSize) {
			++pending.stableTicks;
		} else {
			pending.stableTicks = 0;
			pending.lastSize = size;
		}

		QFile probe(pending.path);
		if (pending.stableTicks >= kStableTicksRequired && probe.open(QIODevice::ReadOnly)) {
			ready.append(it.key());
		} else if (timedOut) {
			obs_log(LOG_WARNING, "recording never became readable: %s",
				info.fileName().toUtf8().constData());
			dropped.append(it.key());
		}
	}

	for (const QString &key : dropped)
		m_pending.remove(key);
	for (const QString &key : ready) {
		const PendingFile done = m_pending.take(key);
		emit recordingReady(done.path, done.durationMs, m_resolutionLabel);
	}

	if (m_pending.isEmpty())
		m_pollTimer->stop();
}

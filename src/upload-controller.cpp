// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "upload-controller.hpp"

#include "credential-store.hpp"

#include <plugin-support.h>

#include <jetpack-site-client.hpp>
#include <settings-store.hpp>

#include <obs-module.h>

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>

using videopress::ApiError;
using videopress::JetpackSiteClient;
using videopress::TusUpload;

namespace {

constexpr int kProcessingPollMs = 20 * 1000;
constexpr qint64 kProcessingPollGiveUpSecs = 3 * 60 * 60;
constexpr qint64 kRateWarmupMs = 3000;

QString describe(const ApiError &error)
{
	if (error.kind == videopress::ErrorKind::Forbidden) {
		switch (error.forbiddenReason) {
		case videopress::ForbiddenReason::VideoPressDisabled:
			return QStringLiteral(
				"VideoPress is turned off on this site (Jetpack → Settings → Performance).");
		case videopress::ForbiddenReason::QuotaExceeded:
			return QStringLiteral("Not enough VideoPress storage left on this site.");
		case videopress::ForbiddenReason::SiteBlocked:
			return QStringLiteral("VideoPress uploads are blocked for this site.");
		case videopress::ForbiddenReason::TrialExpired:
			return QStringLiteral("The VideoPress trial for this site has expired.");
		default:
			break;
		}
	}
	if (error.kind == videopress::ErrorKind::Unauthorized)
		return QStringLiteral("The application password was rejected. Remove the site and add it again.");
	return error.message.isEmpty() ? QStringLiteral("Upload failed (HTTP %1)").arg(error.httpStatus)
				       : error.message;
}

} // namespace

UploadController::UploadController(videopress::SettingsStore *settings, QObject *parent)
	: QObject(parent),
	  m_settings(settings)
{
	m_processingPoll.setInterval(kProcessingPollMs);
	connect(&m_processingPoll, &QTimer::timeout, this, &UploadController::pollProcessing);
}

UploadController::~UploadController()
{
	delete m_upload;
	delete m_client;
}

bool UploadController::isSelectable(const Job &job)
{
	return job.state == State::Pending || job.state == State::Cancelled || job.state == State::Failed;
}

int UploadController::add(const QString &path, const QString &title, qint64 blogId, bool start, const Source &source)
{
	Job job;
	job.id = m_nextId++;
	job.path = path;
	job.title = title;
	job.blogId = blogId;
	job.source = source;
	job.selected = !start; // a new recording is ticked, ready for "Upload"
	job.bytesTotal = QFileInfo(path).size();
	m_jobs.append(job);
	emit jobAdded(job.id);
	if (start)
		this->start(job.id, blogId);
	return job.id;
}

void UploadController::start(int id, qint64 blogId)
{
	Job *job = find(id);
	if (!job || job->state == State::Uploading || job->state == State::Queued || job->state == State::Done)
		return;
	if (job->blogId != blogId)
		job->resume = {}; // a TUS upload belongs to one blog
	job->blogId = blogId;
	job->selected = false;
	job->message.clear();
	job->etaSeconds = -1;

	if (job->bytesTotal > kMaxUploadBytes) {
		job->state = State::Failed;
		job->message = QStringLiteral("Larger than the 6 GB upload limit. Turn on automatic file splitting in "
					      "Settings → Output → Recording, or lower the bitrate.");
	} else if (!m_settings->site(blogId)) {
		job->state = State::Failed;
		job->message = QStringLiteral("Choose a site first.");
	} else {
		job->state = State::Queued;
	}
	emit jobChanged(id);
	pump();
}

void UploadController::startSelected(qint64 blogId)
{
	QList<int> ids;
	for (const Job &job : m_jobs) {
		if (job.selected && isSelectable(job))
			ids.append(job.id);
	}
	for (int id : ids)
		start(id, blogId);
}

void UploadController::cancel(int id)
{
	Job *job = find(id);
	if (!job)
		return;
	if (id == m_activeId && m_upload) {
		m_upload->cancel(); // emits failed(Cancelled); handled in begin()
		return;
	}
	if (job->state == State::Queued) {
		job->state = State::Cancelled;
		emit jobChanged(id);
	}
}

void UploadController::cancelAll()
{
	// Queued ones first, so cancelling the active upload does not start the next.
	QList<int> queued;
	for (const Job &job : m_jobs) {
		if (job.state == State::Queued)
			queued.append(job.id);
	}
	for (int id : queued)
		cancel(id);
	if (m_activeId)
		cancel(m_activeId);
}

void UploadController::remove(int id)
{
	if (id == m_activeId)
		cancel(id);
	m_jobs.removeIf([id](const Job &job) { return job.id == id; });
	emit jobRemoved(id);
}

void UploadController::setSelected(int id, bool selected)
{
	Job *job = find(id);
	if (!job || !isSelectable(*job) || job->selected == selected)
		return;
	job->selected = selected;
	emit jobChanged(id);
}

void UploadController::setPaused(bool paused)
{
	if (m_paused == paused)
		return;
	m_paused = paused;
	emit pausedChanged(paused);
	if (!m_paused)
		pump();
}

const UploadController::Job *UploadController::job(int id) const
{
	for (const Job &job : m_jobs) {
		if (job.id == id)
			return &job;
	}
	return nullptr;
}

UploadController::Job *UploadController::find(int id)
{
	for (Job &job : m_jobs) {
		if (job.id == id)
			return &job;
	}
	return nullptr;
}

void UploadController::pump()
{
	// Uploads already running are not interrupted by a pause; new ones wait.
	if (m_activeId || m_paused)
		return;
	for (Job &job : m_jobs) {
		if (job.state == State::Queued) {
			begin(job);
			return;
		}
	}
}

void UploadController::begin(Job &job)
{
	const videopress::SiteEntry *site = m_settings->site(job.blogId);
	const QString password =
		site ? credential_store::load(credential_store::accountFor(site->siteUrl, site->username)) : QString();
	if (!site || password.isEmpty()) {
		job.state = State::Failed;
		job.message = QStringLiteral("No saved password for this site. Remove the site and add it again.");
		emit jobChanged(job.id);
		pump();
		return;
	}

	m_activeId = job.id;
	job.state = State::Uploading;
	m_rateClock.start();
	m_rateStartBytes = job.resume.offset;
	emit jobChanged(job.id);

	const QByteArray userAgent = QByteArrayLiteral("obs-videopress/") + PLUGIN_VERSION;
	m_client = new JetpackSiteClient(&m_network, {site->siteUrl, site->username, password, site->apiRoot}, this);
	m_client->setUserAgent(userAgent);

	videopress::TusUploadOptions options;
	options.filePath = job.path;
	options.blogId = job.blogId;
	options.userAgent = userAgent;
	options.extraMetadata = {{"title", job.title}};
	m_upload = new TusUpload(&m_network, options, m_client->tokenProvider(), job.resume, this);

	const int id = job.id;
	connect(m_upload, &TusUpload::progress, this, [this, id](qint64 sent, qint64 total) {
		Job *j = find(id);
		if (!j)
			return;
		j->bytesSent = sent;
		j->bytesTotal = total;
		// Average rate since this upload started; steadier than chunk-to-chunk.
		const qint64 elapsed = m_rateClock.elapsed();
		const qint64 transferred = sent - m_rateStartBytes;
		if (elapsed >= kRateWarmupMs && transferred > 0)
			j->etaSeconds = (total - sent) * elapsed / transferred / 1000;
		emit jobChanged(id);
	});
	connect(m_upload, &TusUpload::resumeStateChanged, this, [this, id](const videopress::TusResumeState &state) {
		if (Job *j = find(id))
			j->resume = state;
	});
	connect(m_upload, &TusUpload::finished, this, [this, id](const videopress::TusUploadResult &result) {
		if (Job *j = find(id)) {
			j->state = State::Done;
			j->guid = result.guid;
			j->bytesSent = j->bytesTotal;
			j->uploadedAt = QDateTime::currentDateTime();
			j->processing = Processing::Processing;
			obs_log(LOG_INFO, "uploaded %s as %s", QFileInfo(j->path).fileName().toUtf8().constData(),
				result.guid.toUtf8().constData());
			emit jobChanged(id);
			if (!m_processingPoll.isActive())
				m_processingPoll.start();
		}
		finishActive();
	});
	connect(m_upload, &TusUpload::failed, this, [this, id](const ApiError &error) {
		if (Job *j = find(id)) {
			j->state = error.kind == videopress::ErrorKind::Cancelled ? State::Cancelled : State::Failed;
			j->message = j->state == State::Cancelled ? QString() : describe(error);
			if (j->state == State::Failed)
				obs_log(LOG_WARNING, "upload of %s failed at %lld of %lld bytes: HTTP %d %s: %s",
					QFileInfo(j->path).fileName().toUtf8().constData(),
					static_cast<long long>(j->resume.offset), static_cast<long long>(j->bytesTotal),
					error.httpStatus, error.code.toUtf8().constData(),
					j->message.toUtf8().constData());
			emit jobChanged(id);
		}
		finishActive();
	});

	m_upload->start();
}

void UploadController::finishActive()
{
	m_activeId = 0;
	// Called from TusUpload's own signals: delete the upload and client later.
	if (m_upload)
		m_upload->deleteLater();
	if (m_client)
		m_client->deleteLater();
	m_upload.clear();
	m_client.clear();
	QMetaObject::invokeMethod(this, &UploadController::pump, Qt::QueuedConnection);
}

void UploadController::pollProcessing()
{
	// GET /rest/v1.1/videos/{guid} is public for public videos: "finished" flips once
	// VideoPress has transcoded the upload, and it carries the poster and duration.
	bool waiting = false;
	for (Job &job : m_jobs) {
		if (job.state != State::Done || job.processing != Processing::Processing)
			continue;
		if (job.uploadedAt.secsTo(QDateTime::currentDateTime()) > kProcessingPollGiveUpSecs) {
			job.processing = Processing::Unknown;
			emit jobChanged(job.id);
			continue;
		}
		waiting = true;

		QNetworkRequest request(
			QUrl(QStringLiteral("https://public-api.wordpress.com/rest/v1.1/videos/%1").arg(job.guid)));
		request.setHeader(QNetworkRequest::UserAgentHeader,
				  QByteArrayLiteral("obs-videopress/") + PLUGIN_VERSION);
		request.setTransferTimeout(15000);
		QNetworkReply *reply = m_network.get(request);
		const int id = job.id;
		connect(reply, &QNetworkReply::finished, this, [this, reply, id] {
			reply->deleteLater();
			Job *j = find(id);
			if (!j || j->processing != Processing::Processing)
				return;
			const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
			if (status == 403 || status == 404) {
				// Private video or site: the public endpoint can't tell us. Stop asking.
				j->processing = Processing::Unknown;
				emit jobChanged(id);
				return;
			}
			const QJsonObject video = QJsonDocument::fromJson(reply->readAll()).object();
			if (video.isEmpty())
				return; // transient; try again on the next tick
			const QString poster = video.value(QLatin1String("poster")).toString();
			if (!poster.isEmpty() && !video.value(QLatin1String("thumbnail_generating")).toBool())
				j->posterUrl = QUrl(poster);
			if (j->source.durationMs <= 0)
				j->source.durationMs = video.value(QLatin1String("duration")).toInteger();
			if (video.value(QLatin1String("finished")).toBool())
				j->processing = Processing::Ready;
			emit jobChanged(id);
		});
	}
	if (!waiting)
		m_processingPoll.stop();
}

// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "tus-upload.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace videopress {

namespace {

constexpr int kTransferTimeoutMs = 2 * 60 * 1000;
constexpr int kMaxTokenRefreshes = 2;
constexpr int kMaxOffsetResyncs = 2;
constexpr int kTusUnprocessable = 422;
constexpr int kTusChecksumMismatch = 460; // tus checksum extension

const QByteArray kTokenHeader = QByteArrayLiteral("x-videopress-upload-token");
const QByteArray kKeyTokenHeader = QByteArrayLiteral("x-videopress-upload-key-token");
const QByteArray kGuidHeader = QByteArrayLiteral("x-videopress-upload-guid");
const QByteArray kMediaIdHeader = QByteArrayLiteral("x-videopress-upload-media-id");
const QByteArray kSourceUrlHeader = QByteArrayLiteral("x-videopress-upload-src-url");

int httpStatus(QNetworkReply *reply)
{
	return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

bool readOffsetHeader(QNetworkReply *reply, qint64 *offset)
{
	bool ok = false;
	*offset = reply->rawHeader("Upload-Offset").trimmed().toLongLong(&ok);
	return ok && *offset >= 0;
}

ApiError errorFor(QNetworkReply *reply, const QByteArray &body)
{
	ApiError error = ApiError::fromReply(reply, body);
	// Failures while finalizing the upload come back as 400 + this header, no body.
	// The uploader explains failures in this header (often with an empty body); it is the
	// most specific message we get, so it wins over anything derived from the status.
	const QByteArray uploadError = reply->rawHeader("x-videopress-upload-error");
	if (!uploadError.isEmpty())
		error.message = QString::fromUtf8(uploadError);
	// The upload service answers 422 without a body when it can't store a chunk.
	if (httpStatus(reply) == kTusUnprocessable &&
	    (error.message.isEmpty() || error.message.contains(QLatin1String("HTTP 422"))))
		error.message =
			QStringLiteral("VideoPress couldn't store part of the upload (HTTP 422). Retry to resume it.");
	if (!error.isError())
		error = ApiError::protocol(QStringLiteral("Unexpected HTTP status %1").arg(httpStatus(reply)));
	return error;
}

} // namespace

QJsonObject TusResumeState::toJson() const
{
	QJsonObject obj;
	obj.insert(QLatin1String("upload_url"), uploadUrl.toString(QUrl::FullyEncoded));
	obj.insert(QLatin1String("key_token"), keyToken);
	obj.insert(QLatin1String("key_token_issued_at_ms"), keyTokenIssuedAtMs);
	obj.insert(QLatin1String("offset"), offset);
	return obj;
}

TusResumeState TusResumeState::fromJson(const QJsonObject &obj)
{
	TusResumeState state;
	state.uploadUrl = QUrl(obj.value(QLatin1String("upload_url")).toString(), QUrl::StrictMode);
	state.keyToken = obj.value(QLatin1String("key_token")).toString();
	state.keyTokenIssuedAtMs = obj.value(QLatin1String("key_token_issued_at_ms")).toInteger();
	state.offset = std::max<qint64>(0, obj.value(QLatin1String("offset")).toInteger());
	return state;
}

TusUpload::TusUpload(QNetworkAccessManager *network, TusUploadOptions options, UploadTokenProvider tokenProvider,
		     TusResumeState resume, QObject *parent)
	: QObject(parent),
	  m_network(network),
	  m_options(std::move(options)),
	  m_tokenProvider(std::move(tokenProvider)),
	  m_resume(std::move(resume)),
	  m_clock([] { return QDateTime::currentMSecsSinceEpoch(); })
{
}

TusUpload::~TusUpload()
{
	m_alive.reset();
	if (m_reply) {
		// Disconnect first: abort() emits finished(), and this object is half destroyed.
		m_reply->disconnect(this);
		m_reply->abort();
		m_reply->deleteLater();
	}
}

void TusUpload::start()
{
	if (m_phase != Phase::Idle)
		return;

	m_file.setFileName(m_options.filePath);
	if (!m_file.open(QIODevice::ReadOnly)) {
		fail(ApiError::local(
			QStringLiteral("Cannot open %1: %2").arg(m_options.filePath, m_file.errorString())));
		return;
	}
	m_totalBytes = m_file.size();
	if (m_totalBytes <= 0) {
		fail(ApiError::local(QStringLiteral("%1 is empty").arg(m_options.filePath)));
		return;
	}
	if (m_options.fileName.isEmpty())
		m_options.fileName = QFileInfo(m_options.filePath).fileName();
	if (m_options.mimeType.isEmpty())
		m_options.mimeType = QMimeDatabase().mimeTypeForFile(m_options.filePath).name();
	if (m_options.chunkSize <= 0) {
		fail(ApiError::local(QStringLiteral("Invalid chunk size")));
		return;
	}

	if (m_resume.isEmpty())
		mintToken([this](const QString &jwt) { createUpload(jwt); });
	else
		mintToken([this](const QString &jwt) { syncWithServer(jwt); });
}

void TusUpload::cancel()
{
	if (m_phase == Phase::Finished || m_phase == Phase::Failed || m_phase == Phase::Cancelled)
		return;
	m_phase = Phase::Cancelled;
	if (m_reply)
		m_reply->abort();
	m_file.close();
	emit failed(ApiError::cancelled());
}

void TusUpload::mintToken(std::function<void(const QString &jwt)> then)
{
	m_phase = Phase::MintingToken;
	std::weak_ptr<bool> alive = m_alive;
	m_tokenProvider(m_options.blogId,
			[this, alive, next = std::move(then)](const QString &token, const ApiError &error) {
				if (alive.expired() || m_phase != Phase::MintingToken)
					return;
				if (error.isError()) {
					fail(error);
					return;
				}
				if (token.isEmpty()) {
					fail(ApiError::protocol(QStringLiteral("Empty VideoPress upload token")));
					return;
				}
				next(token);
			});
}

QNetworkRequest TusUpload::baseRequest(const QUrl &url, const QString &token) const
{
	QNetworkRequest request(url);
	request.setRawHeader("Tus-Resumable", "1.0.0");
	request.setRawHeader(kTokenHeader, token.toUtf8());
	// Never Qt's default "Mozilla/5.0", which WordPress.com's edge may rate-limit.
	request.setHeader(QNetworkRequest::UserAgentHeader,
			  m_options.userAgent.isEmpty() ? QByteArrayLiteral("obs-videopress") : m_options.userAgent);
	// The tokens travel in headers; never follow a redirect to another host with them.
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(kTransferTimeoutMs);
	return request;
}

void TusUpload::track(QNetworkReply *reply)
{
	m_reply = reply;
}

void TusUpload::createUpload(const QString &jwt)
{
	m_phase = Phase::Creating;

	const QUrl url =
		m_options.apiBase.resolved(QUrl(QStringLiteral("/rest/v1.1/video-uploads/%1").arg(m_options.blogId)));
	QNetworkRequest request = baseRequest(url, jwt);
	request.setRawHeader("Upload-Length", QByteArray::number(m_totalBytes));
	QByteArray metadata = "filename " + uploadFileName().toUtf8().toBase64() + ",filetype " +
			      m_options.mimeType.toUtf8().toBase64();
	for (const auto &[key, value] : m_options.extraMetadata) {
		if (!value.isEmpty())
			metadata += ',' + key + ' ' + value.toUtf8().toBase64();
	}
	request.setRawHeader("Upload-Metadata", metadata);

	QNetworkReply *reply = m_network->post(request, QByteArray());
	track(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (m_phase == Phase::Creating)
			onCreateFinished(reply);
	});
}

void TusUpload::onCreateFinished(QNetworkReply *reply)
{
	const QByteArray body = reply->readAll();
	const int status = httpStatus(reply);
	if (reply->error() != QNetworkReply::NoError || (status != 201 && status != 200)) {
		fail(errorFor(reply, body));
		return;
	}

	const QByteArray location = reply->rawHeader("Location");
	const QByteArray keyToken = reply->rawHeader(kKeyTokenHeader);
	if (location.isEmpty() || keyToken.isEmpty()) {
		fail(ApiError::protocol(QStringLiteral("Upload was created without a location or key token")));
		return;
	}

	m_resume = TusResumeState();
	m_resume.uploadUrl = reply->url().resolved(QUrl::fromEncoded(location));
	setKeyToken(QString::fromUtf8(keyToken));
	emit resumeStateChanged(m_resume);
	sendNextChunk();
}

QString TusUpload::uploadFileName() const
{
	if (!m_options.uniqueFileName)
		return m_options.fileName;
	// The upload service stores the partial upload under a path derived from this name. Re-uploading a
	// name that was uploaded or attempted before can append to leftover bytes there, and
	// the server then rejects the finished upload ("The uploaded file size does not match
	// the expected size"). A per-attempt suffix keeps every upload on a fresh path; the
	// video's title travels separately in the "title" metadata.
	const QFileInfo info(m_options.fileName);
	const QString suffix =
		QStringLiteral("%1").arg(QRandomGenerator::global()->generate(), 8, 16, QLatin1Char('0'));
	return info.suffix().isEmpty() ? QStringLiteral("%1-%2").arg(info.completeBaseName(), suffix)
				       : QStringLiteral("%1-%2.%3").arg(info.completeBaseName(), suffix, info.suffix());
}

void TusUpload::syncWithServer(const QString &jwt)
{
	m_phase = Phase::Syncing;

	// public-api.wordpress.com only routes GET and POST (a real HEAD gets 405): send HEAD as
	// GET + X-HTTP-Method-Override, like Jetpack's uploader does.
	QNetworkRequest request = baseRequest(m_resume.uploadUrl, jwt);
	request.setRawHeader("X-HTTP-Method-Override", "HEAD");
	QNetworkReply *reply = m_network->get(request);
	track(reply);
	connect(reply, &QNetworkReply::finished, this, [this, reply] {
		reply->deleteLater();
		if (m_phase == Phase::Syncing)
			onSyncFinished(reply);
	});
}

void TusUpload::onSyncFinished(QNetworkReply *reply)
{
	const int status = httpStatus(reply);
	if (status == 404 || status == 410) {
		restartFromScratch();
		return;
	}
	if (reply->error() != QNetworkReply::NoError || (status != 200 && status != 204)) {
		fail(errorFor(reply, reply->readAll()));
		return;
	}

	qint64 offset = 0;
	if (!readOffsetHeader(reply, &offset) || offset > m_totalBytes) {
		fail(ApiError::protocol(QStringLiteral("Server returned an invalid Upload-Offset")));
		return;
	}
	m_resume.offset = offset;

	const QByteArray keyToken = reply->rawHeader(kKeyTokenHeader);
	if (!keyToken.isEmpty())
		setKeyToken(QString::fromUtf8(keyToken));
	emit resumeStateChanged(m_resume);
	emit progress(m_resume.offset, m_totalBytes);

	if (m_resume.offset >= m_totalBytes) {
		// Every byte is on the server. HEAD carries the guid once the video exists.
		if (readResultHeaders(reply))
			finish();
		else
			fail(ApiError::protocol(
				QStringLiteral("Upload is complete but the server returned no video id")));
		return;
	}
	if (keyToken.isEmpty()) {
		fail(ApiError::protocol(QStringLiteral("Server did not return an upload key token")));
		return;
	}
	sendNextChunk();
}

void TusUpload::sendNextChunk()
{
	if (keyTokenNeedsRefresh()) {
		mintToken([this](const QString &jwt) { syncWithServer(jwt); });
		return;
	}

	m_phase = Phase::Uploading;
	const qint64 chunkStart = m_resume.offset;
	const qint64 chunkLength = std::min(m_options.chunkSize, m_totalBytes - chunkStart);

	QByteArray data;
	if (!m_file.seek(chunkStart) || (data = m_file.read(chunkLength)).size() != chunkLength) {
		fail(ApiError::local(
			QStringLiteral("Cannot read %1: %2").arg(m_options.filePath, m_file.errorString())));
		return;
	}

	QNetworkRequest request = baseRequest(m_resume.uploadUrl, m_resume.keyToken);
	// Sent as POST like Jetpack's uploader; the server dispatches on the override header.
	request.setRawHeader("X-HTTP-Method-Override", "PATCH");
	request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/offset+octet-stream"));
	request.setRawHeader("Upload-Offset", QByteArray::number(chunkStart));
	request.setRawHeader("Upload-Checksum",
			     "sha1 " + QCryptographicHash::hash(data, QCryptographicHash::Sha1).toBase64());

	QNetworkReply *reply = m_network->post(request, data);
	track(reply);
	connect(reply, &QNetworkReply::uploadProgress, this, [this, chunkStart](qint64 sent, qint64) {
		if (m_phase == Phase::Uploading)
			emit progress(chunkStart + sent, m_totalBytes);
	});
	connect(reply, &QNetworkReply::finished, this, [this, reply, chunkStart, chunkLength] {
		reply->deleteLater();
		if (m_phase == Phase::Uploading)
			onChunkFinished(reply, chunkStart, chunkLength);
	});
}

void TusUpload::onChunkFinished(QNetworkReply *reply, qint64 chunkStart, qint64 chunkLength)
{
	const QByteArray body = reply->readAll();
	const int status = httpStatus(reply);

	if (reply->error() == QNetworkReply::NoError && (status == 204 || status == 200)) {
		qint64 offset = 0;
		if (!readOffsetHeader(reply, &offset) || offset <= chunkStart || offset > chunkStart + chunkLength) {
			fail(ApiError::protocol(QStringLiteral("Server returned an invalid Upload-Offset")));
			return;
		}
		m_resume.offset = offset;
		m_tokenRefreshes = 0;
		m_offsetResyncs = 0;
		m_chunkRetries = 0;
		emit resumeStateChanged(m_resume);
		emit progress(m_resume.offset, m_totalBytes);

		if (readResultHeaders(reply)) {
			finish();
		} else if (m_resume.offset >= m_totalBytes) {
			// The last chunk normally carries the guid; ask for it explicitly.
			mintToken([this](const QString &jwt) { syncWithServer(jwt); });
		} else {
			sendNextChunk();
		}
		return;
	}

	const ApiError error = errorFor(reply, body);

	if (error.kind == ErrorKind::Forbidden && error.forbiddenReason == ForbiddenReason::InvalidUploadToken &&
	    m_tokenRefreshes < kMaxTokenRefreshes) {
		++m_tokenRefreshes;
		mintToken([this](const QString &jwt) { syncWithServer(jwt); });
		return;
	}
	if ((error.kind == ErrorKind::Conflict || status == kTusChecksumMismatch) &&
	    m_offsetResyncs < kMaxOffsetResyncs) {
		++m_offsetResyncs;
		mintToken([this](const QString &jwt) { syncWithServer(jwt); });
		return;
	}
	if (error.kind == ErrorKind::NotFound || error.kind == ErrorKind::Gone) {
		restartFromScratch();
		return;
	}
	// The server keeps a failed integrity check terminal for this upload key and asks for a
	// fresh upload; resending chunks would only repeat the error.
	if (error.message.contains(QLatin1String("does not match the expected size")) && !m_restarted) {
		restartFromScratch();
		return;
	}
	// Transient server-side failures: the service answers 422 ("Upload failed.") when it can't open
	// the partial file for this chunk, plus the usual 5xx and dropped connections. Wait,
	// re-read the server's offset and send the chunk again; if the partial file is really
	// gone, the resync answers 404/410 and the upload restarts.
	const bool transient = status == kTusUnprocessable || error.kind == ErrorKind::Server ||
			       error.kind == ErrorKind::Network;
	if (transient && m_chunkRetries < m_options.chunkRetryDelaysMs.size()) {
		const int delayMs = m_options.chunkRetryDelaysMs.at(m_chunkRetries);
		++m_chunkRetries;
		m_phase = Phase::MintingToken; // waiting; cancel() still works
		std::weak_ptr<bool> alive = m_alive;
		QTimer::singleShot(delayMs, this, [this, alive] {
			if (alive.expired() || m_phase != Phase::MintingToken)
				return;
			mintToken([this](const QString &jwt) { syncWithServer(jwt); });
		});
		return;
	}
	fail(error);
}

void TusUpload::restartFromScratch()
{
	if (m_restarted) {
		fail(ApiError::protocol(QStringLiteral("The upload disappeared from the server twice")));
		return;
	}
	m_restarted = true;
	m_resume = TusResumeState();
	emit resumeStateChanged(m_resume);
	emit progress(0, m_totalBytes);
	mintToken([this](const QString &jwt) { createUpload(jwt); });
}

bool TusUpload::readResultHeaders(QNetworkReply *reply)
{
	const QByteArray guid = reply->rawHeader(kGuidHeader);
	if (guid.isEmpty())
		return false;
	m_result.guid = QString::fromUtf8(guid);
	m_result.mediaId = reply->rawHeader(kMediaIdHeader).toLongLong();
	m_result.sourceUrl = QUrl(QString::fromUtf8(reply->rawHeader(kSourceUrlHeader)));
	return true;
}

void TusUpload::setKeyToken(const QString &token)
{
	m_resume.keyToken = token;
	m_resume.keyTokenIssuedAtMs = m_clock();
}

bool TusUpload::keyTokenNeedsRefresh() const
{
	return m_resume.keyToken.isEmpty() || m_clock() - m_resume.keyTokenIssuedAtMs >= m_options.tokenRefreshAfterMs;
}

void TusUpload::fail(const ApiError &error)
{
	if (m_phase == Phase::Finished || m_phase == Phase::Failed || m_phase == Phase::Cancelled)
		return;
	m_phase = Phase::Failed;
	m_file.close();
	emit failed(error);
}

void TusUpload::finish()
{
	m_phase = Phase::Finished;
	m_file.close();
	emit finished(m_result);
}

} // namespace videopress

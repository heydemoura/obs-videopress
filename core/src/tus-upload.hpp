// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "api-error.hpp"

#include <QFile>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QObject>
#include <QPointer>
#include <QUrl>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace videopress {

// What has to survive an OBS restart to resume an upload where it stopped.
struct TusResumeState {
	QUrl uploadUrl;
	QString keyToken;
	qint64 keyTokenIssuedAtMs = 0;
	qint64 offset = 0;

	bool isEmpty() const { return uploadUrl.isEmpty(); }
	QJsonObject toJson() const;
	static TusResumeState fromJson(const QJsonObject &obj);
};

struct TusUploadResult {
	QString guid;
	qint64 mediaId = 0;
	QUrl sourceUrl;
};

struct TusUploadOptions {
	QString filePath;
	qint64 blogId = 0;
	QUrl apiBase = QUrl(QStringLiteral("https://public-api.wordpress.com"));
	qint64 chunkSize = 10000000; // Jetpack's uploader uses exactly this
	// Upload JWTs and per-upload key tokens expire after 30 minutes on the server; refresh a
	// little before that.
	qint64 tokenRefreshAfterMs = 25 * 60 * 1000;
	// Waits before re-sending a chunk after a transient failure; one entry per attempt.
	QList<int> chunkRetryDelaysMs = {1000, 4000, 10000};
	QString fileName; // defaults to the file's name on disk
	// Send fileName with a random suffix so each attempt gets a fresh server-side path.
	bool uniqueFileName = true;
	QString mimeType; // defaults to the type detected from the file
	// Extra Upload-Metadata pairs. For Jetpack sites the server copies title, description
	// and caption onto the new attachment.
	QList<QPair<QByteArray, QString>> extraMetadata;
	QByteArray userAgent;
};

// Mints a blog-scoped VideoPress upload JWT, normally via
// POST wpcom/v2/sites/{blog_id}/media/videopress-upload-jwt with the user's token.
using UploadTokenCallback = std::function<void(const QString &token, const ApiError &error)>;
using UploadTokenProvider = std::function<void(qint64 blogId, UploadTokenCallback callback)>;

// One file, one upload, one attempt. Speaks the VideoPress flavour of TUS 1.0 that
// Jetpack's uploader uses (projects/packages/videopress/src/tus/class-tus-client.php):
//
//   1. JWT  = token provider
//   2. POST {api}/rest/v1.1/video-uploads/{blog_id}         x-videopress-upload-token: JWT
//        -> 201 Location: .../{blog_id}/{key}, x-videopress-upload-key-token
//   3. POST {location} + X-HTTP-Method-Override: PATCH      x-videopress-upload-token: key token
//        -> 204 Upload-Offset; the final chunk also returns x-videopress-upload-guid/-media-id/-src-url
//   4. HEAD {location}                                      x-videopress-upload-token: JWT
//        -> Upload-Offset + a fresh key token (used to resume and to refresh expiring tokens)
//
// Retrying after a failure is the caller's job (the upload queue): it gets failed() with
// an ApiError and the latest resumeState() to start a new TusUpload from.
class TusUpload : public QObject {
	Q_OBJECT

public:
	enum class Phase { Idle, MintingToken, Creating, Syncing, Uploading, Finished, Failed, Cancelled };
	Q_ENUM(Phase)

	using Clock = std::function<qint64()>;

	TusUpload(QNetworkAccessManager *network, TusUploadOptions options, UploadTokenProvider tokenProvider,
		  TusResumeState resume = {}, QObject *parent = nullptr);
	~TusUpload() override;

	void setClock(Clock clock) { m_clock = std::move(clock); }

	void start();
	void cancel();

	Phase phase() const { return m_phase; }
	TusResumeState resumeState() const { return m_resume; }
	qint64 totalBytes() const { return m_totalBytes; }

signals:
	void progress(qint64 bytesUploaded, qint64 bytesTotal);
	void resumeStateChanged(const videopress::TusResumeState &state);
	void finished(const videopress::TusUploadResult &result);
	void failed(const videopress::ApiError &error);

private:
	void mintToken(std::function<void(const QString &jwt)> then);
	void createUpload(const QString &jwt);
	void syncWithServer(const QString &jwt);
	void sendNextChunk();
	void restartFromScratch();
	QString uploadFileName() const;

	void onCreateFinished(QNetworkReply *reply);
	void onSyncFinished(QNetworkReply *reply);
	void onChunkFinished(QNetworkReply *reply, qint64 chunkStart, qint64 chunkLength);

	bool readResultHeaders(QNetworkReply *reply);
	QNetworkRequest baseRequest(const QUrl &url, const QString &token) const;
	void setKeyToken(const QString &token);
	bool keyTokenNeedsRefresh() const;
	void track(QNetworkReply *reply);
	void fail(const ApiError &error);
	void finish();

	QNetworkAccessManager *m_network;
	TusUploadOptions m_options;
	UploadTokenProvider m_tokenProvider;
	TusResumeState m_resume;
	Clock m_clock;

	QFile m_file;
	qint64 m_totalBytes = 0;
	Phase m_phase = Phase::Idle;
	QPointer<QNetworkReply> m_reply;
	TusUploadResult m_result;

	// Bounded recovery, reset after every chunk the server accepts.
	int m_tokenRefreshes = 0;
	int m_offsetResyncs = 0;
	int m_chunkRetries = 0;
	bool m_restarted = false;
	// Guards token-provider callbacks that arrive after cancel() or destruction.
	std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace videopress

Q_DECLARE_METATYPE(videopress::TusResumeState)
Q_DECLARE_METATYPE(videopress::TusUploadResult)

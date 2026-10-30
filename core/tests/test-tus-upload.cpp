// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mock-http-server.hpp"
#include "tus-upload.hpp"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <optional>

using namespace videopress;

namespace {

constexpr qint64 kBlogId = 42;
const QByteArray kGuid = "AbCdEf12";

QByteArray jsonError(const char *message)
{
	// Shape of WordPress's wp_send_json_error(), which the upload service uses for errors.
	return QJsonDocument(QJsonObject{{QStringLiteral("success"), false},
					 {QStringLiteral("data"),
					  QJsonObject{{QStringLiteral("error"), QStringLiteral("Access Forbidden")},
						      {QStringLiteral("message"), QString::fromLatin1(message)}}}})
		.toJson(QJsonDocument::Compact);
}

// In-memory model of the VideoPress TUS upload service, with the same token rules:
// create and HEAD take the blog JWT, PATCH only takes the key token issued for that upload.
class FakeVideoPress {
public:
	struct Upload {
		qint64 length = 0;
		QByteArray data;
		QByteArray metadata;
	};

	using Interceptor = std::function<std::optional<MockResponse>(const MockRequest &)>;

	explicit FakeVideoPress(MockHttpServer *server) : m_server(server)
	{
		server->setHandler([this](const MockRequest &request) {
			if (m_interceptor) {
				if (std::optional<MockResponse> response = m_interceptor(request))
					return *response;
			}
			return handle(request);
		});
	}

	UploadTokenProvider tokenProvider()
	{
		return [this](qint64 blogId, UploadTokenCallback callback) {
			++mintCount;
			QVERIFY(blogId == kBlogId);
			const QByteArray jwt = "jwt-" + QByteArray::number(mintCount);
			m_validJwts.insert(jwt);
			callback(QString::fromLatin1(jwt), ApiError());
		};
	}

	void setInterceptor(Interceptor interceptor) { m_interceptor = std::move(interceptor); }
	void expireAllTokens()
	{
		m_validJwts.clear();
		m_validKeyTokens.clear();
	}
	void dropUploads() { uploads.clear(); }

	// Seeds a partially uploaded file, as if a previous OBS session uploaded `bytes`.
	QUrl seedUpload(const QByteArray &key, const QByteArray &bytes, qint64 length)
	{
		uploads.insert(key, Upload{length, bytes, {}});
		return uploadUrl(key);
	}

	QUrl uploadUrl(const QByteArray &key) const
	{
		return m_server->baseUrl().resolved(QUrl(
			QStringLiteral("/rest/v1.1/video-uploads/%1/%2").arg(kBlogId).arg(QString::fromLatin1(key))));
	}

	QHash<QByteArray, Upload> uploads;
	int mintCount = 0;
	int patchCount = 0;
	int headCount = 0;
	int createCount = 0;

private:
	QByteArray issueKeyToken(const QByteArray &key)
	{
		const QByteArray token = "key-" + key + '-' + QByteArray::number(++m_keyTokenCounter);
		m_validKeyTokens.insert(token, key);
		return token;
	}

	MockResponse forbidden(const char *message) const { return MockResponse{403, {}, jsonError(message)}; }

	MockResponse handle(const MockRequest &request)
	{
		const QByteArray base = "/rest/v1.1/video-uploads/" + QByteArray::number(kBlogId);
		const QByteArray token = request.header("x-videopress-upload-token");

		if (request.path == base && request.method == "POST") {
			++createCount;
			if (!m_validJwts.contains(token))
				return forbidden("Missing upload token. Uploading to this blog is not authorized");
			const QByteArray key = "upload" + QByteArray::number(createCount);
			uploads.insert(key, Upload{request.header("upload-length").toLongLong(),
						   {},
						   request.header("upload-metadata")});
			MockResponse response{201, {}, {}};
			response.withHeader("Location", uploadUrl(key).toEncoded())
				.withHeader("x-videopress-upload-key-token", issueKeyToken(key));
			return response;
		}

		if (!request.path.startsWith(base + '/'))
			return MockResponse{404, {}, {}};
		const QByteArray key = request.path.mid(base.size() + 1);
		if (!uploads.contains(key))
			return MockResponse{404, {}, {}};
		Upload &upload = uploads[key];

		// public-api.wordpress.com only routes GET and POST.
		if (request.method == "HEAD")
			return MockResponse{405, {}, {}};
		if (request.method == "GET" && request.header("x-http-method-override") == "HEAD") {
			++headCount;
			if (!m_validJwts.contains(token) && m_validKeyTokens.value(token) != key)
				return forbidden("Missing upload token. Uploading to this blog is not authorized");
			MockResponse response{200, {}, {}};
			response.withHeader("Upload-Offset", QByteArray::number(upload.data.size()))
				.withHeader("Upload-Length", QByteArray::number(upload.length))
				.withHeader("x-videopress-upload-key-token", issueKeyToken(key));
			if (upload.data.size() == upload.length)
				response.withHeader("x-videopress-upload-guid", kGuid);
			return response;
		}

		if (request.method == "POST" && request.header("x-http-method-override") == "PATCH") {
			++patchCount;
			if (m_validKeyTokens.value(token) != key)
				return forbidden("Invalid upload permission for key");
			if (request.header("upload-offset").toLongLong() != upload.data.size())
				return MockResponse{409, {}, "offset mismatch"};
			const QByteArray expected =
				"sha1 " + QCryptographicHash::hash(request.body, QCryptographicHash::Sha1).toBase64();
			if (request.header("upload-checksum") != expected)
				return MockResponse{460, {}, "checksum mismatch"};

			upload.data.append(request.body);
			MockResponse response{204, {}, {}};
			response.withHeader("Upload-Offset", QByteArray::number(upload.data.size()));
			if (upload.data.size() == upload.length) {
				response.withHeader("x-videopress-upload-guid", kGuid)
					.withHeader("x-videopress-upload-media-id", "99")
					.withHeader("x-videopress-upload-src-url", "https://example.com/video.mp4");
			}
			return response;
		}

		return MockResponse{405, {}, {}};
	}

	MockHttpServer *m_server;
	Interceptor m_interceptor;
	QSet<QByteArray> m_validJwts;
	QHash<QByteArray, QByteArray> m_validKeyTokens; // token -> upload key
	int m_keyTokenCounter = 0;
};

} // namespace

class TestTusUpload : public QObject {
	Q_OBJECT

private:
	QTemporaryDir m_dir;
	MockHttpServer *m_server = nullptr;
	FakeVideoPress *m_fake = nullptr;
	QNetworkAccessManager *m_network = nullptr;

	QString writeFile(const QByteArray &contents, const QString &name = QStringLiteral("take.mp4"))
	{
		const QString path = m_dir.filePath(name);
		QFile file(path);
		if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size())
			qFatal("cannot write test file");
		return path;
	}

	static QString metadataValue(const QByteArray &metadata, const char *key)
	{
		for (const QByteArray &pair : metadata.split(',')) {
			const QList<QByteArray> parts = pair.trimmed().split(' ');
			if (parts.value(0) == key)
				return QString::fromUtf8(QByteArray::fromBase64(parts.value(1)));
		}
		return QString();
	}

	static QByteArray pattern(qint64 size)
	{
		QByteArray bytes(size, Qt::Uninitialized);
		for (qint64 i = 0; i < size; ++i)
			bytes[i] = static_cast<char>((i * 31 + 7) % 251);
		return bytes;
	}

	TusUploadOptions options(const QString &path, qint64 chunkSize = 1000) const
	{
		TusUploadOptions opts;
		opts.filePath = path;
		opts.blogId = kBlogId;
		opts.apiBase = m_server->baseUrl();
		opts.chunkSize = chunkSize;
		opts.chunkRetryDelaysMs = {0, 0, 0};
		return opts;
	}

private slots:
	void init()
	{
		QVERIFY(m_dir.isValid());
		m_server = new MockHttpServer(this);
		QVERIFY(m_server->listen());
		m_fake = new FakeVideoPress(m_server);
		m_network = new QNetworkAccessManager(this);
	}

	void cleanup()
	{
		delete m_fake;
		m_fake = nullptr;
		delete m_server;
		m_server = nullptr;
		delete m_network;
		m_network = nullptr;
	}

	void uploadsFileInChunks()
	{
		const QByteArray contents = pattern(3500);
		TusUploadOptions opts = options(writeFile(contents));
		opts.extraMetadata = {{"title", QStringLiteral("Weekly update – take 2")}, {"description", QString()}};
		TusUpload upload(m_network, opts, m_fake->tokenProvider());
		QSignalSpy finished(&upload, &TusUpload::finished);
		QSignalSpy failed(&upload, &TusUpload::failed);
		QSignalSpy progress(&upload, &TusUpload::progress);

		upload.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(failed.count(), 0);

		const auto result = finished.first().first().value<TusUploadResult>();
		QCOMPARE(result.guid, QString::fromLatin1(kGuid));
		QCOMPARE(result.mediaId, qint64(99));
		QCOMPARE(m_fake->createCount, 1);
		QCOMPARE(m_fake->patchCount, 4); // 1000 + 1000 + 1000 + 500
		QCOMPARE(m_fake->mintCount, 1);
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
		QCOMPARE(progress.last().at(0).toLongLong(), qint64(3500));
		QCOMPARE(upload.phase(), TusUpload::Phase::Finished);

		const QByteArray metadata = m_fake->uploads.value("upload1").metadata;
		// A fresh server-side path per attempt: the name gets a random 8-hex suffix.
		QVERIFY2(QRegularExpression(QStringLiteral("^take-[0-9a-f]{8}\\.mp4$"))
				 .match(metadataValue(metadata, "filename"))
				 .hasMatch(),
			 qPrintable(metadataValue(metadata, "filename")));
		QVERIFY(metadata.contains("filetype " + QByteArray("video/mp4").toBase64()));
		QVERIFY(metadata.contains("title " + QStringLiteral("Weekly update – take 2").toUtf8().toBase64()));
		QVERIFY(!metadata.contains("description")); // empty values are left out
	}

	void resumesFromServerOffset()
	{
		const QByteArray contents = pattern(2500);
		TusResumeState resume;
		resume.uploadUrl = m_fake->seedUpload("seeded", contents.left(1000), contents.size());
		resume.keyToken = QStringLiteral("stale-token-from-last-session");
		resume.offset = 0; // the server's offset wins over what was persisted

		TusUpload upload(m_network, options(writeFile(contents)), m_fake->tokenProvider(), resume);
		QSignalSpy finished(&upload, &TusUpload::finished);
		upload.start();
		QVERIFY(finished.wait(5000));

		QCOMPARE(m_fake->createCount, 0);
		QCOMPARE(m_fake->headCount, 1);
		QCOMPARE(m_fake->patchCount, 2); // bytes 1000..2500 only
		QCOMPARE(m_fake->uploads.value("seeded").data, contents);
	}

	void refreshesKeyTokenBeforeItExpires()
	{
		qint64 now = 1'000'000;
		const QByteArray contents = pattern(3000);
		TusUploadOptions opts = options(writeFile(contents));
		opts.tokenRefreshAfterMs = 25 * 60 * 1000;
		TusUpload upload(m_network, opts, m_fake->tokenProvider());
		upload.setClock([&now] { return now; });

		// After the first chunk, jump past the refresh window and expire every token the
		// server issued so far; the uploader must mint a new JWT and HEAD before PATCHing.
		bool expired = false;
		connect(&upload, &TusUpload::resumeStateChanged, this, [&](const TusResumeState &state) {
			if (!expired && state.offset == 1000) {
				expired = true;
				now += 26 * 60 * 1000;
				m_fake->expireAllTokens();
			}
		});

		QSignalSpy finished(&upload, &TusUpload::finished);
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(failed.count(), 0);
		QCOMPARE(m_fake->mintCount, 2);
		QCOMPARE(m_fake->headCount, 1);
		QCOMPARE(m_fake->patchCount, 3);
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
	}

	void recoversFromRejectedKeyToken()
	{
		const QByteArray contents = pattern(2000);
		TusUpload upload(m_network, options(writeFile(contents)), m_fake->tokenProvider());

		bool rejected = false;
		m_fake->setInterceptor([&](const MockRequest &request) -> std::optional<MockResponse> {
			if (!rejected && request.header("x-http-method-override") == "PATCH" &&
			    request.header("upload-offset") == "1000") {
				rejected = true;
				return MockResponse{403, {}, jsonError("Invalid upload permission for key")};
			}
			return std::nullopt;
		});

		QSignalSpy finished(&upload, &TusUpload::finished);
		upload.start();
		QVERIFY(finished.wait(5000));
		QVERIFY(rejected);
		QCOMPARE(m_fake->mintCount, 2);
		QCOMPARE(m_fake->headCount, 1);
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
	}

	void retriesChunkAfterTransient422()
	{
		// The service answers 422 without a body when it can't open the partial file for a chunk.
		const QByteArray contents = pattern(2500);
		int failures = 0;
		m_fake->setInterceptor([&](const MockRequest &request) -> std::optional<MockResponse> {
			if (failures < 2 && request.header("x-http-method-override") == "PATCH" &&
			    request.header("upload-offset") == "1000") {
				++failures;
				return MockResponse{422, {}, {}};
			}
			return std::nullopt;
		});

		TusUpload upload(m_network, options(writeFile(contents)), m_fake->tokenProvider());
		QSignalSpy finished(&upload, &TusUpload::finished);
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(failed.count(), 0);
		QCOMPARE(failures, 2);
		QCOMPARE(m_fake->headCount, 2); // one resync per retry, sent as GET + override
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
	}

	void persistent422FailsWithExplanation()
	{
		m_fake->setInterceptor([](const MockRequest &request) -> std::optional<MockResponse> {
			if (request.header("x-http-method-override") == "PATCH" &&
			    request.header("upload-offset") == "1000")
				return MockResponse{422, {}, {}};
			return std::nullopt;
		});

		TusUpload upload(m_network, options(writeFile(pattern(2500))), m_fake->tokenProvider());
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QVERIFY(failed.wait(5000));
		const auto error = failed.first().first().value<ApiError>();
		QCOMPARE(error.httpStatus, 422);
		QVERIFY2(error.message.contains(QStringLiteral("couldn't store part of the upload")),
			 qPrintable(error.message));
		QCOMPARE(m_fake->headCount, 3);                      // three resyncs before giving up
		QCOMPARE(upload.resumeState().offset, qint64(1000)); // a later Retry resumes here
	}

	void restartsAfterServerIntegrityFailure()
	{
		// The service checks the finished file\'s size; leftover bytes at a path reused from an
		// earlier upload of the same name make it fail, and the server asks for a new upload.
		const QByteArray contents = pattern(2500);
		bool rejected = false;
		m_fake->setInterceptor([&](const MockRequest &request) -> std::optional<MockResponse> {
			if (!rejected && request.header("x-http-method-override") == "PATCH" &&
			    request.header("upload-offset") == "2000") {
				rejected = true;
				MockResponse response{422, {}, {}};
				response.withHeader(
					"x-videopress-upload-error",
					"The uploaded file size does not match the expected size. Please upload the file again.");
				return response;
			}
			return std::nullopt;
		});

		TusUpload upload(m_network, options(writeFile(contents)), m_fake->tokenProvider());
		QSignalSpy finished(&upload, &TusUpload::finished);
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(failed.count(), 0);
		QCOMPARE(m_fake->createCount, 2);
		QCOMPARE(m_fake->uploads.value("upload2").data, contents);
		QVERIFY(metadataValue(m_fake->uploads.value("upload1").metadata, "filename") !=
			metadataValue(m_fake->uploads.value("upload2").metadata, "filename"));
	}

	void restartsWhenServerForgotTheUpload()
	{
		const QByteArray contents = pattern(1500);
		TusResumeState resume;
		resume.uploadUrl = m_fake->uploadUrl("gone");
		resume.keyToken = QStringLiteral("whatever");

		TusUpload upload(m_network, options(writeFile(contents)), m_fake->tokenProvider(), resume);
		QSignalSpy finished(&upload, &TusUpload::finished);
		upload.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(m_fake->createCount, 1);
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
	}

	void quotaRejectionIsTerminal()
	{
		m_fake->setInterceptor([](const MockRequest &request) -> std::optional<MockResponse> {
			if (request.method == "POST" && request.header("x-http-method-override").isEmpty())
				return MockResponse{403, {}, jsonError("Not enough space available on this site.")};
			return std::nullopt;
		});

		TusUpload upload(m_network, options(writeFile(pattern(100))), m_fake->tokenProvider());
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QVERIFY(failed.wait(5000));

		const auto error = failed.first().first().value<ApiError>();
		QCOMPARE(error.kind, ErrorKind::Forbidden);
		QCOMPARE(error.forbiddenReason, ForbiddenReason::QuotaExceeded);
		QVERIFY(!error.isRetryable());
		QCOMPARE(upload.phase(), TusUpload::Phase::Failed);
	}

	void serverErrorKeepsResumeStateForRetry()
	{
		const QByteArray contents = pattern(3000);
		m_fake->setInterceptor([](const MockRequest &request) -> std::optional<MockResponse> {
			if (request.header("upload-offset") == "2000")
				return MockResponse{503, {}, "try later"};
			return std::nullopt;
		});

		const QString path = writeFile(contents);
		TusUpload first(m_network, options(path), m_fake->tokenProvider());
		QSignalSpy failed(&first, &TusUpload::failed);
		first.start();
		QVERIFY(failed.wait(5000));
		const auto error = failed.first().first().value<ApiError>();
		QCOMPARE(error.kind, ErrorKind::Server);
		QVERIFY(error.isRetryable());

		const TusResumeState resume = first.resumeState();
		QCOMPARE(resume.offset, qint64(2000));
		const TusResumeState restored = TusResumeState::fromJson(resume.toJson());
		QCOMPARE(restored.uploadUrl, resume.uploadUrl);
		QCOMPARE(restored.offset, resume.offset);

		// The queue retries with a fresh TusUpload from the persisted state.
		m_fake->setInterceptor(nullptr);
		TusUpload second(m_network, options(path), m_fake->tokenProvider(), restored);
		QSignalSpy finished(&second, &TusUpload::finished);
		second.start();
		QVERIFY(finished.wait(5000));
		QCOMPARE(m_fake->createCount, 1);
		QCOMPARE(m_fake->uploads.value("upload1").data, contents);
	}

	void tokenProviderErrorFailsUpload()
	{
		TusUpload upload(m_network, options(writeFile(pattern(100))), [](qint64, UploadTokenCallback callback) {
			callback(QString(), ApiError::fromHttp(403, R"({"error":"invalid_token","message":"bad"})"));
		});
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QCOMPARE(failed.count(), 1);
		QVERIFY(failed.first().first().value<ApiError>().needsReconnect());
		QCOMPARE(m_fake->createCount, 0);
	}

	void cancelStopsTheUpload()
	{
		UploadTokenCallback pending;
		TusUpload upload(m_network, options(writeFile(pattern(100))),
				 [&pending](qint64, UploadTokenCallback callback) { pending = std::move(callback); });
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		upload.cancel();
		QCOMPARE(failed.count(), 1);
		QCOMPARE(failed.first().first().value<ApiError>().kind, ErrorKind::Cancelled);

		// A token arriving after cancel() is ignored.
		pending(QStringLiteral("jwt-late"), ApiError());
		QCOMPARE(m_fake->createCount, 0);
		QCOMPARE(failed.count(), 1);
	}

	void missingFileFailsLocally()
	{
		TusUpload upload(m_network, options(m_dir.filePath(QStringLiteral("nope.mp4"))),
				 m_fake->tokenProvider());
		QSignalSpy failed(&upload, &TusUpload::failed);
		upload.start();
		QCOMPARE(failed.count(), 1);
		QCOMPARE(failed.first().first().value<ApiError>().kind, ErrorKind::Local);
		QCOMPARE(m_fake->mintCount, 0);
	}
};

QTEST_GUILESS_MAIN(TestTusUpload)
#include "test-tus-upload.moc"

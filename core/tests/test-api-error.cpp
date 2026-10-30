// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "api-error.hpp"

#include <QTest>

using namespace videopress;

class TestApiError : public QObject {
	Q_OBJECT

private slots:
	void parsesUploaderJsonError()
	{
		const ApiError error = ApiError::fromHttp(
			403,
			R"({"success":false,"data":{"error":"Access Forbidden","message":"Jetpack Video is disabled."}})");
		QCOMPARE(error.kind, ErrorKind::Forbidden);
		QCOMPARE(error.code, QStringLiteral("Access Forbidden"));
		QCOMPARE(error.forbiddenReason, ForbiddenReason::VideoPressDisabled);
		QVERIFY(!error.isRetryable());
	}

	void parsesRestV11Error()
	{
		const ApiError error =
			ApiError::fromHttp(403, R"({"error":"unauthorized","message":"Your trial has expired"})");
		QCOMPARE(error.code, QStringLiteral("unauthorized"));
		QCOMPARE(error.forbiddenReason, ForbiddenReason::TrialExpired);
		QVERIFY(!error.needsReconnect());
	}

	void parsesRestV2Error()
	{
		const ApiError error = ApiError::fromHttp(
			401,
			R"({"code":"rest_forbidden","message":"Sorry, you are not allowed to do that.","data":{"status":401}})");
		QCOMPARE(error.kind, ErrorKind::Unauthorized);
		QCOMPARE(error.code, QStringLiteral("rest_forbidden"));
	}

	void invalidTokenMeansReconnect()
	{
		QVERIFY(ApiError::fromHttp(400, R"({"error":"invalid_token","message":"The OAuth2 token is invalid."})")
				.needsReconnect());
		QVERIFY(ApiError::fromHttp(403, R"({"error":"authorization_required","message":"x"})").needsReconnect());
	}

	void plainTextBody()
	{
		const ApiError error = ApiError::fromHttp(502, "  Bad gateway\n");
		QCOMPARE(error.kind, ErrorKind::Server);
		QCOMPARE(error.message, QStringLiteral("Bad gateway"));
		QVERIFY(error.isRetryable());
	}

	void htmlErrorPagesBecomeSentences()
	{
		const ApiError limited = ApiError::fromHttp(
			429,
			"<!DOCTYPE html>\n<html lang=\"en\"><head><title>429 Too Many Requests</title></head></html>");
		QCOMPARE(limited.kind, ErrorKind::Server);
		QVERIFY(limited.isRetryable());
		QVERIFY(!limited.message.contains(QLatin1Char('<')));
		QVERIFY(limited.message.contains(QStringLiteral("Wait a minute")));
		QVERIFY(ApiError::fromHttp(503, "<html></html>").message.contains(QStringLiteral("HTTP 503")));
	}

	void successIsNotAnError() { QVERIFY(!ApiError::fromHttp(204, QByteArray()).isError()); }

	void classifiesForbiddenMessages_data()
	{
		QTest::addColumn<QString>("message");
		QTest::addColumn<int>("reason");
		QTest::newRow("blocked") << QStringLiteral("VideoPress uploading is blocked for this site.")
					 << int(ForbiddenReason::SiteBlocked);
		QTest::newRow("missing token")
			<< QStringLiteral("Missing upload token. Uploading to this blog is not authorized")
			<< int(ForbiddenReason::InvalidUploadToken);
		QTest::newRow("key token") << QStringLiteral("Invalid upload permission for key")
					   << int(ForbiddenReason::InvalidUploadToken);
		QTest::newRow("not authorized") << QStringLiteral("Uploading to this blog is not authorized")
						<< int(ForbiddenReason::NoUploadPermission);
		QTest::newRow("cannot upload")
			<< QStringLiteral("User cannot upload media.") << int(ForbiddenReason::NoUploadPermission);
		QTest::newRow("space")
			<< QStringLiteral("Available space check failed.") << int(ForbiddenReason::QuotaExceeded);
		QTest::newRow("mime") << QStringLiteral("Invalid Mime") << int(ForbiddenReason::InvalidMime);
		QTest::newRow("other") << QStringLiteral("Nope") << int(ForbiddenReason::Unknown);
	}

	void classifiesForbiddenMessages()
	{
		QFETCH(QString, message);
		QFETCH(int, reason);
		QCOMPARE(int(classifyForbidden(message)), reason);
	}

	void invalidUploadTokenIsRetryable()
	{
		const ApiError error = ApiError::fromHttp(
			403,
			R"({"success":false,"data":{"error":"Access Forbidden","message":"Invalid upload permission"}})");
		QVERIFY(error.isRetryable());
	}
};

QTEST_GUILESS_MAIN(TestApiError)
#include "test-api-error.moc"

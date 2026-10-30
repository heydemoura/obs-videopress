// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "oauth.hpp"

#include <QRegularExpression>
#include <QSet>
#include <QTest>
#include <QUrlQuery>

using namespace videopress;

class TestOAuth : public QObject {
	Q_OBJECT

private:
	static OAuthConfig config()
	{
		OAuthConfig cfg;
		cfg.clientId = QStringLiteral("12345");
		cfg.redirectUri = QUrl(QStringLiteral("https://videopress.com/obs/oauth-callback"));
		return cfg;
	}

private slots:
	void stateIsRandomAndUrlSafe()
	{
		QSet<QString> seen;
		static const QRegularExpression urlSafe(QStringLiteral("^[A-Za-z0-9_-]{43}$"));
		for (int i = 0; i < 100; ++i) {
			const QString state = generateOAuthState();
			QVERIFY2(urlSafe.match(state).hasMatch(), qPrintable(state));
			seen.insert(state);
		}
		QCOMPARE(seen.size(), 100);
	}

	void authorizeUrlRequestsGlobalCodeGrant()
	{
		const QUrl url = buildAuthorizeUrl(config(), QStringLiteral("st-1"));
		QCOMPARE(url.host(), QStringLiteral("public-api.wordpress.com"));
		QCOMPARE(url.path(), QStringLiteral("/oauth2/authorize"));

		const QUrlQuery query(url);
		QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), QStringLiteral("12345"));
		QCOMPARE(query.queryItemValue(QStringLiteral("response_type")), QStringLiteral("code"));
		QCOMPARE(query.queryItemValue(QStringLiteral("scope")), QStringLiteral("global"));
		QCOMPARE(query.queryItemValue(QStringLiteral("state")), QStringLiteral("st-1"));
		QCOMPARE(query.queryItemValue(QStringLiteral("redirect_uri"), QUrl::FullyDecoded),
			 QStringLiteral("https://videopress.com/obs/oauth-callback"));
	}

	void parseRedirect_data()
	{
		QTest::addColumn<QString>("url");
		QTest::addColumn<int>("status");
		QTest::addColumn<QString>("code");

		const QString base = QStringLiteral("https://videopress.com/obs/oauth-callback");
		QTest::newRow("success") << base + QStringLiteral("?code=abc%2B1&state=good")
					 << int(RedirectStatus::Success) << QStringLiteral("abc+1");
		QTest::newRow("wrong state") << base + QStringLiteral("?code=abc&state=evil")
					     << int(RedirectStatus::StateMismatch) << QString();
		QTest::newRow("missing state")
			<< base + QStringLiteral("?code=abc") << int(RedirectStatus::StateMismatch) << QString();
		QTest::newRow("denied") << base + QStringLiteral("?error=access_denied&state=good")
					<< int(RedirectStatus::Denied) << QString();
		QTest::newRow("no code")
			<< base + QStringLiteral("?state=good") << int(RedirectStatus::Malformed) << QString();
		QTest::newRow("login page")
			<< QStringLiteral(
				   "https://wordpress.com/log-in?redirect_to=https%3A%2F%2Fvideopress.com%2Fobs%2Foauth-callback%3Fcode%3Dx")
			<< int(RedirectStatus::NotRedirect) << QString();
		QTest::newRow("other path")
			<< QStringLiteral("https://videopress.com/obs/oauth-callback-evil?code=a&state=good")
			<< int(RedirectStatus::NotRedirect) << QString();
		QTest::newRow("http downgrade")
			<< QStringLiteral("http://videopress.com/obs/oauth-callback?code=a&state=good")
			<< int(RedirectStatus::NotRedirect) << QString();
	}

	void parseRedirect()
	{
		QFETCH(QString, url);
		QFETCH(int, status);
		QFETCH(QString, code);

		const RedirectResult result =
			videopress::parseRedirect(QUrl(url), config().redirectUri, QStringLiteral("good"));
		QCOMPARE(int(result.status), status);
		QCOMPARE(result.code, code);
	}

	void loopbackRedirectMatchesPort()
	{
		const QUrl redirect(QStringLiteral("http://127.0.0.1:53682/callback"));
		QVERIFY(isRedirectTo(QUrl(QStringLiteral("http://127.0.0.1:53682/callback?code=x")), redirect));
		QVERIFY(!isRedirectTo(QUrl(QStringLiteral("http://127.0.0.1:53683/callback?code=x")), redirect));
	}

	void tokenRequestHasNoSecret()
	{
		const QByteArray body = buildTokenRequestBody(config(), QStringLiteral("a+b/c"));
		const QUrlQuery query(QString::fromUtf8(body));
		QCOMPARE(query.queryItemValue(QStringLiteral("grant_type")), QStringLiteral("authorization_code"));
		QCOMPARE(query.queryItemValue(QStringLiteral("client_id")), QStringLiteral("12345"));
		QVERIFY(!body.contains("client_secret"));
		QVERIFY(body.contains("code=a%2Bb%2Fc"));
	}

	void parsesTokenResponse()
	{
		TokenResponse token;
		ApiError error;
		QVERIFY(parseTokenResponse(
			R"({"access_token":"tok","token_type":"bearer","blog_id":"0","blog_url":"","scope":"global"})",
			&token, &error));
		QCOMPARE(token.accessToken, QStringLiteral("tok"));
		QCOMPARE(token.scope, QStringLiteral("global"));
		QCOMPARE(token.blogId, qint64(0));

		QVERIFY(parseTokenResponse(R"({"access_token":"tok","blog_id":123})", &token, &error));
		QCOMPARE(token.blogId, qint64(123));

		QVERIFY(!parseTokenResponse(R"({"error":"invalid_grant"})", &token, &error));
		QCOMPARE(error.kind, ErrorKind::Protocol);
		QVERIFY(!parseTokenResponse("<html>", &token, &error));
	}

	void constantTimeEqualsComparesBytes()
	{
		QVERIFY(constantTimeEquals("abc", "abc"));
		QVERIFY(!constantTimeEquals("abc", "abd"));
		QVERIFY(!constantTimeEquals("abc", "abcd"));
		QVERIFY(constantTimeEquals(QByteArray(), QByteArray()));
	}
};

QTEST_GUILESS_MAIN(TestOAuth)
#include "test-oauth.moc"

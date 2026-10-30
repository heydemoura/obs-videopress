// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jetpack-site-client.hpp"
#include "mock-http-server.hpp"

#include <QNetworkAccessManager>
#include <QTest>
#include <QUrlQuery>

using namespace videopress;

namespace {

// The REST route a request targets in either URL form, or a null string for other pages.
QString routeOf(const MockRequest &request)
{
	const QUrl url(QString::fromUtf8(request.path));
	const QUrlQuery query(url);
	if (query.hasQueryItem(QStringLiteral("rest_route")))
		return query.queryItemValue(QStringLiteral("rest_route"), QUrl::FullyDecoded);
	for (const QString prefix : {QStringLiteral("/wp-json"), QStringLiteral("/custom-api")}) {
		if (url.path().startsWith(prefix)) {
			const QString route = url.path().mid(prefix.size());
			return route.isEmpty() ? QStringLiteral("/") : route;
		}
	}
	return QString();
}

MockResponse html(int status)
{
	MockResponse response{status, {}, "<!DOCTYPE html><html><head><title>Home</title></head></html>"};
	response.withHeader("Content-Type", "text/html; charset=UTF-8");
	return response;
}

MockResponse json(int status, const QByteArray &body)
{
	MockResponse response{status, {}, body};
	response.withHeader("Content-Type", "application/json");
	return response;
}

} // namespace

class TestJetpackSiteClient : public QObject {
	Q_OBJECT

private:
	MockHttpServer *m_server = nullptr;
	QNetworkAccessManager *m_network = nullptr;
	bool m_canUpload = true;
	QByteArray m_index;
	QByteArray m_linkHeader;             // homepage Link header, if any
	bool m_prettyServesHtml = false;     // /wp-json/ answered by a 404 theme page
	bool m_queryRouteServesHtml = false; // ?rest_route= answered by a cached homepage
	QByteArray m_jwtResponse;
	int m_jwtStatus = 200;

	SiteCredentials credentials() const
	{
		return {m_server->baseUrl(), QStringLiteral("admin"), QStringLiteral("abcd efgh ijkl mnop")};
	}

	struct VerifyOutcome {
		SiteVerification result;
		ApiError error;
	};

	VerifyOutcome runVerify()
	{
		JetpackSiteClient client(m_network, credentials());
		VerifyOutcome outcome;
		bool done = false;
		client.verify([&](const SiteVerification &result, const ApiError &error) {
			outcome = {result, error};
			done = true;
		});
		if (!QTest::qWaitFor([&] { return done; }, 5000))
			qFatal("verify() never completed");
		return outcome;
	}

private slots:
	void init()
	{
		m_canUpload = true;
		m_jwtStatus = 200;
		m_linkHeader.clear();
		m_prettyServesHtml = false;
		m_queryRouteServesHtml = false;
		m_index =
			R"({"name":"My Video Blog","namespaces":["wp/v2","jetpack/v4","wpcom/v2"],"authentication":{"application-passwords":{"endpoints":{"authorization":"https://example.com/wp-admin/authorize-application.php"}}}})";
		m_jwtResponse =
			R"({"upload_token":"jwt-1","upload_url":"https://public-api.wordpress.com/rest/v1.1/video-uploads/777/","upload_blog_id":777})";
		m_server = new MockHttpServer(this);
		QVERIFY(m_server->listen());
		m_network = new QNetworkAccessManager(this);
		m_server->setHandler([this](const MockRequest &request) {
			const QString route = routeOf(request);
			if (request.path.startsWith("/old/")) {
				// e.g. example.wordpress.com redirecting to its custom domain
				MockResponse moved{301, {}, {}};
				moved.withHeader("Location", m_server->baseUrl().toEncoded() + "/");
				return moved;
			}
			if (route.isNull()) {
				MockResponse page = html(200);
				if (!m_linkHeader.isEmpty())
					page.withHeader("Link", m_linkHeader);
				return page;
			}
			const bool pretty = request.path.startsWith("/wp-json");
			const bool custom = request.path.startsWith("/custom-api");
			if (pretty && m_prettyServesHtml)
				return html(404);
			if (!pretty && !custom && m_queryRouteServesHtml)
				return html(200);
			// The REST index is public on WordPress sites.
			if (route == QLatin1String("/"))
				return json(200, m_index);

			const QByteArray expectedAuth = "Basic " + QByteArray("admin:abcd efgh ijkl mnop").toBase64();
			if (request.header("authorization") != expectedAuth)
				return json(
					401,
					R"({"code":"incorrect_password","message":"The provided password is an invalid application password.","data":{"status":401}})");

			if (route == QLatin1String("/wp/v2/users/me"))
				return json(200, QByteArray(R"({"name":"Heyde","capabilities":{"upload_files":)") +
							 (m_canUpload ? "true" : "false") + "}}");
			if (route == QLatin1String("/wpcom/v2/videopress/upload-jwt") && request.method == "POST")
				return json(m_jwtStatus, m_jwtResponse);
			return json(404, R"({"code":"rest_no_route","message":"No route"})");
		});
	}

	void cleanup()
	{
		delete m_server;
		m_server = nullptr;
		delete m_network;
		m_network = nullptr;
	}

	void verifiesSite()
	{
		const VerifyOutcome outcome = runVerify();
		QVERIFY2(!outcome.error.isError(), qPrintable(outcome.error.message));
		QCOMPARE(outcome.result.siteName, QStringLiteral("My Video Blog"));
		QCOMPARE(outcome.result.userDisplayName, QStringLiteral("Heyde"));
		QCOMPARE(outcome.result.blogId, qint64(777));
		QCOMPARE(m_server->requests().size(), 3);
	}

	void rejectsUserWithoutUploadCapability()
	{
		m_canUpload = false;
		const VerifyOutcome outcome = runVerify();
		QCOMPARE(outcome.error.forbiddenReason, ForbiddenReason::NoUploadPermission);
		QCOMPARE(m_server->requests().size(), 2); // never asked for a token
	}

	void explainsUnconnectedUser()
	{
		m_jwtStatus = 403;
		m_jwtResponse =
			R"({"code":"rest_forbidden","message":"Sorry, you are not allowed to do that.","data":{"status":403}})";
		const VerifyOutcome outcome = runVerify();
		QCOMPARE(outcome.error.kind, ErrorKind::Forbidden);
		QVERIFY(outcome.error.message.contains(QStringLiteral("Connect the user to WordPress.com")));
	}

	void reportsWrongPassword()
	{
		JetpackSiteClient client(m_network,
					 {m_server->baseUrl(), QStringLiteral("admin"), QStringLiteral("nope")});
		ApiError error;
		bool done = false;
		client.verify([&](const SiteVerification &, const ApiError &e) {
			error = e;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QCOMPARE(error.kind, ErrorKind::Unauthorized);
		QVERIFY(error.message.contains(QStringLiteral("application password")));
	}

	void tokenProviderFeedsTusUpload()
	{
		JetpackSiteClient client(m_network, credentials());
		QString token;
		bool done = false;
		client.tokenProvider()(777, [&](const QString &t, const ApiError &) {
			token = t;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QCOMPARE(token, QStringLiteral("jwt-1"));
	}

	void probesSiteWithoutCredentials()
	{
		JetpackSiteClient client(m_network, {m_server->baseUrl(), QString(), QString(), QUrl()});
		SiteProbe probe;
		ApiError error;
		bool done = false;
		client.probe([&](const SiteProbe &result, const ApiError &e) {
			probe = result;
			error = e;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QCOMPARE(probe.siteName, QStringLiteral("My Video Blog"));
		QVERIFY(probe.applicationPasswordsAdvertised);
		QVERIFY(m_server->requests().last().header("authorization").isEmpty());
		QCOMPARE(m_server->requests().last().header("user-agent"), QByteArray("obs-videopress"));
	}

	void probeRejectsSiteWithoutJetpack()
	{
		m_index = R"({"name":"Plain WordPress","namespaces":["wp/v2"]})";
		JetpackSiteClient client(m_network, {m_server->baseUrl(), QString(), QString(), QUrl()});
		ApiError error;
		bool done = false;
		client.probe([&](const SiteProbe &, const ApiError &e) {
			error = e;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QVERIFY(error.isError());
		QVERIFY(error.message.contains(QStringLiteral("Jetpack")));
	}

	void applicationPasswordsUrlPointsAtTheSite()
	{
		QCOMPARE(JetpackSiteClient::applicationPasswordsUrl(QUrl(QStringLiteral("https://example.com")))
				 .toString(),
			 QStringLiteral("https://example.com/wp-admin/users.php?page=application-passwords"));
		QCOMPARE(JetpackSiteClient::applicationPasswordsUrl(QUrl(QStringLiteral("https://example.com/blog")))
				 .toString(),
			 QStringLiteral("https://example.com/blog/wp-admin/users.php?page=application-passwords"));
	}

	SiteProbe runProbe(ApiError *error)
	{
		JetpackSiteClient client(m_network, {m_server->baseUrl(), QString(), QString(), QUrl()});
		SiteProbe probe;
		bool done = false;
		client.probe([&](const SiteProbe &result, const ApiError &e) {
			probe = result;
			*error = e;
			done = true;
		});
		if (!QTest::qWaitFor([&] { return done; }, 5000))
			qFatal("probe() never completed");
		return probe;
	}

	void probeUsesAdvertisedApiRootAsFallback()
	{
		const QByteArray root = m_server->baseUrl().toEncoded() + "/custom-api/";
		m_linkHeader = "<" + root + R"(>; rel="https://api.w.org/", <https://wp.me/x>; rel=shortlink)";
		m_prettyServesHtml = true;
		m_queryRouteServesHtml = true;
		ApiError error;
		const SiteProbe probe = runProbe(&error);
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QCOMPARE(probe.apiRoot.toEncoded(), root);
		QCOMPARE(m_server->requests().size(), 4); // homepage, /wp-json/, ?rest_route=, advertised
		QCOMPARE(m_server->requests().first().header("cache-control"), QByteArray("no-cache"));
	}

	void staleWordPressComLinkDoesNotOverrideOwnApi()
	{
		// The reported bug: a cached homepage from before a move to managed hosting still
		// advertised public-api.wordpress.com, while the site served its own /wp-json/.
		m_linkHeader =
			R"(<https://public-api.wordpress.com/wp-json/?rest_route=/sites/example.wordpress.com>; rel="https://api.w.org/")";
		ApiError error;
		const SiteProbe probe = runProbe(&error);
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QVERIFY(!probe.hostedOnWordPressCom);
		QCOMPARE(probe.apiRoot.path(), QStringLiteral("/wp-json/"));
		QVERIFY(probe.applicationPasswordsAdvertised);
	}

	void probeFallsBackWhenPrettyPermalinksAreOff()
	{
		m_prettyServesHtml = true;
		ApiError error;
		const SiteProbe probe = runProbe(&error);
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QVERIFY(QUrlQuery(probe.apiRoot).hasQueryItem(QStringLiteral("rest_route")));
	}

	void probeSkipsCachedHomepageForQueryRoute()
	{
		// The reported bug: ?rest_route=/ answered with the cached homepage (HTTP 200 HTML).
		m_queryRouteServesHtml = true;
		ApiError error;
		const SiteProbe probe = runProbe(&error);
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QCOMPARE(probe.apiRoot.path(), QStringLiteral("/wp-json/"));
	}

	void probeExplainsWhatTheSiteReturned()
	{
		m_prettyServesHtml = true;
		m_queryRouteServesHtml = true;
		ApiError error;
		runProbe(&error);
		QVERIFY(error.isError());
		QVERIFY2(error.message.contains(QStringLiteral("text/html")), qPrintable(error.message));
		QVERIFY2(error.message.contains(QStringLiteral("HTTP 200")), qPrintable(error.message));
	}

	void probeFollowsHomepageRedirect()
	{
		QUrl moved = m_server->baseUrl();
		moved.setPath(QStringLiteral("/old"));
		JetpackSiteClient client(m_network, {moved, QString(), QString(), QUrl()});
		SiteProbe probe;
		ApiError error;
		bool done = false;
		client.probe([&](const SiteProbe &result, const ApiError &e) {
			probe = result;
			error = e;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QCOMPARE(probe.siteUrl, m_server->baseUrl()); // the address that answered
		QCOMPARE(probe.apiRoot.path(), QStringLiteral("/wp-json/"));
	}

	void probeRecognizesWordPressComHostedSites()
	{
		// Sites hosted on WordPress.com have no REST API on their own domain; only public-api answers.
		m_linkHeader =
			R"(<https://public-api.wordpress.com/wp-json/?rest_route=/sites/example.wordpress.com>; rel="https://api.w.org/")";
		m_prettyServesHtml = true;
		m_queryRouteServesHtml = true;
		ApiError error;
		const SiteProbe probe = runProbe(&error);
		QVERIFY2(!error.isError(), qPrintable(error.message));
		QVERIFY(probe.hostedOnWordPressCom);
		QCOMPARE(m_server->requests().size(), 3); // homepage + own API attempts; never public-api
	}

	void authenticatedRequestsUseTheDiscoveredRoot()
	{
		QUrl root = m_server->baseUrl();
		root.setPath(QStringLiteral("/wp-json/"));
		JetpackSiteClient client(m_network, {m_server->baseUrl(), QStringLiteral("admin"),
						     QStringLiteral("abcd efgh ijkl mnop"), root});
		bool done = false;
		ApiError error;
		client.verify([&](const SiteVerification &, const ApiError &e) {
			error = e;
			done = true;
		});
		QVERIFY(QTest::qWaitFor([&] { return done; }, 5000));
		QVERIFY2(!error.isError(), qPrintable(error.message));
		for (const MockRequest &request : m_server->requests())
			QVERIFY2(request.path.startsWith("/wp-json/"), request.path.constData());
		QCOMPARE(m_server->requests().at(1).path, QByteArray("/wp-json/wp/v2/users/me?context=edit"));
	}

	void routeUrlHandlesBothForms()
	{
		QUrlQuery extra;
		extra.addQueryItem(QStringLiteral("context"), QStringLiteral("edit"));
		QCOMPARE(JetpackSiteClient::routeUrl(QUrl(QStringLiteral("https://example.com/blog/wp-json/")),
						     QStringLiteral("/wp/v2/users/me"), extra)
				 .toString(),
			 QStringLiteral("https://example.com/blog/wp-json/wp/v2/users/me?context=edit"));
		QCOMPARE(JetpackSiteClient::routeUrl(QUrl(QStringLiteral("https://example.com/?rest_route=/")),
						     QStringLiteral("/wp/v2/users/me"), extra)
				 .toString(),
			 QStringLiteral("https://example.com/?rest_route=/wp/v2/users/me&context=edit"));
	}

	void parsesJsonAfterNoise()
	{
		QJsonObject obj;
		QVERIFY(JetpackSiteClient::parseJsonObject("\xEF\xBB\xBF{\"name\":\"A\"}", &obj));
		QCOMPARE(obj.value(QStringLiteral("name")).toString(), QStringLiteral("A"));
		QVERIFY(JetpackSiteClient::parseJsonObject(
			"<br />\n<b>Notice</b>: Undefined index in functions.php<br />\n{\"name\":\"B\"}", &obj));
		QCOMPARE(obj.value(QStringLiteral("name")).toString(), QStringLiteral("B"));
		QVERIFY(!JetpackSiteClient::parseJsonObject("<!DOCTYPE html><html></html>", &obj));
	}

	void normalizesSiteUrls_data()
	{
		QTest::addColumn<QString>("input");
		QTest::addColumn<QString>("expected");
		QTest::newRow("bare host") << QStringLiteral("example.com") << QStringLiteral("https://example.com");
		QTest::newRow("trailing slash")
			<< QStringLiteral("https://example.com/blog/") << QStringLiteral("https://example.com/blog");
		QTest::newRow("wp-admin")
			<< QStringLiteral("https://example.com/wp-admin/") << QStringLiteral("https://example.com");
		QTest::newRow("query")
			<< QStringLiteral("https://example.com/?p=1") << QStringLiteral("https://example.com");
		QTest::newRow("http rejected") << QStringLiteral("http://example.com") << QString();
		QTest::newRow("http localhost")
			<< QStringLiteral("http://localhost:8888") << QStringLiteral("http://localhost:8888");
	}

	void normalizesSiteUrls()
	{
		QFETCH(QString, input);
		QFETCH(QString, expected);
		QString error;
		const QUrl url = JetpackSiteClient::normalizeSiteUrl(input, &error);
		QCOMPARE(url.toString(), expected);
		QCOMPARE(error.isEmpty(), !expected.isEmpty());
	}
};

QTEST_GUILESS_MAIN(TestJetpackSiteClient)
#include "test-jetpack-site-client.moc"

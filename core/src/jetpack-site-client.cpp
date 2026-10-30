// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jetpack-site-client.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrlQuery>

#include <memory>
#include <utility>

namespace videopress {

namespace {

constexpr int kTimeoutMs = 30 * 1000;
constexpr int kMaxProbeRedirects = 3;

bool isLocalHost(const QString &host)
{
	return host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") ||
	       host == QLatin1String("::1") || host.endsWith(QLatin1String(".local")) ||
	       host.endsWith(QLatin1String(".test"));
}

constexpr const char *kWordPressComApiHost = "public-api.wordpress.com";

// What came back instead of REST API JSON, in words a user can act on.
ApiError notRestApiError(QNetworkReply *reply)
{
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	QString type = reply->header(QNetworkRequest::ContentTypeHeader).toString().section(QLatin1Char(';'), 0, 0);
	if (type.isEmpty())
		type = QStringLiteral("no content type");
	ApiError error = ApiError::protocol(
		QStringLiteral("%1 answered with %2 (HTTP %3) instead of the WordPress REST API. A caching or "
			       "security plugin, or a maintenance or coming-soon mode, may be blocking it.")
			.arg(reply->url().toString(QUrl::RemoveUserInfo), type)
			.arg(status));
	error.httpStatus = status;
	return error;
}

// A failed reply, or a successful one whose body is not a JSON object.
ApiError replyError(QNetworkReply *reply, const QByteArray &body, QJsonObject *json)
{
	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status >= 300 && status < 400) {
		const QUrl target =
			reply->url().resolved(reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
		ApiError error = ApiError::protocol(
			QStringLiteral("The site redirected to %1. Use that address as the site URL.")
				.arg(target.adjusted(QUrl::RemoveQuery).toString()));
		error.httpStatus = status;
		return error;
	}

	ApiError error = ApiError::fromReply(reply, body);
	if (error.isError())
		return error;

	if (!JetpackSiteClient::parseJsonObject(body, json))
		return notRestApiError(reply);
	return ApiError();
}

} // namespace

JetpackSiteClient::JetpackSiteClient(QNetworkAccessManager *network, SiteCredentials credentials, QObject *parent)
	: QObject(parent),
	  m_network(network),
	  m_credentials(std::move(credentials))
{
}

QUrl JetpackSiteClient::normalizeSiteUrl(const QString &input, QString *error)
{
	QString text = input.trimmed();
	if (!text.contains(QLatin1String("://")))
		text.prepend(QLatin1String("https://"));

	QUrl url(text, QUrl::StrictMode);
	if (!url.isValid() || url.host().isEmpty()) {
		*error = QStringLiteral("Enter the site address, for example https://example.com");
		return QUrl();
	}
	if (url.scheme() != QLatin1String("https") && !isLocalHost(url.host())) {
		*error = QStringLiteral(
			"The site must use https:// (application passwords are sent with every request)");
		return QUrl();
	}

	url.setQuery(QString());
	url.setFragment(QString());
	QString path = url.path();
	for (const QString &suffix : {QStringLiteral("/wp-admin"), QStringLiteral("/wp-login.php")}) {
		if (path.endsWith(suffix) || path.endsWith(suffix + QLatin1Char('/')))
			path = path.left(path.lastIndexOf(suffix));
	}
	while (path.endsWith(QLatin1Char('/')))
		path.chop(1);
	url.setPath(path);
	return url;
}

QUrl JetpackSiteClient::applicationPasswordsUrl(const QUrl &siteUrl)
{
	QUrl url = siteUrl;
	url.setPath(siteUrl.path() + QStringLiteral("/wp-admin/users.php"));
	url.setQuery(QStringLiteral("page=application-passwords"));
	return url;
}

QUrl JetpackSiteClient::apiRootFromLinkHeader(const QByteArray &linkHeader, const QUrl &base)
{
	// Link: <https://example.com/wp-json/>; rel="https://api.w.org/", <...>; rel=shortlink
	static const QRegularExpression pattern(
		QStringLiteral(R"(<([^>]+)>\s*;[^,<]*rel\s*=\s*"?https://api\.w\.org/"?)"));
	const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(linkHeader));
	if (!match.hasMatch())
		return QUrl();
	const QUrl url = base.resolved(QUrl(match.captured(1)));
	return url.isValid() ? url : QUrl();
}

QUrl JetpackSiteClient::routeUrl(const QUrl &apiRoot, const QString &route, const QUrlQuery &extraQuery)
{
	QUrl url = apiRoot;
	QUrlQuery query(apiRoot);
	if (query.hasQueryItem(QStringLiteral("rest_route"))) {
		// https://example.com/?rest_route=/ -> ?rest_route=/wp/v2/users/me
		query.removeAllQueryItems(QStringLiteral("rest_route"));
		query.addQueryItem(QStringLiteral("rest_route"), route);
	} else {
		// https://example.com/wp-json/ -> https://example.com/wp-json/wp/v2/users/me
		QString path = url.path();
		if (!path.endsWith(QLatin1Char('/')))
			path += QLatin1Char('/');
		url.setPath(path + (route.startsWith(QLatin1Char('/')) ? route.mid(1) : route));
	}
	for (const auto &item : extraQuery.queryItems(QUrl::FullyDecoded))
		query.addQueryItem(item.first, item.second);
	url.setQuery(query);
	return url;
}

bool JetpackSiteClient::parseJsonObject(const QByteArray &body, QJsonObject *out)
{
	QJsonDocument doc = QJsonDocument::fromJson(body);
	if (!doc.isObject()) {
		// Plugins and themes sometimes print a BOM or PHP notices before the JSON.
		const qsizetype start = body.indexOf('{');
		if (start > 0)
			doc = QJsonDocument::fromJson(body.mid(start));
	}
	if (!doc.isObject())
		return false;
	*out = doc.object();
	return true;
}

QNetworkRequest JetpackSiteClient::baseRequest(const QUrl &url) const
{
	QNetworkRequest req(url);
	req.setRawHeader("Accept", "application/json");
	// Never Qt's default "Mozilla/5.0": WordPress.com's edge answers 429 to that combined
	// with an application password.
	req.setHeader(QNetworkRequest::UserAgentHeader,
		      m_userAgent.isEmpty() ? QByteArrayLiteral("obs-videopress") : m_userAgent);
	// Never replay the Authorization header to wherever a redirect points.
	req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
	req.setTransferTimeout(kTimeoutMs);
	return req;
}

QNetworkRequest JetpackSiteClient::request(const QString &route, const QUrlQuery &extraQuery) const
{
	QUrl root = m_credentials.apiRoot;
	if (!root.isValid() || root.isEmpty()) {
		// Sites saved before discovery existed: ?rest_route= works with and without pretty permalinks.
		root = m_credentials.siteUrl;
		root.setPath(root.path() + QLatin1Char('/'));
		root.setQuery(QStringLiteral("rest_route=/"));
	}

	QNetworkRequest req = baseRequest(routeUrl(root, route, extraQuery));
	if (!m_credentials.username.isEmpty()) {
		const QByteArray basic =
			(m_credentials.username + QLatin1Char(':') + m_credentials.applicationPassword).toUtf8();
		req.setRawHeader("Authorization", "Basic " + basic.toBase64());
	}
	return req;
}

void JetpackSiteClient::track(QNetworkReply *reply, std::function<void(QNetworkReply *)> done)
{
	connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
		reply->deleteLater();
		done(reply);
	});
}

void JetpackSiteClient::get(const QString &route, const QUrlQuery &query, std::function<void(QNetworkReply *)> done)
{
	track(m_network->get(request(route, query)), std::move(done));
}

void JetpackSiteClient::post(const QString &route, std::function<void(QNetworkReply *)> done)
{
	QNetworkRequest req = request(route, QUrlQuery());
	req.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));
	track(m_network->post(req, QByteArrayLiteral("{}")), std::move(done));
}

void JetpackSiteClient::probe(ProbeCallback callback)
{
	probeSite(m_credentials.siteUrl, kMaxProbeRedirects, std::move(callback));
}

void JetpackSiteClient::probeSite(const QUrl &siteUrl, int redirectsLeft, ProbeCallback callback)
{
	m_credentials.siteUrl = siteUrl;
	QUrl home = siteUrl;
	home.setPath(siteUrl.path() + QLatin1Char('/'));
	// Caches can hold a homepage from before a hosting change (a site moved to WordPress.com's
	// managed hosting kept advertising public-api.wordpress.com). Ask for a fresh copy.
	home.setQuery(QStringLiteral("obs-videopress-probe=%1").arg(QRandomGenerator::global()->generate()));
	QNetworkRequest homeRequest = baseRequest(home);
	homeRequest.setRawHeader("Cache-Control", "no-cache");

	// The homepage tells us about redirects and the API root WordPress advertises.
	track(m_network->get(homeRequest), [this, siteUrl, redirectsLeft, callback](QNetworkReply *reply) {
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (status >= 300 && status < 400 && redirectsLeft > 0) {
			// e.g. example.wordpress.com -> its custom domain. No credentials are sent while
			// probing, so following is safe; WordPress.com may redirect to http://, so ask
			// for https:// to keep application passwords off plain HTTP.
			QUrl target = reply->url().resolved(
				reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl());
			if (target.scheme() == QLatin1String("http") && !isLocalHost(target.host()))
				target.setScheme(QStringLiteral("https"));
			QString ignored;
			const QUrl next = normalizeSiteUrl(target.toString(), &ignored);
			if (next.isValid() && next != siteUrl) {
				probeSite(next, redirectsLeft - 1, callback);
				return;
			}
		}
		if (status == 0 || (status >= 300 && status < 400)) {
			QJsonObject unused;
			callback({}, replyError(reply, QByteArray(), &unused));
			return;
		}

		const QUrl advertised = apiRootFromLinkHeader(reply->rawHeader("Link"), reply->url());
		const bool advertisesWordPressCom = advertised.host() == QLatin1String(kWordPressComApiHost);

		// The site's own REST API wins over whatever a (possibly cached) page advertises: a
		// site that answers /wp-json/ itself serves its own REST API.
		QUrl prettyRoot = siteUrl;
		prettyRoot.setPath(siteUrl.path() + QStringLiteral("/wp-json/"));
		QUrl queryRoot = siteUrl;
		queryRoot.setPath(siteUrl.path() + QLatin1Char('/'));
		queryRoot.setQuery(QStringLiteral("rest_route=/"));

		QList<QUrl> candidates;
		for (const QUrl &candidate : {prettyRoot, queryRoot, advertisesWordPressCom ? QUrl() : advertised}) {
			if (candidate.isValid() && !candidate.isEmpty() && !candidates.contains(candidate))
				candidates.append(candidate);
		}
		probeCandidates(candidates, advertisesWordPressCom ? advertised : QUrl(), QString(), callback);
	});
}

void JetpackSiteClient::probeCandidates(QList<QUrl> candidates, QUrl wordPressComRoot, QString lastProblem,
					ProbeCallback callback)
{
	if (candidates.isEmpty()) {
		if (wordPressComRoot.isValid()) {
			// No API on the site's own domain, and it points at public-api.wordpress.com:
			// a site hosted on WordPress.com, where application passwords are off.
			SiteProbe result;
			result.siteUrl = m_credentials.siteUrl;
			result.apiRoot = wordPressComRoot;
			result.hostedOnWordPressCom = true;
			callback(result, ApiError());
			return;
		}
		callback({}, ApiError::protocol(lastProblem));
		return;
	}

	const QUrl root = candidates.takeFirst();
	const QUrl siteUrl = m_credentials.siteUrl;
	track(m_network->get(baseRequest(routeUrl(root, QStringLiteral("/"), QUrlQuery()))),
	      [this, root, siteUrl, candidates, wordPressComRoot, callback](QNetworkReply *reply) {
		      QJsonObject index;
		      const ApiError error = replyError(reply, reply->readAll(), &index);
		      if (error.isError() || !index.value(QLatin1String("namespaces")).isArray()) {
			      // If no candidate works, report what the last one returned.
			      const QString problem = error.isError()
							      ? error.message
							      : QStringLiteral("%1 is not a WordPress REST API index.")
									.arg(reply->url().toString());
			      probeCandidates(candidates, wordPressComRoot, problem, callback);
			      return;
		      }
		      if (!index.value(QLatin1String("namespaces")).toArray().contains(QStringLiteral("wpcom/v2"))) {
			      callback({},
				       ApiError::local(QStringLiteral(
					       "This WordPress site doesn't have Jetpack connected, so it can't use "
					       "VideoPress. Install Jetpack and connect it to WordPress.com first.")));
			      return;
		      }

		      SiteProbe result;
		      result.siteUrl = siteUrl;
		      result.apiRoot = root;
		      result.siteName = index.value(QLatin1String("name")).toString();
		      result.applicationPasswordsAdvertised = index.value(QLatin1String("authentication"))
								      .toObject()
								      .contains(QLatin1String("application-passwords"));
		      callback(result, ApiError());
	      });
}

void JetpackSiteClient::verify(VerifyCallback callback)
{
	auto result = std::make_shared<SiteVerification>();

	get(QStringLiteral("/"), QUrlQuery(), [this, result, callback](QNetworkReply *reply) {
		QJsonObject index;
		ApiError error = replyError(reply, reply->readAll(), &index);
		if (error.isError()) {
			callback({}, error);
			return;
		}
		if (!index.value(QLatin1String("namespaces")).toArray().contains(QStringLiteral("wpcom/v2"))) {
			callback({}, ApiError::local(QStringLiteral(
					     "Jetpack's VideoPress features are not active on this site.")));
			return;
		}
		result->siteName = index.value(QLatin1String("name")).toString();

		QUrlQuery context;
		context.addQueryItem(QStringLiteral("context"), QStringLiteral("edit"));
		get(QStringLiteral("/wp/v2/users/me"), context, [this, result, callback](QNetworkReply *meReply) {
			QJsonObject me;
			ApiError meError = replyError(meReply, meReply->readAll(), &me);
			if (meError.isError()) {
				callback({}, meError);
				return;
			}
			result->userDisplayName = me.value(QLatin1String("name")).toString();
			const QJsonObject caps = me.value(QLatin1String("capabilities")).toObject();
			if (!caps.value(QLatin1String("upload_files")).toBool()) {
				ApiError forbidden =
					ApiError::local(QStringLiteral("%1 cannot upload media on this site.")
								.arg(result->userDisplayName));
				forbidden.kind = ErrorKind::Forbidden;
				forbidden.forbiddenReason = ForbiddenReason::NoUploadPermission;
				callback({}, forbidden);
				return;
			}

			mintUploadToken([result, callback](const QString &, qint64 blogId, const ApiError &tokenError) {
				if (tokenError.isError()) {
					callback({}, tokenError);
					return;
				}
				result->blogId = blogId;
				callback(*result, ApiError());
			});
		});
	});
}

void JetpackSiteClient::mintUploadToken(TokenCallback callback)
{
	post(QStringLiteral("/wpcom/v2/videopress/upload-jwt"), [callback](QNetworkReply *reply) {
		QJsonObject json;
		ApiError error = replyError(reply, reply->readAll(), &json);
		if (!error.isError()) {
			const QString token = json.value(QLatin1String("upload_token")).toString();
			const QJsonValue blog = json.value(QLatin1String("upload_blog_id"));
			const qint64 blogId = blog.isString() ? blog.toString().toLongLong() : blog.toInteger();
			if (!token.isEmpty() && blogId > 0) {
				callback(token, blogId, ApiError());
				return;
			}
			error = ApiError::protocol(QStringLiteral("The site returned no VideoPress upload token"));
		}
		// rest_forbidden here usually means the user is not connected to WordPress.com.
		if (error.code == QLatin1String("rest_forbidden") || error.code == QLatin1String("rest_cannot_create"))
			error.message = QStringLiteral(
				"This user can't upload to VideoPress. Connect the user to WordPress.com in Jetpack "
				"(Jetpack → Dashboard → Connect your user account) and make sure they can upload media.");
		callback(QString(), 0, error);
	});
}

UploadTokenProvider JetpackSiteClient::tokenProvider()
{
	QPointer<JetpackSiteClient> self(this);
	return [self](qint64, UploadTokenCallback callback) {
		if (!self) {
			callback(QString(), ApiError::cancelled());
			return;
		}
		self->mintUploadToken(
			[callback](const QString &token, qint64, const ApiError &error) { callback(token, error); });
	};
}

} // namespace videopress

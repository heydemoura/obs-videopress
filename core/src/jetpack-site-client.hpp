// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "api-error.hpp"
#include "tus-upload.hpp"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
class QUrlQuery;

namespace videopress {

// Prototype authentication path that needs no WordPress.com OAuth app: talk to a
// Jetpack-connected site directly with a WordPress Application Password. The site's
// Jetpack VideoPress package mints the upload JWT with its own blog token
// (POST /wpcom/v2/videopress/upload-jwt, the endpoint the block editor's uploader uses);
// the upload itself then goes to public-api.wordpress.com over TUS as usual.
//
// Requirements on the site: Jetpack connected, VideoPress available, and the user behind
// the application password connected to WordPress.com with upload_files.
struct SiteCredentials {
	QUrl siteUrl;
	QString username;
	QString applicationPassword;
	// REST API root found by probe(), e.g. https://example.com/wp-json/ or
	// https://example.com/?rest_route=/. Empty means ?rest_route= on siteUrl.
	QUrl apiRoot;
};

// What the public REST index says about a site, before any credentials are entered.
struct SiteProbe {
	QUrl siteUrl;
	QUrl apiRoot;
	QString siteName;
	// Sites hosted on WordPress.com serve their REST API from public-api.wordpress.com and
	// have application passwords switched off until activated in wp-admin, which moves the
	// site to WordPress.com's managed hosting.
	bool hostedOnWordPressCom = false;
	// WordPress advertises application passwords in the index ("authentication") when they
	// are enabled for the site.
	bool applicationPasswordsAdvertised = false;
};

struct SiteVerification {
	QString siteName;
	QString userDisplayName;
	qint64 blogId = 0;
};

class JetpackSiteClient : public QObject {
	Q_OBJECT

public:
	using ProbeCallback = std::function<void(const SiteProbe &result, const ApiError &error)>;
	using VerifyCallback = std::function<void(const SiteVerification &result, const ApiError &error)>;
	using TokenCallback = std::function<void(const QString &token, qint64 blogId, const ApiError &error)>;

	JetpackSiteClient(QNetworkAccessManager *network, SiteCredentials credentials, QObject *parent = nullptr);

	// https://example.com/blog/ -> https://example.com/blog. Rejects plain http except for
	// localhost, because application passwords travel in every request.
	static QUrl normalizeSiteUrl(const QString &input, QString *error);

	// Where the user creates (and, if needed, enables) application passwords on the site.
	static QUrl applicationPasswordsUrl(const QUrl &siteUrl);

	// The REST API root a page advertises in its Link header (rel="https://api.w.org/").
	static QUrl apiRootFromLinkHeader(const QByteArray &linkHeader, const QUrl &base);
	// URL of `route` under `apiRoot`, for both the pretty (/wp-json/) and ?rest_route= forms.
	static QUrl routeUrl(const QUrl &apiRoot, const QString &route, const QUrlQuery &extraQuery);
	// Parses a JSON object, tolerating a UTF-8 BOM or PHP notices printed before it.
	static bool parseJsonObject(const QByteArray &body, QJsonObject *out);

	void setUserAgent(const QByteArray &userAgent) { m_userAgent = userAgent; }

	// Unauthenticated: finds the REST API root (/wp-json/, then ?rest_route=, then the root
	// the homepage advertises), and checks it is WordPress with Jetpack's wpcom/v2 routes. Follows
	// homepage redirects (as https://); the result's siteUrl is the address that answered.
	// Only siteUrl from the credentials is used.
	void probe(ProbeCallback callback);

	// REST index -> users/me -> one upload JWT: proves the credentials, the Jetpack
	// connection and VideoPress all work, and returns the WordPress.com blog id.
	void verify(VerifyCallback callback);
	void mintUploadToken(TokenCallback callback);

	// Adapter for TusUpload.
	UploadTokenProvider tokenProvider();

private:
	void probeSite(const QUrl &siteUrl, int redirectsLeft, ProbeCallback callback);
	// Tries each candidate API root in order; if none answers and the site advertised
	// public-api.wordpress.com, reports a site hosted on WordPress.com.
	void probeCandidates(QList<QUrl> candidates, QUrl wordPressComRoot, QString lastProblem,
			     ProbeCallback callback);
	QNetworkRequest baseRequest(const QUrl &url) const;
	QNetworkRequest request(const QString &route, const QUrlQuery &extraQuery) const;
	void get(const QString &route, const QUrlQuery &query, std::function<void(QNetworkReply *)> done);
	void post(const QString &route, std::function<void(QNetworkReply *)> done);
	void track(QNetworkReply *reply, std::function<void(QNetworkReply *)> done);

	QNetworkAccessManager *m_network;
	SiteCredentials m_credentials;
	QByteArray m_userAgent;
};

} // namespace videopress

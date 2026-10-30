// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "api-error.hpp"

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace videopress {

// Classic WordPress.com OAuth2, authorization-code grant, for an app registered as
// "native": the code exchange needs no client_secret.
// scope=global gives one token for every site the user can reach, which is what lets the
// plugin switch sites without asking the user to authorize again.
struct OAuthConfig {
	QString clientId;
	QUrl redirectUri;
	QString scope = QStringLiteral("global");
	QUrl authorizeEndpoint = QUrl(QStringLiteral("https://public-api.wordpress.com/oauth2/authorize"));
	QUrl tokenEndpoint = QUrl(QStringLiteral("https://public-api.wordpress.com/oauth2/token"));
};

enum class RedirectStatus {
	NotRedirect,   // some other page; keep waiting
	Success,       // code extracted and state verified
	StateMismatch, // looks like our redirect but the state is wrong: abort the attempt
	Denied,        // the user (or WP.com) returned ?error=...
	Malformed,     // our redirect without a code or error
};

struct RedirectResult {
	RedirectStatus status = RedirectStatus::NotRedirect;
	QString code;
	QString error;
	QString errorDescription;
};

struct TokenResponse {
	QString accessToken;
	QString scope;
	qint64 blogId = 0; // 0 for a global token
};

// 32 bytes from the system CSPRNG, base64url without padding.
QString generateOAuthState();

QUrl buildAuthorizeUrl(const OAuthConfig &config, const QString &state);

// True only for the exact redirect endpoint: scheme, host, port and path must match.
bool isRedirectTo(const QUrl &url, const QUrl &redirectUri);

RedirectResult parseRedirect(const QUrl &url, const QUrl &redirectUri, const QString &expectedState);

// application/x-www-form-urlencoded body for the code exchange (no client_secret).
QByteArray buildTokenRequestBody(const OAuthConfig &config, const QString &code);

// Parses the token endpoint's JSON. Returns false and fills *error on failure.
bool parseTokenResponse(const QByteArray &body, TokenResponse *out, ApiError *error);

bool constantTimeEquals(const QByteArray &a, const QByteArray &b);

} // namespace videopress

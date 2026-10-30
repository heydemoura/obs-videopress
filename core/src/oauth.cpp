// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "oauth.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QUrlQuery>

#include <array>
#include <utility>

namespace videopress {

QString generateOAuthState()
{
	std::array<quint32, 8> words;
	QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
	const QByteArray bytes(reinterpret_cast<const char *>(words.data()),
			       static_cast<qsizetype>(words.size() * sizeof(quint32)));
	return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QUrl buildAuthorizeUrl(const OAuthConfig &config, const QString &state)
{
	QUrlQuery query;
	query.addQueryItem(QStringLiteral("client_id"), config.clientId);
	query.addQueryItem(QStringLiteral("redirect_uri"), config.redirectUri.toString(QUrl::FullyEncoded));
	query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
	query.addQueryItem(QStringLiteral("scope"), config.scope);
	query.addQueryItem(QStringLiteral("state"), state);

	QUrl url = config.authorizeEndpoint;
	url.setQuery(query);
	return url;
}

bool isRedirectTo(const QUrl &url, const QUrl &redirectUri)
{
	if (!url.isValid() || !redirectUri.isValid())
		return false;
	return url.scheme().compare(redirectUri.scheme(), Qt::CaseInsensitive) == 0 &&
	       url.host().compare(redirectUri.host(), Qt::CaseInsensitive) == 0 &&
	       url.port(-1) == redirectUri.port(-1) && url.path() == redirectUri.path();
}

RedirectResult parseRedirect(const QUrl &url, const QUrl &redirectUri, const QString &expectedState)
{
	RedirectResult result;
	if (!isRedirectTo(url, redirectUri))
		return result;

	const QUrlQuery query(url);
	const QString state = query.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded);
	if (expectedState.isEmpty() || !constantTimeEquals(state.toUtf8(), expectedState.toUtf8())) {
		result.status = RedirectStatus::StateMismatch;
		return result;
	}

	result.error = query.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
	if (!result.error.isEmpty()) {
		result.status = RedirectStatus::Denied;
		result.errorDescription = query.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
		return result;
	}

	result.code = query.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
	result.status = result.code.isEmpty() ? RedirectStatus::Malformed : RedirectStatus::Success;
	return result;
}

QByteArray buildTokenRequestBody(const OAuthConfig &config, const QString &code)
{
	const QList<std::pair<const char *, QString>> fields = {
		{"client_id", config.clientId},
		{"redirect_uri", config.redirectUri.toString(QUrl::FullyEncoded)},
		{"grant_type", QStringLiteral("authorization_code")},
		{"code", code},
	};

	// Encode every reserved character: QUrlQuery leaves '+' and '/' alone, and a form
	// decoder reads '+' as a space.
	QByteArray body;
	for (const auto &[name, value] : fields) {
		if (!body.isEmpty())
			body += '&';
		body += name;
		body += '=';
		body += QUrl::toPercentEncoding(value);
	}
	return body;
}

bool parseTokenResponse(const QByteArray &body, TokenResponse *out, ApiError *error)
{
	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (!doc.isObject()) {
		*error = ApiError::protocol(QStringLiteral("Token response is not JSON"));
		return false;
	}

	const QJsonObject obj = doc.object();
	const QString accessToken = obj.value(QLatin1String("access_token")).toString();
	if (accessToken.isEmpty()) {
		*error = ApiError::protocol(QStringLiteral("Token response has no access_token"));
		return false;
	}

	out->accessToken = accessToken;
	out->scope = obj.value(QLatin1String("scope")).toString();
	// WP.com returns blog_id as a string ("0" for global tokens) in some versions.
	const QJsonValue blogId = obj.value(QLatin1String("blog_id"));
	out->blogId = blogId.isString() ? blogId.toString().toLongLong() : blogId.toInteger();
	return true;
}

bool constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
	if (a.size() != b.size())
		return false;
	unsigned char diff = 0;
	for (qsizetype i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i] ^ b[i]);
	return diff == 0;
}

} // namespace videopress

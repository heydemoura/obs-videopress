// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "api-error.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>

namespace videopress {

namespace {

constexpr int kMaxPlainBodyMessage = 300;

// Error codes WP.com uses for a missing, expired or revoked access token.
bool isTokenErrorCode(const QString &code)
{
	return code == QLatin1String("invalid_token") || code == QLatin1String("authorization_required") ||
	       code == QLatin1String("rest_not_logged_in");
}

// A readable sentence for statuses that come back as an HTML error page.
QString messageForStatus(int status)
{
	if (status == 429)
		return QStringLiteral("Too many requests: the site or WordPress.com is rate-limiting this connection. "
				      "Wait a minute and try again.");
	if (status == 401)
		return QStringLiteral("The site refused the credentials (HTTP 401).");
	if (status == 403)
		return QStringLiteral(
			"The site refused access (HTTP 403). A security plugin or firewall may be blocking it.");
	if (status == 404)
		return QStringLiteral("Not found (HTTP 404).");
	if (status >= 500)
		return QStringLiteral("The site had a server error (HTTP %1). Try again later.").arg(status);
	return QStringLiteral("The site answered with HTTP %1.").arg(status);
}

ErrorKind kindForStatus(int status)
{
	if (status == 401)
		return ErrorKind::Unauthorized;
	if (status == 403)
		return ErrorKind::Forbidden;
	if (status == 404)
		return ErrorKind::NotFound;
	if (status == 409)
		return ErrorKind::Conflict;
	if (status == 410)
		return ErrorKind::Gone;
	if (status == 413)
		return ErrorKind::PayloadTooLarge;
	if (status == 429 || status >= 500)
		return ErrorKind::Server;
	if (status >= 400)
		return ErrorKind::Client;
	return ErrorKind::None;
}

} // namespace

bool ApiError::isRetryable() const
{
	switch (kind) {
	case ErrorKind::Network:
	case ErrorKind::Server:
	case ErrorKind::Conflict:
		return true;
	case ErrorKind::Forbidden:
		return forbiddenReason == ForbiddenReason::InvalidUploadToken;
	default:
		return false;
	}
}

ApiError ApiError::fromHttp(int httpStatus, const QByteArray &body)
{
	ApiError error;
	error.httpStatus = httpStatus;
	error.kind = kindForStatus(httpStatus);
	if (error.kind == ErrorKind::None)
		return error;

	const QJsonDocument doc = QJsonDocument::fromJson(body);
	if (doc.isObject()) {
		QJsonObject obj = doc.object();
		// wp_send_json_error() wraps the payload: {"success":false,"data":{"error","message"}}.
		if (obj.value(QLatin1String("data")).isObject() && obj.contains(QLatin1String("success")))
			obj = obj.value(QLatin1String("data")).toObject();
		// REST v1.1 uses "error", REST v2 (WP_REST_Response) uses "code".
		error.code = obj.value(QLatin1String("error")).toString();
		if (error.code.isEmpty())
			error.code = obj.value(QLatin1String("code")).toString();
		error.message = obj.value(QLatin1String("message")).toString();
	} else {
		const QString text = QString::fromUtf8(body).trimmed();
		// Edge and server error pages are HTML; never show markup to the user.
		error.message = text.isEmpty() || text.startsWith(QLatin1Char('<')) ? messageForStatus(httpStatus)
										    : text.left(kMaxPlainBodyMessage);
	}

	if (isTokenErrorCode(error.code) && (httpStatus == 400 || httpStatus == 403))
		error.kind = ErrorKind::Unauthorized;

	if (error.kind == ErrorKind::Forbidden)
		error.forbiddenReason = classifyForbidden(error.message);

	return error;
}

ApiError ApiError::fromReply(QNetworkReply *reply, const QByteArray &body)
{
	if (reply->error() == QNetworkReply::OperationCanceledError)
		return cancelled();

	const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
	if (status == 0) {
		ApiError error;
		error.kind = reply->error() == QNetworkReply::NoError ? ErrorKind::Protocol : ErrorKind::Network;
		error.message = reply->errorString();
		return error;
	}
	return fromHttp(status, body);
}

ApiError ApiError::local(const QString &message)
{
	ApiError error;
	error.kind = ErrorKind::Local;
	error.message = message;
	return error;
}

ApiError ApiError::protocol(const QString &message)
{
	ApiError error;
	error.kind = ErrorKind::Protocol;
	error.message = message;
	return error;
}

ApiError ApiError::cancelled()
{
	ApiError error;
	error.kind = ErrorKind::Cancelled;
	error.message = QStringLiteral("Cancelled");
	return error;
}

ForbiddenReason classifyForbidden(const QString &message)
{
	const auto has = [&message](const char *needle) {
		return message.contains(QLatin1String(needle), Qt::CaseInsensitive);
	};

	if (has("blocked for this site"))
		return ForbiddenReason::SiteBlocked;
	if (has("Jetpack Video is disabled"))
		return ForbiddenReason::VideoPressDisabled;
	if (has("trial has expired"))
		return ForbiddenReason::TrialExpired;
	if (has("Invalid upload permission") || has("Missing upload token"))
		return ForbiddenReason::InvalidUploadToken;
	if (has("cannot upload media") || has("not authorized"))
		return ForbiddenReason::NoUploadPermission;
	if (has("space"))
		return ForbiddenReason::QuotaExceeded;
	if (has("Invalid Mime"))
		return ForbiddenReason::InvalidMime;
	return ForbiddenReason::Unknown;
}

} // namespace videopress

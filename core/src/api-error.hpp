// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>

class QNetworkReply;

namespace videopress {

enum class ErrorKind {
	None,
	Network,
	Cancelled,
	Unauthorized, // the WP.com access token is invalid; the user has to reconnect
	Forbidden,    // the server refused the action; see ForbiddenReason
	NotFound,
	Gone,
	Conflict,
	PayloadTooLarge,
	Server,
	Client,
	Protocol, // the server answered, but not in the shape we expect
	Local,    // file system or other client-side failure
};

// Reasons the VideoPress upload service gives for a 403, recognised by their messages.
enum class ForbiddenReason {
	Unknown,
	SiteBlocked,
	VideoPressDisabled,
	TrialExpired,
	NoUploadPermission,
	QuotaExceeded,
	InvalidMime,
	InvalidUploadToken, // recoverable: mint a new upload JWT and resync
};

struct ApiError {
	ErrorKind kind = ErrorKind::None;
	int httpStatus = 0;
	QString code;
	QString message;
	ForbiddenReason forbiddenReason = ForbiddenReason::Unknown;

	bool isError() const { return kind != ErrorKind::None; }
	bool needsReconnect() const { return kind == ErrorKind::Unauthorized; }
	bool isRetryable() const;

	static ApiError fromHttp(int httpStatus, const QByteArray &body);
	static ApiError fromReply(QNetworkReply *reply, const QByteArray &body);
	static ApiError local(const QString &message);
	static ApiError protocol(const QString &message);
	static ApiError cancelled();
};

ForbiddenReason classifyForbidden(const QString &message);

} // namespace videopress

Q_DECLARE_METATYPE(videopress::ApiError)

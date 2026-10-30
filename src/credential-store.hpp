// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QUrl>

// Secrets (application passwords today, OAuth tokens later) never go into settings files.
// macOS: login keychain. Elsewhere, until QtKeychain lands: kept in memory for
// the session only.
namespace credential_store {

// One keychain item per site + user.
inline QString accountFor(const QUrl &siteUrl, const QString &username)
{
	return siteUrl.toString() + QLatin1Char(' ') + username;
}

bool isPersistent();
bool save(const QString &account, const QString &secret, QString *error);
QString load(const QString &account);
void remove(const QString &account);

} // namespace credential_store

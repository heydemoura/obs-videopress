// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QList>
#include <QString>
#include <QUrl>

namespace videopress {

enum class AfterRecording { Ask, Upload, Nothing };

// Settings that follow the OBS profile, so a creator can bind "Gaming" to one site and
// "Podcast" to another.
struct ProfileSettings {
	qint64 activeBlogId = 0;
	AfterRecording afterRecording = AfterRecording::Ask;
	QString titleTemplate = QStringLiteral("{profile} – {date} {time}");
	bool pauseWhileStreaming = true;

	bool operator==(const ProfileSettings &other) const;
};

// A site connected with an application password (prototype auth path). The password
// itself lives in the OS keychain, keyed by siteUrl + username.
struct SiteEntry {
	qint64 blogId = 0;
	QUrl siteUrl;
	QUrl apiRoot; // discovered REST API root; empty for sites saved before discovery
	QString username;
	QString name;

	bool operator==(const SiteEntry &other) const;
};

// JSON file in the plugin's config directory. Tokens never go here: they live in the OS
// keychain.
class SettingsStore {
public:
	static constexpr int kSchemaVersion = 1;
	static constexpr qsizetype kMaxRecentBlogs = 10;

	explicit SettingsStore(QString filePath);

	// A missing file is not an error: the store starts with defaults.
	bool load(QString *error = nullptr);
	// Atomic (QSaveFile): a crash mid-write never leaves a truncated file behind.
	bool save(QString *error = nullptr) const;

	ProfileSettings profile(const QString &name) const;
	void setProfile(const QString &name, const ProfileSettings &settings);
	void renameProfile(const QString &from, const QString &to);

	QList<SiteEntry> sites() const { return m_sites; }
	const SiteEntry *site(qint64 blogId) const;
	// Inserts, or replaces the entry with the same blog id.
	void upsertSite(const SiteEntry &site);
	void removeSite(qint64 blogId);

	QList<qint64> recentBlogIds() const { return m_recentBlogIds; }
	void touchRecentBlog(qint64 blogId);

private:
	QString m_filePath;
	QHash<QString, ProfileSettings> m_profiles;
	QList<SiteEntry> m_sites;
	QList<qint64> m_recentBlogIds;
};

} // namespace videopress

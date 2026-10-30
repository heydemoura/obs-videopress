// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings-store.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <utility>

namespace videopress {

namespace {

QString afterRecordingToString(AfterRecording value)
{
	switch (value) {
	case AfterRecording::Upload:
		return QStringLiteral("upload");
	case AfterRecording::Nothing:
		return QStringLiteral("nothing");
	case AfterRecording::Ask:
		break;
	}
	return QStringLiteral("ask");
}

AfterRecording afterRecordingFromString(const QString &value)
{
	if (value == QLatin1String("upload"))
		return AfterRecording::Upload;
	if (value == QLatin1String("nothing"))
		return AfterRecording::Nothing;
	return AfterRecording::Ask;
}

QJsonObject profileToJson(const ProfileSettings &settings)
{
	QJsonObject obj;
	obj.insert(QLatin1String("active_blog_id"), settings.activeBlogId);
	obj.insert(QLatin1String("after_recording"), afterRecordingToString(settings.afterRecording));
	obj.insert(QLatin1String("title_template"), settings.titleTemplate);
	obj.insert(QLatin1String("pause_while_streaming"), settings.pauseWhileStreaming);
	return obj;
}

ProfileSettings profileFromJson(const QJsonObject &obj)
{
	ProfileSettings settings;
	settings.activeBlogId = obj.value(QLatin1String("active_blog_id")).toInteger();
	settings.afterRecording = afterRecordingFromString(obj.value(QLatin1String("after_recording")).toString());
	if (obj.contains(QLatin1String("title_template")))
		settings.titleTemplate = obj.value(QLatin1String("title_template")).toString();
	settings.pauseWhileStreaming = obj.value(QLatin1String("pause_while_streaming")).toBool(true);
	return settings;
}

QJsonObject siteToJson(const SiteEntry &site)
{
	QJsonObject obj;
	obj.insert(QLatin1String("blog_id"), site.blogId);
	obj.insert(QLatin1String("site_url"), site.siteUrl.toString());
	if (site.apiRoot.isValid())
		obj.insert(QLatin1String("api_root"), site.apiRoot.toString());
	obj.insert(QLatin1String("username"), site.username);
	obj.insert(QLatin1String("name"), site.name);
	return obj;
}

SiteEntry siteFromJson(const QJsonObject &obj)
{
	SiteEntry site;
	site.blogId = obj.value(QLatin1String("blog_id")).toInteger();
	site.siteUrl = QUrl(obj.value(QLatin1String("site_url")).toString());
	site.apiRoot = QUrl(obj.value(QLatin1String("api_root")).toString());
	site.username = obj.value(QLatin1String("username")).toString();
	site.name = obj.value(QLatin1String("name")).toString();
	return site;
}

} // namespace

bool SiteEntry::operator==(const SiteEntry &other) const
{
	return blogId == other.blogId && siteUrl == other.siteUrl && apiRoot == other.apiRoot &&
	       username == other.username && name == other.name;
}

bool ProfileSettings::operator==(const ProfileSettings &other) const
{
	return activeBlogId == other.activeBlogId && afterRecording == other.afterRecording &&
	       titleTemplate == other.titleTemplate && pauseWhileStreaming == other.pauseWhileStreaming;
}

SettingsStore::SettingsStore(QString filePath) : m_filePath(std::move(filePath)) {}

bool SettingsStore::load(QString *error)
{
	m_profiles.clear();
	m_sites.clear();
	m_recentBlogIds.clear();

	QFile file(m_filePath);
	if (!file.exists())
		return true;
	if (!file.open(QIODevice::ReadOnly)) {
		if (error)
			*error = file.errorString();
		return false;
	}

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
	if (!doc.isObject()) {
		if (error)
			*error = parseError.errorString();
		return false;
	}

	const QJsonObject root = doc.object();
	const QJsonObject profiles = root.value(QLatin1String("profiles")).toObject();
	for (auto it = profiles.constBegin(); it != profiles.constEnd(); ++it)
		m_profiles.insert(it.key(), profileFromJson(it.value().toObject()));

	for (const QJsonValue value : root.value(QLatin1String("sites")).toArray()) {
		const SiteEntry site = siteFromJson(value.toObject());
		if (site.blogId > 0 && site.siteUrl.isValid())
			upsertSite(site);
	}

	const QJsonArray recent = root.value(QLatin1String("recent_blog_ids")).toArray();
	for (const QJsonValue value : recent) {
		const qint64 blogId = value.toInteger();
		if (blogId > 0 && !m_recentBlogIds.contains(blogId) && m_recentBlogIds.size() < kMaxRecentBlogs)
			m_recentBlogIds.append(blogId);
	}
	return true;
}

bool SettingsStore::save(QString *error) const
{
	QJsonObject profiles;
	for (auto it = m_profiles.constBegin(); it != m_profiles.constEnd(); ++it)
		profiles.insert(it.key(), profileToJson(it.value()));

	QJsonArray sites;
	for (const SiteEntry &site : m_sites)
		sites.append(siteToJson(site));

	QJsonArray recent;
	for (qint64 blogId : m_recentBlogIds)
		recent.append(blogId);

	QJsonObject root;
	root.insert(QLatin1String("version"), kSchemaVersion);
	root.insert(QLatin1String("profiles"), profiles);
	root.insert(QLatin1String("sites"), sites);
	root.insert(QLatin1String("recent_blog_ids"), recent);

	QDir().mkpath(QFileInfo(m_filePath).absolutePath());
	QSaveFile file(m_filePath);
	if (!file.open(QIODevice::WriteOnly)) {
		if (error)
			*error = file.errorString();
		return false;
	}
	file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
	if (!file.commit()) {
		if (error)
			*error = file.errorString();
		return false;
	}
	return true;
}

ProfileSettings SettingsStore::profile(const QString &name) const
{
	return m_profiles.value(name);
}

void SettingsStore::setProfile(const QString &name, const ProfileSettings &settings)
{
	m_profiles.insert(name, settings);
}

void SettingsStore::renameProfile(const QString &from, const QString &to)
{
	if (from == to || !m_profiles.contains(from))
		return;
	m_profiles.insert(to, m_profiles.take(from));
}

const SiteEntry *SettingsStore::site(qint64 blogId) const
{
	for (const SiteEntry &entry : m_sites) {
		if (entry.blogId == blogId)
			return &entry;
	}
	return nullptr;
}

void SettingsStore::upsertSite(const SiteEntry &site)
{
	for (SiteEntry &entry : m_sites) {
		if (entry.blogId == site.blogId) {
			entry = site;
			return;
		}
	}
	m_sites.append(site);
}

void SettingsStore::removeSite(qint64 blogId)
{
	m_sites.removeIf([blogId](const SiteEntry &entry) { return entry.blogId == blogId; });
	m_recentBlogIds.removeAll(blogId);
}

void SettingsStore::touchRecentBlog(qint64 blogId)
{
	if (blogId <= 0)
		return;
	m_recentBlogIds.removeAll(blogId);
	m_recentBlogIds.prepend(blogId);
	while (m_recentBlogIds.size() > kMaxRecentBlogs)
		m_recentBlogIds.removeLast();
}

} // namespace videopress

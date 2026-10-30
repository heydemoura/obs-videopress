// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "settings-store.hpp"
#include "title-template.hpp"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using namespace videopress;

class TestSettingsStore : public QObject {
	Q_OBJECT

private:
	QTemporaryDir m_dir;

	QString path() const { return m_dir.filePath(QStringLiteral("nested/obs-videopress.json")); }

private slots:
	void missingFileGivesDefaults()
	{
		SettingsStore store(m_dir.filePath(QStringLiteral("absent.json")));
		QVERIFY(store.load());
		const ProfileSettings defaults = store.profile(QStringLiteral("Untitled"));
		QCOMPARE(defaults.activeBlogId, qint64(0));
		QCOMPARE(defaults.afterRecording, AfterRecording::Ask);
		QVERIFY(defaults.pauseWhileStreaming);
		QVERIFY(store.recentBlogIds().isEmpty());
	}

	void roundTrips()
	{
		ProfileSettings gaming;
		gaming.activeBlogId = 111;
		gaming.afterRecording = AfterRecording::Upload;
		gaming.titleTemplate = QStringLiteral("{scene} {date}");
		gaming.pauseWhileStreaming = false;

		SettingsStore writer(path());
		writer.setProfile(QStringLiteral("Gaming"), gaming);
		writer.touchRecentBlog(111);
		writer.touchRecentBlog(222);
		QString error;
		QVERIFY2(writer.save(&error), qPrintable(error));

		SettingsStore reader(path());
		QVERIFY(reader.load());
		QVERIFY(reader.profile(QStringLiteral("Gaming")) == gaming);
		QCOMPARE(reader.recentBlogIds(), (QList<qint64>{222, 111}));
	}

	void storesSitesWithoutSecrets()
	{
		SiteEntry site;
		site.blogId = 555;
		site.siteUrl = QUrl(QStringLiteral("https://example.com"));
		site.username = QStringLiteral("admin");
		site.name = QStringLiteral("Example");

		SettingsStore writer(path());
		writer.upsertSite(site);
		site.name = QStringLiteral("Renamed");
		writer.upsertSite(site);
		QVERIFY(writer.save());

		QFile file(path());
		QVERIFY(file.open(QIODevice::ReadOnly));
		QVERIFY(!file.readAll().contains("password"));

		SettingsStore reader(path());
		QVERIFY(reader.load());
		QCOMPARE(reader.sites().size(), 1);
		QVERIFY(reader.site(555) && *reader.site(555) == site);

		reader.touchRecentBlog(555);
		reader.removeSite(555);
		QVERIFY(!reader.site(555));
		QVERIFY(reader.recentBlogIds().isEmpty());
	}

	void renamesProfiles()
	{
		SettingsStore store(path());
		ProfileSettings settings;
		settings.activeBlogId = 7;
		store.setProfile(QStringLiteral("Old"), settings);
		store.renameProfile(QStringLiteral("Old"), QStringLiteral("New"));
		QCOMPARE(store.profile(QStringLiteral("New")).activeBlogId, qint64(7));
		QCOMPARE(store.profile(QStringLiteral("Old")).activeBlogId, qint64(0));
	}

	void recentBlogsAreCappedAndDeduplicated()
	{
		SettingsStore store(path());
		for (qint64 id = 1; id <= 15; ++id)
			store.touchRecentBlog(id);
		store.touchRecentBlog(10);
		store.touchRecentBlog(0);
		const QList<qint64> recent = store.recentBlogIds();
		QCOMPARE(recent.size(), SettingsStore::kMaxRecentBlogs);
		QCOMPARE(recent.first(), qint64(10));
		QCOMPARE(recent.count(10), 1);
		QVERIFY(!recent.contains(1));
	}

	void corruptFileIsReported()
	{
		const QString file = m_dir.filePath(QStringLiteral("corrupt.json"));
		QFile f(file);
		QVERIFY(f.open(QIODevice::WriteOnly));
		f.write("{not json");
		f.close();

		SettingsStore store(file);
		QString error;
		QVERIFY(!store.load(&error));
		QVERIFY(!error.isEmpty());
	}

	void rendersTitles_data()
	{
		QTest::addColumn<QString>("templ");
		QTest::addColumn<QString>("scene");
		QTest::addColumn<QString>("expected");
		QTest::newRow("default") << QStringLiteral("{profile} – {date} {time}") << QString()
					 << QStringLiteral("Podcast – 2026-10-30 14:02");
		QTest::newRow("empty scene")
			<< QStringLiteral("{scene} – {date}") << QString() << QStringLiteral("2026-10-30");
		QTest::newRow("scene")
			<< QStringLiteral("{scene}: take") << QStringLiteral("Intro") << QStringLiteral("Intro: take");
		QTest::newRow("unknown token")
			<< QStringLiteral("{episode} {date}") << QString() << QStringLiteral("{episode} 2026-10-30");
		QTest::newRow("fallback")
			<< QStringLiteral("{scene}") << QString() << QStringLiteral("2026-10-30 14-02-11");
	}

	void rendersTitles()
	{
		QFETCH(QString, templ);
		QFETCH(QString, scene);
		QFETCH(QString, expected);

		TitleContext context;
		context.profile = QStringLiteral("Podcast");
		context.scene = scene;
		context.recordedAt = QDateTime(QDate(2026, 10, 30), QTime(14, 2, 11));
		context.fileBaseName = QStringLiteral("2026-10-30 14-02-11");
		QCOMPARE(renderTitle(templ, context), expected);
	}
};

QTEST_GUILESS_MAIN(TestSettingsStore)
#include "test-settings-store.moc"

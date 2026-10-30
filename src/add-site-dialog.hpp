// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <jetpack-site-client.hpp>
#include <settings-store.hpp>

#include <QDialog>
#include <QNetworkAccessManager>
#include <QPointer>

class QLabel;
class QLineEdit;
class QPushButton;

// Prototype connection flow, in two steps:
//   1. the site address, checked against the site's public REST index (WordPress + Jetpack);
//   2. how to create an application password on that site, then username + password,
//      verified against the site's Jetpack VideoPress endpoints before anything is saved.
class AddSiteDialog : public QDialog {
	Q_OBJECT

public:
	// A non-empty address skips straight to checking it.
	explicit AddSiteDialog(QWidget *parent = nullptr, const QString &address = QString());
	~AddSiteDialog() override;

	// Valid after exec() returns Accepted. The password is already in the keychain.
	videopress::SiteEntry site() const { return m_site; }

private:
	void next();
	void back();
	void checkAddress();
	void showCredentialsStep(const videopress::SiteProbe &probe);
	void showPage(QWidget *page);
	bool onAddressStep() const;
	void connectSite();
	void verifyCredentials(const QString &username, const QString &password);
	void setBusy(bool busy, const QString &message = QString());
	void showError(QLabel *label, const QString &message);
	videopress::JetpackSiteClient *newClient(const videopress::SiteCredentials &credentials);

	QNetworkAccessManager m_network;
	QPointer<videopress::JetpackSiteClient> m_client; // in-flight request
	videopress::SiteProbe m_probe;
	QUrl m_passwordsUrl; // <site>/wp-admin/users.php?page=application-passwords

	// Two pages in one layout, shown one at a time. (QStackedWidget does not propagate the
	// height-for-width of word-wrapped labels, which clipped the instructions.)
	QWidget *m_addressPage;
	QWidget *m_credentialsPage;
	QLineEdit *m_url;
	QLabel *m_addressStatus;
	QLabel *m_heading;
	QLabel *m_intro;
	QPushButton *m_openPasswords;
	QLabel *m_passwordsLink;
	QLabel *m_instructions;
	QLineEdit *m_username;
	QLineEdit *m_password;
	QLabel *m_credentialsStatus;
	QPushButton *m_back;
	QPushButton *m_next;
	videopress::SiteEntry m_site;
};

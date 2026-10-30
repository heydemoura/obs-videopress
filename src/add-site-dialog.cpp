// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "add-site-dialog.hpp"

#include "credential-store.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

using videopress::ApiError;
using videopress::JetpackSiteClient;

namespace {

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QLabel *wrappingLabel(QWidget *parent)
{
	auto *label = new QLabel(parent);
	label->setWordWrap(true);
	label->setTextFormat(Qt::RichText);
	label->setOpenExternalLinks(true);
	return label;
}

// Failures go to the OBS log so they can be diagnosed later. Never log credentials.
void logFailure(const QUrl &siteUrl, const QString &step, const QString &message)
{
	obs_log(LOG_WARNING, "add site %s (%s) failed: %s", siteUrl.host().toUtf8().constData(),
		step.toUtf8().constData(), message.toUtf8().constData());
}

QString describe(const ApiError &error)
{
	if (!error.message.isEmpty())
		return error.message;
	return error.httpStatus ? QStringLiteral("HTTP %1").arg(error.httpStatus) : QStringLiteral("Unknown error");
}

} // namespace

AddSiteDialog::AddSiteDialog(QWidget *parent, const QString &address)
	: QDialog(parent),
	  m_addressPage(new QWidget(this)),
	  m_credentialsPage(new QWidget(this)),
	  m_url(new QLineEdit(this)),
	  m_addressStatus(wrappingLabel(this)),
	  m_heading(wrappingLabel(this)),
	  m_intro(wrappingLabel(this)),
	  m_openPasswords(new QPushButton(this)),
	  m_passwordsLink(wrappingLabel(this)),
	  m_instructions(wrappingLabel(this)),
	  m_username(new QLineEdit(this)),
	  m_password(new QLineEdit(this)),
	  m_credentialsStatus(wrappingLabel(this)),
	  m_back(new QPushButton(text("VideoPress.AddSite.Back"), this)),
	  m_next(new QPushButton(this))
{
	setWindowTitle(text("VideoPress.AddSite.Title"));
	setMinimumWidth(480);

	// Step 1: site address.
	QWidget *addressPage = m_addressPage;
	auto *addressIntro = wrappingLabel(addressPage);
	addressIntro->setText(text("VideoPress.AddSite.AddressIntro"));
	m_url->setPlaceholderText(QStringLiteral("https://example.com"));
	auto *addressForm = new QFormLayout();
	addressForm->addRow(text("VideoPress.AddSite.Url"), m_url);
	auto *addressLayout = new QVBoxLayout(addressPage);
	addressLayout->setContentsMargins(0, 0, 0, 0);
	addressLayout->addWidget(addressIntro);
	addressLayout->addLayout(addressForm);
	addressLayout->addWidget(m_addressStatus);

	// Step 2: application password.
	QWidget *credentialsPage = m_credentialsPage;
	m_password->setEchoMode(QLineEdit::Password);
	m_password->setPlaceholderText(QStringLiteral("xxxx xxxx xxxx xxxx xxxx xxxx"));
	auto *credentialsForm = new QFormLayout();
	credentialsForm->addRow(text("VideoPress.AddSite.Username"), m_username);
	credentialsForm->addRow(text("VideoPress.AddSite.Password"), m_password);
	auto *credentialsLayout = new QVBoxLayout(credentialsPage);
	credentialsLayout->setContentsMargins(0, 0, 0, 0);
	credentialsLayout->addWidget(m_heading);
	credentialsLayout->addWidget(m_intro);
	// The way to get an application password: open the site's page for it in the browser.
	m_openPasswords->setAutoDefault(false);
	m_passwordsLink->setTextInteractionFlags(Qt::TextBrowserInteraction);
	auto *openRow = new QHBoxLayout();
	openRow->addWidget(m_openPasswords);
	openRow->addStretch(1);
	credentialsLayout->addLayout(openRow);
	credentialsLayout->addWidget(m_passwordsLink);
	credentialsLayout->addWidget(m_instructions);
	credentialsLayout->addLayout(credentialsForm);
	credentialsLayout->addWidget(m_credentialsStatus);

	auto *buttons = new QDialogButtonBox(this);
	buttons->addButton(m_back, QDialogButtonBox::ActionRole);
	buttons->addButton(m_next, QDialogButtonBox::ActionRole);
	buttons->addButton(QDialogButtonBox::Cancel);
	m_next->setDefault(true);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(m_addressPage);
	layout->addWidget(m_credentialsPage);
	layout->addWidget(buttons);

	connect(m_next, &QPushButton::clicked, this, &AddSiteDialog::next);
	connect(m_back, &QPushButton::clicked, this, &AddSiteDialog::back);
	connect(m_openPasswords, &QPushButton::clicked, this, [this] {
		if (m_passwordsUrl.isValid())
			QDesktopServices::openUrl(m_passwordsUrl);
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	back(); // start on the address step
	if (!address.trimmed().isEmpty()) {
		m_url->setText(address.trimmed());
		QTimer::singleShot(0, this, &AddSiteDialog::checkAddress);
	}
}

AddSiteDialog::~AddSiteDialog()
{
	// Before m_network goes away: aborting its replies must not call back into this dialog.
	delete m_client;
}

JetpackSiteClient *AddSiteDialog::newClient(const videopress::SiteCredentials &credentials)
{
	delete m_client;
	auto *client = new JetpackSiteClient(&m_network, credentials, this);
	client->setUserAgent(QByteArrayLiteral("obs-videopress/") + PLUGIN_VERSION);
	m_client = client;
	return client;
}

bool AddSiteDialog::onAddressStep() const
{
	return !m_addressPage->isHidden();
}

void AddSiteDialog::showPage(QWidget *page)
{
	m_addressPage->setVisible(page == m_addressPage);
	m_credentialsPage->setVisible(page == m_credentialsPage);
	// Re-fit to the new page's wrapped text, at the width the dialog actually has.
	layout()->activate();
	adjustSize();
	if (layout()->hasHeightForWidth())
		resize(width(), layout()->totalHeightForWidth(width()));
}

void AddSiteDialog::next()
{
	if (onAddressStep())
		checkAddress();
	else
		connectSite();
}

void AddSiteDialog::back()
{
	delete m_client;
	setBusy(false);
	showPage(m_addressPage);
	m_back->setVisible(false);
	m_next->setText(text("VideoPress.AddSite.Continue"));
	m_credentialsStatus->clear();
	m_url->setFocus();
}

void AddSiteDialog::setBusy(bool busy, const QString &message)
{
	m_next->setEnabled(!busy);
	m_back->setEnabled(!busy);
	m_url->setEnabled(!busy);
	m_username->setEnabled(!busy);
	m_password->setEnabled(!busy);
	if (busy) {
		QLabel *status = onAddressStep() ? m_addressStatus : m_credentialsStatus;
		status->setStyleSheet(QString());
		status->setText(message);
	}
}

void AddSiteDialog::showError(QLabel *label, const QString &message)
{
	setBusy(false);
	label->setStyleSheet(QStringLiteral("color: #e5484d;"));
	label->setText(message.toHtmlEscaped());
}

void AddSiteDialog::checkAddress()
{
	QString error;
	const QUrl siteUrl = JetpackSiteClient::normalizeSiteUrl(m_url->text(), &error);
	if (!siteUrl.isValid()) {
		showError(m_addressStatus, error);
		return;
	}
	m_url->setText(siteUrl.toString());

	setBusy(true, text("VideoPress.AddSite.CheckingAddress"));
	JetpackSiteClient *client = newClient({siteUrl, QString(), QString(), QUrl()});
	QPointer<AddSiteDialog> self(this);
	client->probe([self, client](const videopress::SiteProbe &probe, const ApiError &probeError) {
		client->deleteLater();
		if (!self)
			return;
		if (probeError.isError()) {
			logFailure(QUrl(self->m_url->text()), QStringLiteral("address check"), describe(probeError));
			self->showError(self->m_addressStatus, describe(probeError));
			return;
		}
		self->showCredentialsStep(probe);
	});
}

void AddSiteDialog::showCredentialsStep(const videopress::SiteProbe &probe)
{
	m_probe = probe;
	setBusy(false);
	m_addressStatus->clear();
	m_url->setText(probe.siteUrl.toString()); // the address that answered, after redirects
	if (probe.hostedOnWordPressCom)
		obs_log(LOG_INFO, "add site %s: WordPress.com-hosted, application passwords not available",
			probe.siteUrl.host().toUtf8().constData());

	const QString name = (probe.siteName.isEmpty() ? probe.siteUrl.host() : probe.siteName).toHtmlEscaped();
	m_passwordsUrl = JetpackSiteClient::applicationPasswordsUrl(probe.siteUrl);
	const QString passwordsUrl = m_passwordsUrl.toString(QUrl::FullyEncoded).toHtmlEscaped();

	m_heading->setText(text("VideoPress.AddSite.CredentialsHeading").arg(name));
	// WordPress.com-hosted sites have application passwords off until they are activated.
	const bool onWordPressCom = probe.hostedOnWordPressCom;
	m_intro->setText(
		text(onWordPressCom ? "VideoPress.AddSite.IntroWordPressCom" : "VideoPress.AddSite.Intro").arg(name));
	m_openPasswords->setText(text("VideoPress.AddSite.OpenPasswords").arg(probe.siteUrl.host()));
	m_openPasswords->setToolTip(m_passwordsUrl.toString());
	m_passwordsLink->setText(QStringLiteral("<a href=\"%1\">%1</a>").arg(passwordsUrl));
	QString instructions =
		text(onWordPressCom ? "VideoPress.AddSite.StepsWordPressCom" : "VideoPress.AddSite.Steps");
	if (!onWordPressCom && !probe.applicationPasswordsAdvertised)
		instructions += text("VideoPress.AddSite.PasswordsMaybeOff");
	m_instructions->setText(instructions);

	showPage(m_credentialsPage);
	m_back->setVisible(true);
	m_next->setText(text("VideoPress.AddSite.Connect"));
	m_credentialsStatus->clear();
	m_username->setFocus();
}

void AddSiteDialog::connectSite()
{
	const QString username = m_username->text().trimmed();
	const QString password = m_password->text().trimmed();
	if (username.isEmpty() || password.isEmpty()) {
		showError(m_credentialsStatus, text("VideoPress.AddSite.Missing"));
		return;
	}
	if (!m_probe.hostedOnWordPressCom) {
		verifyCredentials(username, password);
		return;
	}

	// Activating application passwords moves a WordPress.com site to managed hosting, after
	// which it serves its own REST API. Look again before trying the credentials.
	setBusy(true, text("VideoPress.AddSite.Checking"));
	JetpackSiteClient *client = newClient({m_probe.siteUrl, QString(), QString(), QUrl()});
	QPointer<AddSiteDialog> self(this);
	client->probe([self, client, username, password](const videopress::SiteProbe &probe, const ApiError &error) {
		client->deleteLater();
		if (!self)
			return;
		if (error.isError()) {
			logFailure(self->m_probe.siteUrl, QStringLiteral("re-check"), describe(error));
			self->showError(self->m_credentialsStatus, describe(error));
			return;
		}
		if (probe.hostedOnWordPressCom) {
			const QString message = text("VideoPress.AddSite.NotActivatedYet").arg(probe.siteUrl.host());
			logFailure(probe.siteUrl, QStringLiteral("connect"), message);
			self->showError(self->m_credentialsStatus, message);
			return;
		}
		self->m_probe = probe;
		self->verifyCredentials(username, password);
	});
}

void AddSiteDialog::verifyCredentials(const QString &username, const QString &password)
{
	const QUrl siteUrl = m_probe.siteUrl;
	const QUrl apiRoot = m_probe.apiRoot;

	setBusy(true, text("VideoPress.AddSite.Checking"));
	JetpackSiteClient *client = newClient({siteUrl, username, password, apiRoot});
	QPointer<AddSiteDialog> self(this);
	client->verify([self, client, siteUrl, apiRoot, username, password](const videopress::SiteVerification &result,
									    const ApiError &verifyError) {
		client->deleteLater();
		if (!self)
			return;
		if (verifyError.isError()) {
			const QString message = verifyError.kind == videopress::ErrorKind::Unauthorized
							? text("VideoPress.AddSite.BadPassword")
							: describe(verifyError);
			logFailure(siteUrl, QStringLiteral("connect"),
				   QStringLiteral("HTTP %1 %2: %3")
					   .arg(verifyError.httpStatus)
					   .arg(verifyError.code, message));
			self->showError(self->m_credentialsStatus, message);
			return;
		}

		QString keychainError;
		if (!credential_store::save(credential_store::accountFor(siteUrl, username), password,
					    &keychainError)) {
			self->showError(self->m_credentialsStatus,
					text("VideoPress.AddSite.KeychainFailed").arg(keychainError));
			return;
		}

		self->m_site.blogId = result.blogId;
		self->m_site.siteUrl = siteUrl;
		self->m_site.apiRoot = apiRoot;
		self->m_site.username = username;
		self->m_site.name = result.siteName.isEmpty() ? siteUrl.host() : result.siteName;
		obs_log(LOG_INFO, "connected site %s (blog %lld, API %s)", siteUrl.host().toUtf8().constData(),
			static_cast<long long>(result.blogId), apiRoot.toString().toUtf8().constData());
		self->accept();
	});
}

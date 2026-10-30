// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "videopress-dock.hpp"

#include "add-site-dialog.hpp"
#include "credential-store.hpp"
#include "dock-widgets.hpp"
#include "upload-controller.hpp"

#include <jetpack-site-client.hpp>
#include <settings-store.hpp>
#include <title-template.hpp>

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkReply>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyle>
#include <QVBoxLayout>

#include <utility>

using videopress::ProfileSettings;
using videopress::SiteEntry;

namespace {

// Design spacing scale (density 0.85): --space-2 6.8, -3 10.2, -4 13.6, -6 20.4.
constexpr int kSpace2 = 7;
constexpr int kSpace3 = 10;
constexpr int kSpace4 = 14;
constexpr int kSpace6 = 20;

const QStringList kVideoSuffixes = {QStringLiteral("mp4"),  QStringLiteral("mov"), QStringLiteral("mkv"),
				    QStringLiteral("webm"), QStringLiteral("m4v"), QStringLiteral("flv"),
				    QStringLiteral("ts"),   QStringLiteral("avi")};

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

QString takeObsString(char *value)
{
	QString result = value ? QString::fromUtf8(value) : QString();
	bfree(value);
	return result;
}

QLabel *roleLabel(const QString &content, const char *role, QWidget *parent)
{
	auto *label = new QLabel(content, parent);
	label->setProperty("vpRole", QString::fromLatin1(role));
	return label;
}

QPushButton *roleButton(const QString &content, const char *role, QWidget *parent)
{
	auto *button = new QPushButton(content, parent);
	button->setProperty("vpRole", QString::fromLatin1(role));
	button->setCursor(Qt::PointingHandCursor);
	return button;
}

// h6 in the design: Barlow Condensed 13 px, uppercase, 0.08em tracking.
QFont sectionFont()
{
	QFont font = DockTheme::headingFont(13, QFont::DemiBold);
	font.setLetterSpacing(QFont::PercentageSpacing, 108);
	return font;
}

// The legacy default the title template shipped with; recordings now title as the mockup.
const QString kLegacyTitleTemplate = QStringLiteral("{profile} – {date} {time}");
const QString kTitleTemplate = QStringLiteral("{date} {time}");

} // namespace

VideoPressDock::VideoPressDock(videopress::SettingsStore *settings, std::function<void()> saveSettings, QWidget *parent)
	: QWidget(parent),
	  m_settings(settings),
	  m_uploads(new UploadController(settings, this)),
	  m_saveSettings(std::move(saveSettings)),
	  m_sitesEmpty(new QWidget(this)),
	  m_sitesEmptyTitle(new QLabel(text("VideoPress.Empty.Title"), m_sitesEmpty)),
	  m_emptyAddress(new QLineEdit(m_sitesEmpty)),
	  m_emptyError(roleLabel(QString(), "error", m_sitesEmpty)),
	  m_emptyContinue(roleButton(text("VideoPress.Empty.Continue"), "primary", nullptr)),
	  m_siteField(new QWidget(this)),
	  m_sitePicker(roleButton(QString(), "input", m_siteField)),
	  m_siteName(new QLabel(m_sitePicker)),
	  m_siteDomain(roleLabel(QString(), "muted", m_sitePicker)),
	  m_siteChevron(new QLabel(m_sitePicker)),
	  m_addSite(roleButton(QString(), "icon", m_siteField)),
	  m_removeSite(roleButton(QString(), "icon", m_siteField)),
	  m_listTitle(new QLabel(this)),
	  m_uploadFile(roleButton(text("VideoPress.UploadFile"), "ghost", this)),
	  m_listFrame(new QWidget(this)),
	  m_listStack(new QStackedWidget(m_listFrame)),
	  m_emptyList(new QWidget(m_listStack)),
	  m_emptyIcon(new QLabel(m_emptyList)),
	  m_emptyTitle(new QLabel(text("VideoPress.List.EmptyTitle"), m_emptyList)),
	  m_chooseFile(roleButton(text("VideoPress.List.ChooseFile"), "secondary", m_emptyList)),
	  m_rows(nullptr),
	  m_dropOverlay(new DropOverlay(&m_theme, m_listFrame)),
	  m_autoUpload(new SelectBox(&m_theme, nullptr)),
	  m_footerText(roleLabel(QString(), "footer", nullptr)),
	  m_uploadFrame(nullptr),
	  m_uploadSelected(roleButton(text("VideoPress.Footer.Upload"), "primary", nullptr)),
	  m_cancelAll(roleButton(text("VideoPress.Footer.CancelAll"), "secondary", nullptr)),
	  m_hint(roleLabel(QString(), "hint", nullptr))
{
	setObjectName(QStringLiteral("vpDock"));
	setAttribute(Qt::WA_StyledBackground, true);
	setAcceptDrops(true);
	m_theme = DockTheme::fromPalette(QGuiApplication::palette());

	// — No site yet ———————————————————————————————————————————————————————
	m_sitesEmptyTitle->setWordWrap(true);
	auto *emptyBody = roleLabel(text("VideoPress.Empty.Body"), "muted", m_sitesEmpty);
	emptyBody->setWordWrap(true);
	m_emptyAddress->setPlaceholderText(QStringLiteral("https://example.com"));
	m_emptyAddress->setAccessibleName(text("VideoPress.AddSite.Url"));
	m_emptyError->setWordWrap(true);
	m_emptyError->hide();
	auto *continueFrame = new BlueprintFrame(m_emptyContinue, &m_theme, m_sitesEmpty);
	auto *addressRow = new QHBoxLayout();
	addressRow->setSpacing(kSpace2);
	addressRow->addWidget(m_emptyAddress, 1);
	addressRow->addWidget(continueFrame);
	auto *sitesEmptyLayout = new QVBoxLayout(m_sitesEmpty);
	sitesEmptyLayout->setContentsMargins(kSpace4, kSpace4, kSpace4, kSpace3);
	sitesEmptyLayout->setSpacing(kSpace2);
	sitesEmptyLayout->addWidget(m_sitesEmptyTitle);
	sitesEmptyLayout->addWidget(emptyBody);
	sitesEmptyLayout->addLayout(addressRow);
	sitesEmptyLayout->addWidget(m_emptyError);

	// — Site field: label, picker (name + domain + chevron), add, remove ————————
	m_siteName->setFont(DockTheme::bodyFont(14, QFont::Medium));
	m_siteDomain->setFont(DockTheme::bodyFont(13));
	for (QWidget *w : {static_cast<QWidget *>(m_siteName), static_cast<QWidget *>(m_siteDomain),
			   static_cast<QWidget *>(m_siteChevron)})
		w->setAttribute(Qt::WA_TransparentForMouseEvents);
	auto *pickerLayout = new QHBoxLayout(m_sitePicker);
	pickerLayout->setContentsMargins(10, 0, 8, 0);
	pickerLayout->setSpacing(6);
	pickerLayout->addWidget(m_siteName);
	pickerLayout->addWidget(m_siteDomain, 1);
	pickerLayout->addWidget(m_siteChevron);
	m_sitePicker->setAccessibleName(text("VideoPress.Site"));
	m_addSite->setToolTip(text("VideoPress.AddSite.Title"));
	m_addSite->setAccessibleName(text("VideoPress.AddSite.Title"));
	m_removeSite->setToolTip(text("VideoPress.RemoveSite"));
	m_removeSite->setAccessibleName(text("VideoPress.RemoveSite"));

	auto *siteRow = new QHBoxLayout();
	siteRow->setSpacing(6);
	siteRow->addWidget(m_sitePicker, 1);
	siteRow->addWidget(m_addSite);
	siteRow->addWidget(m_removeSite);
	auto *siteFieldLayout = new QVBoxLayout(m_siteField);
	siteFieldLayout->setContentsMargins(kSpace4, kSpace4, kSpace4, kSpace3);
	siteFieldLayout->setSpacing(5);
	siteFieldLayout->addWidget(roleLabel(text("VideoPress.Site"), "fieldLabel", m_siteField));
	siteFieldLayout->addLayout(siteRow);

	// — "RECORDINGS n" + Upload a file… ————————————————————————————————————
	m_listTitle->setFont(sectionFont());
	m_listTitle->setTextFormat(Qt::RichText);
	auto *headerRow = new QHBoxLayout();
	headerRow->setContentsMargins(kSpace4, 0, kSpace4, kSpace2);
	headerRow->setSpacing(8);
	headerRow->addWidget(m_listTitle, 1);
	headerRow->addWidget(m_uploadFile);

	// — List: empty state | rows, with the drop target on top ———————————————
	m_listFrame->setObjectName(QStringLiteral("vpListFrame"));
	m_listFrame->setAttribute(Qt::WA_StyledBackground, true);

	m_emptyIcon->setFixedSize(64, 64);
	m_emptyIcon->setAlignment(Qt::AlignCenter);
	m_emptyIcon->setObjectName(QStringLiteral("vpEmptyIcon"));
	auto *emptyIconFrame = new BlueprintFrame(m_emptyIcon, &m_theme, m_emptyList);
	m_emptyTitle->setFont(DockTheme::headingFont(20, QFont::DemiBold));
	m_emptyTitle->setAlignment(Qt::AlignCenter);
	auto *emptyListBody = roleLabel(text("VideoPress.List.EmptyBody"), "footer", m_emptyList);
	emptyListBody->setWordWrap(true);
	emptyListBody->setAlignment(Qt::AlignCenter);
	// A wrapped label centred in a layout only gets its narrow hint; give it the design's width.
	emptyListBody->setFixedWidth(240);
	auto *dropHint = roleLabel(text("VideoPress.List.DropHint"), "hint", m_emptyList);
	dropHint->setAlignment(Qt::AlignCenter);
	auto *emptyListLayout = new QVBoxLayout(m_emptyList);
	emptyListLayout->setContentsMargins(kSpace6, kSpace6, kSpace6, kSpace6);
	emptyListLayout->setSpacing(kSpace3);
	emptyListLayout->addStretch(1);
	emptyListLayout->addWidget(emptyIconFrame, 0, Qt::AlignHCenter);
	emptyListLayout->addWidget(m_emptyTitle);
	emptyListLayout->addWidget(emptyListBody, 0, Qt::AlignHCenter);
	emptyListLayout->addSpacing(kSpace2 - kSpace3 + kSpace2);
	emptyListLayout->addWidget(m_chooseFile, 0, Qt::AlignHCenter);
	emptyListLayout->addWidget(dropHint);
	emptyListLayout->addStretch(1);

	auto *scroll = new QScrollArea(m_listStack);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	auto *rowsHost = new QWidget(scroll);
	m_rows = new QVBoxLayout(rowsHost);
	m_rows->setContentsMargins(0, 0, 0, 0);
	m_rows->setSpacing(0);
	m_rows->addStretch(1);
	scroll->setWidget(rowsHost);

	// Docks can be much shorter than the mockup: let the empty state scroll rather than
	// letting the layout overlap its pieces.
	auto *emptyScroll = new QScrollArea(m_listStack);
	emptyScroll->setWidgetResizable(true);
	emptyScroll->setFrameShape(QFrame::NoFrame);
	emptyScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	emptyScroll->setWidget(m_emptyList);
	m_listStack->addWidget(emptyScroll);
	m_listStack->addWidget(scroll);
	auto *listFrameLayout = new QVBoxLayout(m_listFrame);
	listFrameLayout->setContentsMargins(0, 0, 0, 0);
	listFrameLayout->addWidget(m_listStack);

	auto *listRow = new QHBoxLayout();
	listRow->setContentsMargins(kSpace4, 0, kSpace4, 0);
	listRow->addWidget(m_listFrame);

	// — Footer: auto-upload toggle; status text + Upload N / Cancel all ————————
	auto *footer = new QWidget(this);
	footer->setObjectName(QStringLiteral("vpFooter"));
	footer->setAttribute(Qt::WA_StyledBackground, true);
	m_autoUpload->setParent(footer);
	m_autoUpload->setAccessibleName(text("VideoPress.AutoUpload"));
	auto *autoLabel = new QLabel(text("VideoPress.AutoUpload"), footer);
	autoLabel->setCursor(Qt::PointingHandCursor);
	autoLabel->installEventFilter(this); // the whole label toggles, like <label> in the design
	m_autoLabel = autoLabel;
	auto *autoRow = new QHBoxLayout();
	autoRow->setSpacing(8);
	autoRow->addWidget(m_autoUpload);
	autoRow->addWidget(autoLabel, 1);

	m_footerText->setParent(footer);
	m_footerText->setWordWrap(true);
	m_uploadSelected->setParent(footer);
	m_uploadFrame = new BlueprintFrame(m_uploadSelected, &m_theme, footer);
	m_cancelAll->setParent(footer);
	m_hint->setParent(footer);
	m_hint->setWordWrap(true);
	auto *actionRow = new QHBoxLayout();
	actionRow->setSpacing(kSpace3);
	actionRow->addWidget(m_footerText, 1);
	actionRow->addWidget(m_uploadFrame);
	actionRow->addWidget(m_cancelAll);
	auto *footerLayout = new QVBoxLayout(footer);
	footerLayout->setContentsMargins(kSpace4, kSpace3, kSpace4, kSpace4);
	footerLayout->setSpacing(kSpace3);
	footerLayout->addLayout(autoRow);
	footerLayout->addLayout(actionRow);
	footerLayout->addWidget(m_hint);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	layout->addWidget(m_sitesEmpty);
	layout->addWidget(m_siteField);
	layout->addLayout(headerRow);
	layout->addLayout(listRow, 1);
	layout->addWidget(footer);

	// — Wiring ————————————————————————————————————————————————————————————
	connect(m_emptyContinue, &QPushButton::clicked, this, &VideoPressDock::continueFromEmptyState);
	connect(m_emptyAddress, &QLineEdit::returnPressed, this, &VideoPressDock::continueFromEmptyState);
	connect(m_sitePicker, &QPushButton::clicked, this, &VideoPressDock::showSiteMenu);
	connect(m_addSite, &QPushButton::clicked, this, [this] { addSite(); });
	connect(m_removeSite, &QPushButton::clicked, this, &VideoPressDock::removeSite);
	connect(m_uploadFile, &QPushButton::clicked, this, &VideoPressDock::chooseFiles);
	connect(m_chooseFile, &QPushButton::clicked, this, &VideoPressDock::chooseFiles);
	connect(m_autoUpload, &SelectBox::toggled, this, &VideoPressDock::setAutoUpload);
	connect(m_uploadSelected, &QPushButton::clicked, this, [this] { m_uploads->startSelected(activeBlogId()); });
	connect(m_cancelAll, &QPushButton::clicked, m_uploads, &UploadController::cancelAll);

	connect(m_uploads, &UploadController::jobAdded, this, &VideoPressDock::addRow);
	connect(m_uploads, &UploadController::jobChanged, this, &VideoPressDock::updateRow);
	connect(m_uploads, &UploadController::jobRemoved, this, &VideoPressDock::removeRow);
	connect(m_uploads, &UploadController::pausedChanged, this, &VideoPressDock::updateFooter);

	// "Uploaded 4 min ago" ages while the dock is open.
	m_relativeTimeTicker.setInterval(30 * 1000);
	connect(&m_relativeTimeTicker, &QTimer::timeout, this, [this] {
		for (const UploadController::Job &job : m_uploads->jobs()) {
			if (job.state == UploadController::State::Done)
				updateRow(job.id);
		}
	});
	m_relativeTimeTicker.start();

	applyTheme();
	reloadProfile();
	updateListState();
}

// — Theme ————————————————————————————————————————————————————————————————

void VideoPressDock::applyTheme()
{
	if (m_applyingTheme)
		return;
	m_applyingTheme = true;
	m_theme = DockTheme::fromPalette(QGuiApplication::palette());
	setStyleSheet(m_theme.styleSheet());

	m_addSite->setIcon(dockIcon(DockIcon::Plus, m_theme.text, 16));
	m_removeSite->setIcon(dockIcon(DockIcon::Minus, m_theme.text, 16));
	m_siteChevron->setPixmap(dockIcon(DockIcon::ChevronDown, m_theme.text, 16).pixmap(16, 16));
	m_uploadFile->setIcon(dockIcon(DockIcon::Upload, m_theme.link, 14));
	m_uploadFile->setIconSize(QSize(14, 14));
	m_emptyIcon->setPixmap(dockIcon(DockIcon::Video, m_theme.accent, 28).pixmap(28, 28));
	m_emptyIcon->setStyleSheet(
		QStringLiteral("QLabel#vpEmptyIcon { border: 1px solid %1; }").arg(DockTheme::css(m_theme.divider)));
	m_sitesEmptyTitle->setFont(DockTheme::headingFont(20, QFont::DemiBold));

	for (RecordingRow *row : std::as_const(m_rowWidgets))
		row->refreshTheme();
	updateListState();
	update();
	m_applyingTheme = false;
}

bool VideoPressDock::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == m_autoLabel && event->type() == QEvent::MouseButtonRelease) {
		m_autoUpload->toggle();
		return true;
	}
	return QWidget::eventFilter(watched, event);
}

void VideoPressDock::changeEvent(QEvent *event)
{
	QWidget::changeEvent(event);
	// Our own stylesheet triggers StyleChange; only a palette change means a new OBS theme.
	if (event->type() == QEvent::PaletteChange && !m_applyingTheme)
		QTimer::singleShot(0, this, &VideoPressDock::applyTheme);
}

// — Profile & sites ——————————————————————————————————————————————————————

QString VideoPressDock::currentProfile() const
{
	return takeObsString(obs_frontend_get_current_profile());
}

qint64 VideoPressDock::activeBlogId() const
{
	const qint64 blogId = m_settings->profile(currentProfile()).activeBlogId;
	return m_settings->site(blogId) ? blogId : 0;
}

QString VideoPressDock::titleFor(const QString &path) const
{
	videopress::TitleContext context;
	context.profile = currentProfile();
	if (obs_source_t *scene = obs_frontend_get_current_scene()) {
		context.scene = QString::fromUtf8(obs_source_get_name(scene));
		obs_source_release(scene);
	}
	const QFileInfo info(path);
	context.recordedAt = info.lastModified();
	context.fileBaseName = info.completeBaseName();
	QString templ = m_settings->profile(context.profile).titleTemplate;
	if (templ == kLegacyTitleTemplate)
		templ = kTitleTemplate;
	return videopress::renderTitle(templ, context);
}

void VideoPressDock::reloadProfile()
{
	const ProfileSettings profile = m_settings->profile(currentProfile());
	{
		const QSignalBlocker blocker(m_autoUpload);
		m_autoUpload->setChecked(profile.afterRecording == videopress::AfterRecording::Upload);
	}
	refreshSites();
}

void VideoPressDock::refreshSites()
{
	const QList<SiteEntry> sites = m_settings->sites();
	const bool hasSites = !sites.isEmpty();
	if (hasSites && activeBlogId() == 0)
		selectSite(sites.first().blogId); // first site added becomes the active one

	m_sitesEmpty->setVisible(!hasSites);
	m_siteField->setVisible(hasSites);
	if (const SiteEntry *site = m_settings->site(activeBlogId())) {
		m_siteName->setText(site->name);
		m_siteDomain->setText(site->siteUrl.host());
		m_sitePicker->setToolTip(site->siteUrl.toString());
	}

	const bool sessionOnly = hasSites && !credential_store::isPersistent();
	m_hint->setText(sessionOnly ? text("VideoPress.Hint.SessionOnly") : QString());
	m_hint->setVisible(sessionOnly);
	updateFooter();
}

void VideoPressDock::selectSite(qint64 blogId)
{
	if (!m_settings->site(blogId))
		return;
	const QString profileName = currentProfile();
	ProfileSettings profile = m_settings->profile(profileName);
	profile.activeBlogId = blogId;
	m_settings->setProfile(profileName, profile);
	m_settings->touchRecentBlog(blogId);
	m_saveSettings();
	refreshSites();
}

void VideoPressDock::showSiteMenu()
{
	QMenu menu(this);
	menu.setObjectName(QStringLiteral("vpSiteMenu"));
	const qint64 active = activeBlogId();
	for (const SiteEntry &site : m_settings->sites()) {
		QAction *action = menu.addAction(QStringLiteral("%1   %2").arg(site.name, site.siteUrl.host()));
		action->setCheckable(true);
		action->setChecked(site.blogId == active);
		const qint64 blogId = site.blogId;
		connect(action, &QAction::triggered, this, [this, blogId] { selectSite(blogId); });
	}
	menu.addSeparator();
	connect(menu.addAction(text("VideoPress.AddSite.Title")), &QAction::triggered, this, [this] { addSite(); });
	menu.setMinimumWidth(m_sitePicker->width());
	menu.exec(m_sitePicker->mapToGlobal(QPoint(0, m_sitePicker->height())));
}

void VideoPressDock::continueFromEmptyState()
{
	QString error;
	const QUrl siteUrl = videopress::JetpackSiteClient::normalizeSiteUrl(m_emptyAddress->text(), &error);
	m_emptyError->setText(error);
	m_emptyError->setVisible(!siteUrl.isValid());
	if (siteUrl.isValid())
		addSite(siteUrl.toString());
}

void VideoPressDock::addSite(const QString &address)
{
	AddSiteDialog dialog(this, address);
	if (dialog.exec() != QDialog::Accepted)
		return;
	m_emptyAddress->clear();
	const SiteEntry site = dialog.site();
	m_settings->upsertSite(site);
	selectSite(site.blogId);
}

void VideoPressDock::removeSite()
{
	const qint64 blogId = activeBlogId();
	const SiteEntry *site = m_settings->site(blogId);
	if (!site)
		return;
	const auto answer = QMessageBox::question(this, text("VideoPress.RemoveSite"),
						  text("VideoPress.RemoveSite.Confirm").arg(site->name));
	if (answer != QMessageBox::Yes)
		return;
	credential_store::remove(credential_store::accountFor(site->siteUrl, site->username));
	m_settings->removeSite(blogId);

	const QString profileName = currentProfile();
	ProfileSettings profile = m_settings->profile(profileName);
	profile.activeBlogId = 0;
	m_settings->setProfile(profileName, profile);
	m_saveSettings();
	refreshSites();
}

void VideoPressDock::setAutoUpload(bool enabled)
{
	const QString profileName = currentProfile();
	ProfileSettings profile = m_settings->profile(profileName);
	profile.afterRecording = enabled ? videopress::AfterRecording::Upload : videopress::AfterRecording::Ask;
	m_settings->setProfile(profileName, profile);
	m_saveSettings();
}

// — Adding recordings and files ——————————————————————————————————————————

void VideoPressDock::handleRecording(const QString &path, qint64 durationMs, const QString &resolutionLabel)
{
	const bool upload = m_settings->profile(currentProfile()).afterRecording == videopress::AfterRecording::Upload;
	const qint64 blogId = activeBlogId();
	UploadController::Source source;
	source.fromRecording = true;
	source.durationMs = durationMs;
	source.resolutionLabel = resolutionLabel;
	m_uploads->add(path, titleFor(path), blogId, upload && blogId > 0, source);
}

void VideoPressDock::chooseFiles()
{
	const QStringList paths = QFileDialog::getOpenFileNames(
		this, text("VideoPress.UploadFile"), QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
		text("VideoPress.UploadFile.Filter") + QStringLiteral(" (*.") +
			kVideoSuffixes.join(QStringLiteral(" *.")) + QLatin1Char(')'));
	uploadFiles(paths);
}

void VideoPressDock::uploadFiles(const QStringList &paths)
{
	const qint64 blogId = activeBlogId();
	for (const QString &path : paths) {
		UploadController::Source source; // a file, not an OBS recording
		m_uploads->add(path, QFileInfo(path).completeBaseName(), blogId, blogId > 0, source);
	}
}

QStringList VideoPressDock::videoFiles(const QMimeData *mime)
{
	QStringList files;
	if (!mime || !mime->hasUrls())
		return files;
	for (const QUrl &url : mime->urls()) {
		const QFileInfo info(url.toLocalFile());
		if (url.isLocalFile() && info.isFile() && kVideoSuffixes.contains(info.suffix().toLower()))
			files.append(info.absoluteFilePath());
	}
	return files;
}

void VideoPressDock::dragEnterEvent(QDragEnterEvent *event)
{
	const QStringList files = videoFiles(event->mimeData());
	const SiteEntry *site = m_settings->site(activeBlogId());
	if (files.isEmpty() || !site) {
		event->ignore();
		return;
	}
	const QFileInfo first(files.first());
	QString detail = QStringLiteral("%1 · %2").arg(first.fileName(), dock_format::bytes(first.size()));
	if (files.size() > 1)
		detail = text("VideoPress.Drop.Files").arg(files.size());
	m_dropOverlay->setDetails(detail, site->name);
	positionDropOverlay();
	m_dropOverlay->show();
	m_dropOverlay->raise();
	event->acceptProposedAction();
}

void VideoPressDock::dragMoveEvent(QDragMoveEvent *event)
{
	event->acceptProposedAction();
}

void VideoPressDock::dragLeaveEvent(QDragLeaveEvent *event)
{
	m_dropOverlay->hide();
	QWidget::dragLeaveEvent(event);
}

void VideoPressDock::dropEvent(QDropEvent *event)
{
	m_dropOverlay->hide();
	const QStringList files = videoFiles(event->mimeData());
	if (files.isEmpty())
		return;
	event->acceptProposedAction();
	uploadFiles(files);
}

void VideoPressDock::resizeEvent(QResizeEvent *event)
{
	QWidget::resizeEvent(event);
	positionDropOverlay();
}

void VideoPressDock::positionDropOverlay()
{
	m_dropOverlay->setGeometry(m_listFrame->rect());
}

// — Rows ————————————————————————————————————————————————————————————————

void VideoPressDock::addRow(int jobId)
{
	auto *row = new RecordingRow(&m_theme);
	m_rowWidgets.insert(jobId, row);
	m_rows->insertWidget(0, row); // newest first

	connect(row, &RecordingRow::selectToggled, this,
		[this, jobId](bool selected) { m_uploads->setSelected(jobId, selected); });
	connect(row, &RecordingRow::cancelClicked, this, [this, jobId] { m_uploads->cancel(jobId); });
	connect(row, &RecordingRow::retryClicked, this, [this, jobId] {
		const UploadController::Job *job = m_uploads->job(jobId);
		m_uploads->start(jobId, job && job->blogId > 0 ? job->blogId : activeBlogId());
	});
	connect(row, &RecordingRow::copyLinkClicked, this, [this, jobId] {
		if (const UploadController::Job *job = m_uploads->job(jobId))
			QGuiApplication::clipboard()->setText(dock_format::videoUrl(job->guid));
	});
	connect(row, &RecordingRow::openLinkClicked, this, [this, jobId] {
		if (const UploadController::Job *job = m_uploads->job(jobId))
			QDesktopServices::openUrl(QUrl(dock_format::videoUrl(job->guid)));
	});
	connect(row, &RecordingRow::removeRequested, this, [this, jobId] { m_uploads->remove(jobId); });
	connect(row, &RecordingRow::showFileRequested, this, [this, jobId] {
		if (const UploadController::Job *job = m_uploads->job(jobId))
			QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(job->path).absolutePath()));
	});

	updateRow(jobId);
}

void VideoPressDock::updateRow(int jobId)
{
	RecordingRow *row = m_rowWidgets.value(jobId);
	const UploadController::Job *job = m_uploads->job(jobId);
	if (!row || !job)
		return;
	row->setJob(*job);
	if (job->posterUrl.isValid()) {
		if (m_posters.contains(job->posterUrl))
			row->setPoster(m_posters.value(job->posterUrl));
		else
			requestPoster(jobId, job->posterUrl);
	}
	updateListState();
}

void VideoPressDock::removeRow(int jobId)
{
	if (RecordingRow *row = m_rowWidgets.take(jobId))
		row->deleteLater();
	updateListState();
}

void VideoPressDock::requestPoster(int jobId, const QUrl &url)
{
	if (m_posterRequests.contains(url))
		return;
	m_posterRequests.insert(url);
	QNetworkRequest request(url);
	request.setTransferTimeout(20000);
	QNetworkReply *reply = m_network.get(request);
	connect(reply, &QNetworkReply::finished, this, [this, reply, url, jobId] {
		reply->deleteLater();
		QPixmap poster;
		if (reply->error() == QNetworkReply::NoError && poster.loadFromData(reply->readAll())) {
			m_posters.insert(url, poster);
			if (RecordingRow *row = m_rowWidgets.value(jobId))
				row->setPoster(poster);
		} else {
			m_posterRequests.remove(url); // try again on a later update
		}
	});
}

void VideoPressDock::updateListState()
{
	const int count = int(m_uploads->jobs().size());
	m_listTitle->setText(QStringLiteral("%1 <span style=\"color:%2\">%3</span>")
				     .arg(text("VideoPress.List.Title").toUpper().toHtmlEscaped(),
					  DockTheme::css(m_theme.neutral500),
					  count ? QString::number(count) : QString()));
	m_listTitle->setStyleSheet(QStringLiteral("color: %1;").arg(DockTheme::css(m_theme.neutral700)));
	m_listStack->setCurrentIndex(count ? 1 : 0);
	updateFooter();
}

void VideoPressDock::updateFooter()
{
	// Mockup logic: "N selected · size" with Upload N; "Uploading N · keep OBS open" with
	// Cancel all while uploads run and nothing new is ticked.
	int chosen = 0;
	int uploading = 0;
	qint64 chosenBytes = 0;
	for (const UploadController::Job &job : m_uploads->jobs()) {
		if (job.selected && UploadController::isSelectable(job)) {
			++chosen;
			chosenBytes += job.bytesTotal;
		}
		if (job.state == UploadController::State::Uploading || job.state == UploadController::State::Queued)
			++uploading;
	}
	const bool hasItems = !m_uploads->jobs().isEmpty();
	const bool hasSite = activeBlogId() > 0;

	QString status = chosen ? text("VideoPress.Footer.Selected").arg(chosen).arg(dock_format::bytes(chosenBytes))
				: text("VideoPress.Footer.SelectPrompt");
	if (uploading && !chosen)
		status = m_uploads->isPaused() ? text("VideoPress.Footer.Paused")
					       : text("VideoPress.Footer.Uploading").arg(uploading);
	if (!hasItems)
		status.clear();
	m_footerText->setText(status);
	m_footerText->setVisible(!status.isEmpty());

	const bool showUpload = hasItems && !(uploading && !chosen);
	m_uploadFrame->setVisible(showUpload);
	m_uploadSelected->setEnabled(chosen > 0 && hasSite);
	m_uploadSelected->setText(chosen ? text("VideoPress.Footer.UploadN").arg(chosen)
					 : text("VideoPress.Footer.Upload"));
	m_cancelAll->setVisible(uploading > 0 && !chosen);
}

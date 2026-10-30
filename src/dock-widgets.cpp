// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock-widgets.hpp"

#include <obs-module.h>

#include <QContextMenuEvent>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {

constexpr int kMarkOverhang = 6; // registration marks sit 6 px outside the frame
constexpr int kLeadIndent = 26;  // sub-rows line up under the thumbnail (16 + 10)

QString text(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

void setRole(QWidget *widget, const char *property, const char *value)
{
	widget->setProperty(property, QString::fromLatin1(value));
	widget->style()->unpolish(widget);
	widget->style()->polish(widget);
}

QColor withAlpha(QColor color, qreal alpha)
{
	color.setAlphaF(float(alpha));
	return color;
}

} // namespace

// — formatting ———————————————————————————————————————————————————————————

namespace dock_format {

QString bytes(qint64 value)
{
	// Decimal units, like the mockup: 612 MB, 1.4 GB.
	const double mb = double(value) / 1e6;
	if (mb >= 1000)
		return QStringLiteral("%1 GB").arg(mb / 1000, 0, 'f', 1);
	if (mb >= 1)
		return QStringLiteral("%1 MB").arg(qRound(mb));
	return QStringLiteral("%1 KB").arg(qMax<qint64>(1, value / 1000));
}

QString duration(qint64 ms)
{
	if (ms <= 0)
		return QString();
	const qint64 total = ms / 1000;
	const qint64 h = total / 3600, m = (total % 3600) / 60, s = total % 60;
	if (h > 0)
		return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
	return QStringLiteral("%1:%2").arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
}

QString timeLeft(qint64 seconds)
{
	if (seconds < 0)
		return text("VideoPress.Upload.Starting");
	if (seconds < 60)
		return text("VideoPress.Upload.SecondsLeft").arg(qMax<qint64>(5, (seconds + 4) / 5 * 5));
	if (seconds < 3600)
		return text("VideoPress.Upload.MinutesLeft").arg((seconds + 30) / 60);
	return text("VideoPress.Upload.HoursLeft").arg(seconds / 3600).arg((seconds % 3600) / 60);
}

QString relative(const QDateTime &when)
{
	const qint64 secs = when.secsTo(QDateTime::currentDateTime());
	if (secs < 60)
		return text("VideoPress.Time.JustNow");
	if (secs < 3600)
		return text("VideoPress.Time.MinutesAgo").arg(secs / 60);
	if (secs < 24 * 3600)
		return text("VideoPress.Time.HoursAgo").arg(secs / 3600);
	return when.toString(QStringLiteral("MMM d"));
}

QString videoUrl(const QString &guid)
{
	return QStringLiteral("https://videopress.com/v/") + guid;
}

} // namespace dock_format

// — BlueprintFrame ————————————————————————————————————————————————————————

BlueprintFrame::BlueprintFrame(QWidget *child, const DockTheme *theme, QWidget *parent)
	: QWidget(parent),
	  m_child(child),
	  m_theme(theme)
{
	auto *layout = new QHBoxLayout(this);
	layout->setContentsMargins(kMarkOverhang, kMarkOverhang, kMarkOverhang, kMarkOverhang);
	layout->addWidget(child);
}

void BlueprintFrame::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setPen(QPen(withAlpha(m_theme->text, 0.55), 1));
	const QRect r = m_child->geometry();
	// An 11 px cross centred on each corner, like .blueprint > .corner.
	for (const QPoint &c : {r.topLeft(), QPoint(r.right() + 1, r.top()), QPoint(r.left(), r.bottom() + 1),
				QPoint(r.right() + 1, r.bottom() + 1)}) {
		painter.drawLine(c.x() - 5, c.y(), c.x() + 5, c.y());
		painter.drawLine(c.x(), c.y() - 5, c.x(), c.y() + 5);
	}
}

// — SelectBox —————————————————————————————————————————————————————————————

SelectBox::SelectBox(const DockTheme *theme, QWidget *parent) : QAbstractButton(parent), m_theme(theme)
{
	setCheckable(true);
	setCursor(Qt::PointingHandCursor);
	setFixedSize(16, 16);
	setFocusPolicy(Qt::StrongFocus);
}

void SelectBox::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QRectF box = QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75);
	const QColor border = isChecked() ? m_theme->accent : (underMouse() ? m_theme->accent : m_theme->neutral400);
	painter.setPen(QPen(border, 1.5));
	painter.setBrush(isChecked() ? m_theme->accent : Qt::transparent);
	painter.drawRect(box);
	if (isChecked())
		paintDockIcon(painter, DockIcon::Check, QRectF(2, 2, 12, 12), m_theme->bg, 3.0);
	if (hasFocus()) {
		painter.setPen(QPen(m_theme->accent, 1));
		painter.setBrush(Qt::NoBrush);
		painter.drawRect(QRectF(rect()).adjusted(-0.5, -0.5, 0.5, 0.5));
	}
}

// — Thumbnail —————————————————————————————————————————————————————————————

Thumbnail::Thumbnail(const DockTheme *theme, QWidget *parent) : QWidget(parent), m_theme(theme)
{
	setFixedSize(64, 36);
}

void Thumbnail::setPoster(const QPixmap &poster)
{
	m_poster = poster;
	update();
}

void Thumbnail::setDuration(const QString &duration)
{
	m_duration = duration;
	update();
}

void Thumbnail::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	const QRect inner = rect().adjusted(1, 1, -1, -1);
	if (!m_poster.isNull()) {
		// Centre-crop the poster into the 16:9 slot.
		painter.save();
		painter.setClipRect(inner);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
		QRect target(QPoint(0, 0), m_poster.size().scaled(inner.size(), Qt::KeepAspectRatioByExpanding));
		target.moveCenter(inner.center());
		painter.drawPixmap(target, m_poster);
		painter.restore();
	} else {
		// repeating-linear-gradient(135deg, neutral-200 0 4px, transparent 4px 8px)
		painter.save();
		painter.setClipRect(inner);
		painter.setRenderHint(QPainter::Antialiasing, true);
		painter.setPen(QPen(m_theme->neutral200, 2.83));
		for (int x = -inner.height(); x < inner.width() + inner.height(); x += 8)
			painter.drawLine(QPointF(inner.left() + x, inner.bottom() + 1),
					 QPointF(inner.left() + x + inner.height() + 1, inner.top()));
		painter.restore();
	}
	painter.setPen(QPen(m_theme->divider, 1));
	painter.setBrush(Qt::NoBrush);
	painter.drawRect(rect().adjusted(0, 0, -1, -1));

	if (!m_duration.isEmpty()) {
		const QFont font = DockTheme::bodyFont(9, QFont::Medium);
		painter.setFont(font);
		const QFontMetrics metrics(font);
		const QRect badge(width() - 2 - metrics.horizontalAdvance(m_duration) - 6, height() - 2 - 13,
				  metrics.horizontalAdvance(m_duration) + 6, 13);
		painter.fillRect(badge, m_theme->dark ? QColor(0, 0, 0, 200) : m_theme->neutral900);
		painter.setPen(m_theme->dark ? m_theme->text : m_theme->bg);
		painter.drawText(badge, Qt::AlignCenter, m_duration);
	}
}

// — ElidedLabel ———————————————————————————————————————————————————————————

void ElidedLabel::setFullText(const QString &full)
{
	m_full = full;
	setToolTip(full);
	updateElided();
}

QSize ElidedLabel::minimumSizeHint() const
{
	return QSize(20, QLabel::minimumSizeHint().height());
}

void ElidedLabel::resizeEvent(QResizeEvent *event)
{
	QLabel::resizeEvent(event);
	updateElided();
}

void ElidedLabel::updateElided()
{
	const QString elided = fontMetrics().elidedText(m_full, Qt::ElideRight, qMax(0, width()));
	if (textFormat() == Qt::RichText && !property("vpHref").toString().isEmpty())
		QLabel::setText(QStringLiteral("<a href=\"%1\" style=\"color:%3\">%2</a>")
					.arg(property("vpHref").toString().toHtmlEscaped(), elided.toHtmlEscaped(),
					     property("vpLinkColor").toString()));
	else
		QLabel::setText(elided);
}

// — DropOverlay ———————————————————————————————————————————————————————————

DropOverlay::DropOverlay(const DockTheme *theme, QWidget *parent) : QWidget(parent), m_theme(theme)
{
	setAttribute(Qt::WA_TransparentForMouseEvents);
	hide();
}

void DropOverlay::setDetails(const QString &file, const QString &site)
{
	m_file = file;
	m_site = site;
	update();
}

void DropOverlay::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const QRectF box = QRectF(rect()).adjusted(0.75, 6.75, -0.75, -6.75);
	painter.fillRect(box, withAlpha(m_theme->accent100, 0.92));
	QPen dashed(m_theme->accent, 1.5, Qt::DashLine);
	dashed.setDashPattern({4, 3});
	painter.setPen(dashed);
	painter.drawRect(box);

	const QColor ink = m_theme->dark ? m_theme->link : m_theme->accent800;
	const QFont title = DockTheme::headingFont(20, QFont::DemiBold);
	const QFont detail = DockTheme::bodyFont(12.5);
	const QFont site = DockTheme::bodyFont(12);
	const int lineTitle = QFontMetrics(title).height();
	const int lineDetail = QFontMetrics(detail).height();
	const int total = 32 + 8 + lineTitle + 8 + lineDetail + 8 + lineDetail;
	int y = int(box.center().y()) - total / 2;

	paintDockIcon(painter, DockIcon::Upload, QRectF(box.center().x() - 16, y, 32, 32), ink);
	y += 32 + 8;
	painter.setPen(ink);
	painter.setFont(title);
	painter.drawText(QRect(0, y, width(), lineTitle), Qt::AlignHCenter, text("VideoPress.Drop.Title"));
	y += lineTitle + 8;
	painter.setFont(detail);
	painter.drawText(QRect(8, y, width() - 16, lineDetail), Qt::AlignHCenter,
			 QFontMetrics(detail).elidedText(m_file, Qt::ElideMiddle, width() - 16));
	y += lineDetail + 8;
	painter.setFont(site);
	painter.setPen(m_theme->dark ? m_theme->link : m_theme->accent700);
	painter.drawText(QRect(8, y, width() - 16, lineDetail), Qt::AlignHCenter,
			 text("VideoPress.Drop.ToSite").arg(m_site));
}

// — RecordingRow ——————————————————————————————————————————————————————————

RecordingRow::RecordingRow(const DockTheme *theme, QWidget *parent)
	: QWidget(parent),
	  m_theme(theme),
	  m_select(new SelectBox(theme, this)),
	  m_status(new QLabel(this)),
	  m_thumb(new Thumbnail(theme, this)),
	  m_title(new ElidedLabel(this)),
	  m_meta(new ElidedLabel(this)),
	  m_tag(new QLabel(this)),
	  m_progressRow(new QWidget(this)),
	  m_progress(new QProgressBar(m_progressRow)),
	  m_progressText(new QLabel(m_progressRow)),
	  m_cancel(new QPushButton(text("VideoPress.Upload.Cancel"), m_progressRow)),
	  m_linkRow(new QWidget(this)),
	  m_link(new ElidedLabel(m_linkRow)),
	  m_copy(new QPushButton(text("VideoPress.Upload.CopyLink"), m_linkRow)),
	  m_open(new QPushButton(m_linkRow)),
	  m_errorRow(new QWidget(this)),
	  m_error(new QLabel(m_errorRow)),
	  m_retry(new QPushButton(text("VideoPress.Upload.Retry"), m_errorRow))
{
	m_status->setFixedSize(16, 16);
	m_title->setFont(DockTheme::headingFont(15, QFont::DemiBold));
	m_title->setTextFormat(Qt::PlainText);
	m_meta->setProperty("vpRole", QStringLiteral("meta"));
	m_meta->setTextFormat(Qt::PlainText);
	// The title and meta give way (eliding); the tag always shows in full.
	m_tag->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

	auto *textColumn = new QVBoxLayout();
	textColumn->setSpacing(1);
	textColumn->setContentsMargins(0, 0, 0, 0);
	textColumn->addWidget(m_title);
	textColumn->addWidget(m_meta);

	auto *top = new QHBoxLayout();
	top->setSpacing(10);
	top->setContentsMargins(0, 0, 0, 0);
	top->addWidget(m_select);
	top->addWidget(m_status);
	top->addWidget(m_thumb);
	top->addLayout(textColumn, 1);
	top->addWidget(m_tag, 0, Qt::AlignVCenter);

	// Uploading: bar · "64% · 1 min left" · Cancel
	m_progress->setRange(0, 1000);
	m_progress->setTextVisible(false);
	m_progressText->setProperty("vpRole", QStringLiteral("footer"));
	m_cancel->setProperty("vpRole", QStringLiteral("ghostMuted"));
	auto *progressLayout = new QHBoxLayout(m_progressRow);
	progressLayout->setContentsMargins(kLeadIndent, 0, 0, 0);
	progressLayout->setSpacing(8);
	progressLayout->addWidget(m_progress, 1);
	progressLayout->addWidget(m_progressText);
	progressLayout->addWidget(m_cancel);

	// Done: link · Copy link · open in browser
	m_link->setTextFormat(Qt::RichText);
	m_link->setProperty("vpRole", QStringLiteral("link"));
	m_link->setOpenExternalLinks(false);
	m_copy->setProperty("vpRole", QStringLiteral("small"));
	m_open->setProperty("vpRole", QStringLiteral("smallIcon"));
	m_open->setToolTip(text("VideoPress.Upload.OpenInBrowser"));
	auto *linkLayout = new QHBoxLayout(m_linkRow);
	linkLayout->setContentsMargins(kLeadIndent, 0, 0, 0);
	linkLayout->setSpacing(6);
	linkLayout->addWidget(m_link, 1);
	linkLayout->addWidget(m_copy);
	linkLayout->addWidget(m_open);

	// Failed: reason · Retry
	m_error->setProperty("vpRole", QStringLiteral("error"));
	m_error->setWordWrap(true);
	m_error->setTextFormat(Qt::PlainText);
	m_retry->setProperty("vpRole", QStringLiteral("small"));
	auto *errorLayout = new QHBoxLayout(m_errorRow);
	errorLayout->setContentsMargins(kLeadIndent, 0, 0, 0);
	errorLayout->setSpacing(8);
	errorLayout->addWidget(m_error, 1);
	errorLayout->addWidget(m_retry, 0, Qt::AlignTop);

	auto *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 10, 0, 10);
	layout->setSpacing(8);
	layout->addLayout(top);
	layout->addWidget(m_progressRow);
	layout->addWidget(m_linkRow);
	layout->addWidget(m_errorRow);

	connect(m_select, &SelectBox::toggled, this, &RecordingRow::selectToggled);
	connect(m_cancel, &QPushButton::clicked, this, &RecordingRow::cancelClicked);
	connect(m_retry, &QPushButton::clicked, this, &RecordingRow::retryClicked);
	connect(m_copy, &QPushButton::clicked, this, &RecordingRow::copyLinkClicked);
	connect(m_open, &QPushButton::clicked, this, &RecordingRow::openLinkClicked);
	connect(m_link, &QLabel::linkActivated, this, [this] { emit openLinkClicked(); });

	refreshTheme();
}

void RecordingRow::refreshTheme()
{
	m_copy->setIcon(dockIcon(DockIcon::Copy, m_theme->text, 13));
	m_copy->setIconSize(QSize(13, 13));
	m_open->setIcon(dockIcon(DockIcon::ExternalLink, m_theme->text, 13));
	m_open->setIconSize(QSize(13, 13));
	render();
}

void RecordingRow::setJob(const UploadController::Job &job)
{
	m_job = job;
	render();
}

void RecordingRow::setPoster(const QPixmap &poster)
{
	m_thumb->setPoster(poster);
}

QString RecordingRow::metaText() const
{
	const QFileInfo info(m_job.path);
	const QString size = dock_format::bytes(m_job.bytesTotal);
	if (m_job.state == UploadController::State::Done)
		return text("VideoPress.Meta.Uploaded").arg(dock_format::relative(m_job.uploadedAt), size);
	if (m_job.state == UploadController::State::Uploading && !m_job.source.fromRecording)
		return text("VideoPress.Meta.UploadingFrom").arg(info.dir().dirName(), size);

	QStringList parts = {info.suffix().toLower(), size};
	if (!m_job.source.resolutionLabel.isEmpty())
		parts.append(m_job.source.resolutionLabel);
	return parts.join(QStringLiteral(" · "));
}

void RecordingRow::render()
{
	using State = UploadController::State;
	const bool selectable = UploadController::isSelectable(m_job);

	m_select->setVisible(selectable);
	{
		const QSignalBlocker blocker(m_select);
		m_select->setChecked(m_job.selected);
	}

	// Status glyph for jobs that are on their way or done.
	m_status->setVisible(!selectable);
	if (!selectable) {
		DockIcon icon = DockIcon::Clock;
		QColor color = m_theme->neutral500;
		if (m_job.state == State::Done) {
			icon = DockIcon::CheckCircle;
			color = m_theme->accent;
		} else if (m_job.state == State::Uploading) {
			icon = DockIcon::ArrowUpCircle;
			color = m_theme->accent;
		}
		m_status->setPixmap(dockIcon(icon, color, 16).pixmap(16, 16));
	}

	m_title->setFullText(m_job.title);
	m_meta->setFullText(metaText());
	m_thumb->setDuration(dock_format::duration(m_job.source.durationMs));

	// Tag: VideoPress state wins, then queue state, then "File" for non-recordings.
	QString tag;
	const char *tagStyle = "neutral";
	if (m_job.state == State::Done) {
		if (m_job.processing == UploadController::Processing::Ready) {
			tag = text("VideoPress.Tag.Ready");
			tagStyle = "accent";
		} else if (m_job.processing == UploadController::Processing::Processing) {
			tag = text("VideoPress.Tag.Processing");
		} else {
			tag = text("VideoPress.Tag.Uploaded");
		}
	} else if (m_job.state == State::Queued) {
		tag = text("VideoPress.Tag.Queued");
	} else if (m_job.state == State::Failed) {
		tag = text("VideoPress.Tag.Failed");
		tagStyle = "danger";
	} else if (!m_job.source.fromRecording) {
		tag = text("VideoPress.Tag.File");
		tagStyle = "outline";
	}
	m_tag->setText(tag);
	m_tag->setVisible(!tag.isEmpty());
	setRole(m_tag, "vpTag", tagStyle);
	m_tag->adjustSize();

	const bool uploading = m_job.state == State::Uploading;
	m_progressRow->setVisible(uploading);
	if (uploading) {
		const double fraction = m_job.bytesTotal > 0 ? double(m_job.bytesSent) / double(m_job.bytesTotal) : 0.0;
		m_progress->setValue(int(fraction * 1000));
		m_progressText->setText(QStringLiteral("%1% · %2")
						.arg(int(fraction * 100))
						.arg(dock_format::timeLeft(m_job.etaSeconds)));
	}

	const bool done = m_job.state == State::Done;
	m_linkRow->setVisible(done);
	if (done) {
		const QString url = dock_format::videoUrl(m_job.guid);
		m_link->setProperty("vpHref", url);
		// Inline style: the stylesheet's re-polish resets the palette's Link role.
		m_link->setProperty("vpLinkColor", DockTheme::css(m_theme->link));
		m_link->setFullText(url);
	}

	const bool failed = m_job.state == State::Failed;
	m_errorRow->setVisible(failed);
	if (failed)
		m_error->setText(m_job.message);

	update();
}

void RecordingRow::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setPen(QPen(m_theme->rowDivider, 1));
	painter.drawLine(0, height() - 1, width(), height() - 1);
}

void RecordingRow::contextMenuEvent(QContextMenuEvent *event)
{
	QMenu menu(this);
	QAction *show = menu.addAction(text("VideoPress.Upload.ShowFile"));
	QAction *remove = menu.addAction(text("VideoPress.Upload.Remove"));
	remove->setEnabled(m_job.state != UploadController::State::Uploading);
	QAction *chosen = menu.exec(event->globalPos());
	if (chosen == show)
		emit showFileRequested();
	else if (chosen == remove)
		emit removeRequested();
}

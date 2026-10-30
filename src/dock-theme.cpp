// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock-theme.hpp"

#include <obs-module.h>
#include <plugin-support.h>

#include <QFontDatabase>
#include <QStringList>

#include <algorithm>

namespace {

QString g_headingFamily;
QString g_bodyFamily;

} // namespace

QColor DockTheme::mix(const QColor &a, const QColor &b, qreal amountOfA)
{
	const qreal t = qBound(0.0, amountOfA, 1.0);
	return QColor::fromRgbF(float(a.redF() * t + b.redF() * (1 - t)), float(a.greenF() * t + b.greenF() * (1 - t)),
				float(a.blueF() * t + b.blueF() * (1 - t)),
				float(a.alphaF() * t + b.alphaF() * (1 - t)));
}

QString DockTheme::css(const QColor &color)
{
	return color.alpha() == 255 ? color.name(QColor::HexRgb)
				    : QStringLiteral("rgba(%1, %2, %3, %4)")
					      .arg(color.red())
					      .arg(color.green())
					      .arg(color.blue())
					      .arg(color.alphaF(), 0, 'f', 3);
}

DockTheme DockTheme::fromPalette(const QPalette &palette)
{
	DockTheme t;
	const QColor window = palette.color(QPalette::Window);
	t.dark = window.lightnessF() < 0.5;

	t.accent = QColor(0x59, 0x80, 0xa6);
	t.accent600 = QColor(0x59, 0x7e, 0xa3);
	t.accent700 = QColor(0x41, 0x61, 0x80);
	t.accent800 = QColor(0x2c, 0x45, 0x5d);

	if (!t.dark) {
		// The design tokens, as specified (styles.css :root).
		t.bg = QColor(0xf2, 0xf2, 0xf3);
		t.surface = QColor(0xe9, 0xe9, 0xea);
		t.text = QColor(0x1d, 0x1f, 0x20);
		t.neutral100 = QColor(0xf5, 0xf5, 0xf8);
		t.neutral200 = QColor(0xe7, 0xe7, 0xea);
		t.neutral400 = QColor(0xb7, 0xb7, 0xba);
		t.neutral500 = QColor(0x98, 0x98, 0x9b);
		t.neutral600 = QColor(0x7a, 0x7a, 0x7d);
		t.neutral700 = QColor(0x5d, 0x5d, 0x60);
		t.neutral800 = QColor(0x42, 0x42, 0x44);
		t.neutral900 = QColor(0x2b, 0x2b, 0x2d);
		t.accent100 = QColor(0xee, 0xf6, 0xff);
		t.link = t.accent700; // accent text on the light ground uses the deep step
		t.danger = QColor(0xb4, 0x3c, 0x3c);
	} else {
		// Dark variant: OBS's own ground; neutrals keep the ramp's order by mixing the
		// text color into it, so 100 is closest to the ground and 900 to the text.
		t.bg = window;
		t.surface = palette.color(QPalette::Base);
		if (qAbs(t.surface.lightnessF() - t.bg.lightnessF()) < 0.02)
			t.surface = mix(QColor(Qt::white), t.bg, 0.04);
		t.text = palette.color(QPalette::WindowText);
		t.neutral100 = mix(t.text, t.bg, 0.06);
		t.neutral200 = mix(t.text, t.bg, 0.12);
		t.neutral400 = mix(t.text, t.bg, 0.35);
		t.neutral500 = mix(t.text, t.bg, 0.48);
		t.neutral600 = mix(t.text, t.bg, 0.58);
		t.neutral700 = mix(t.text, t.bg, 0.70);
		t.neutral800 = mix(t.text, t.bg, 0.80);
		t.neutral900 = mix(t.text, t.bg, 0.90);
		t.accent100 = mix(t.accent, t.bg, 0.22);
		t.accent800 = QColor(0xb5, 0xd9, 0xfd); // accent-300: readable on dark tints
		t.link = QColor(0x94, 0xbc, 0xe3);      // accent-400 on a dark ground
		t.danger = QColor(0xe5, 0x7a, 0x7a);
	}

	QColor divider = t.text;
	divider.setAlphaF(0.16f);
	t.divider = divider;
	QColor rowDivider = t.text;
	rowDivider.setAlphaF(0.08f);
	t.rowDivider = rowDivider;
	return t;
}

void DockTheme::loadBundledFonts()
{
	static bool loaded = false;
	if (loaded)
		return;
	loaded = true;

	const char *files[] = {"fonts/Barlow-Regular.ttf", "fonts/Barlow-Medium.ttf", "fonts/Barlow-SemiBold.ttf",
			       "fonts/BarlowCondensed-Medium.ttf", "fonts/BarlowCondensed-SemiBold.ttf"};
	for (const char *file : files) {
		char *path = obs_module_file(file);
		if (!path) {
			obs_log(LOG_WARNING, "missing bundled font %s", file);
			continue;
		}
		const int id = QFontDatabase::addApplicationFont(QString::fromUtf8(path));
		bfree(path);
		if (id < 0)
			continue;
		const QStringList families = QFontDatabase::applicationFontFamilies(id);
		if (families.isEmpty())
			continue;
		if (families.first().contains(QLatin1String("Condensed")))
			g_headingFamily = families.first();
		else
			g_bodyFamily = families.first();
	}
}

QString DockTheme::headingFamily()
{
	return g_headingFamily.isEmpty() ? QStringLiteral("Barlow Condensed") : g_headingFamily;
}

QString DockTheme::bodyFamily()
{
	return g_bodyFamily.isEmpty() ? QStringLiteral("Barlow") : g_bodyFamily;
}

QFont DockTheme::headingFont(qreal pixelSize, int weight)
{
	QFont font(headingFamily());
	font.setPixelSize(qRound(pixelSize));
	font.setWeight(QFont::Weight(weight));
	return font;
}

QFont DockTheme::bodyFont(qreal pixelSize, int weight)
{
	QFont font(bodyFamily());
	font.setPixelSize(qRound(pixelSize));
	font.setWeight(QFont::Weight(weight));
	return font;
}

QString DockTheme::styleSheet() const
{
	// The design's component rules (.btn, .input, .tag, .field label), translated to QSS.
	// Everything is square: the system draws controls as hairline wireframes.
	const QString heading = headingFamily();
	const QString body = bodyFamily();
	QColor hoverTint = text;
	hoverTint.setAlphaF(0.07f);
	QColor pressTint = text;
	pressTint.setAlphaF(0.14f);
	QColor ghostHover = accent;
	ghostHover.setAlphaF(0.10f);
	QColor ghostPress = accent;
	ghostPress.setAlphaF(0.18f);
	QColor disabledText = bg;
	disabledText.setAlphaF(0.75f);

	QString qss = QStringLiteral(R"(
#vpDock { background: %bg; color: %text; font-family: "%body"; font-size: 13px; }
#vpDock QLabel { background: transparent; color: %text; }
#vpDock QLabel[vpRole="fieldLabel"] { font-size: 12px; color: %n700; }
#vpDock QLabel[vpRole="muted"] { color: %n600; }
#vpDock QLabel[vpRole="meta"] { font-size: 11px; color: %n600; }
#vpDock QLabel[vpRole="footer"] { font-size: 12px; color: %n700; }
#vpDock QLabel[vpRole="hint"] { font-size: 12px; color: %n600; }
#vpDock QLabel[vpRole="error"] { font-size: 12px; color: %danger; }
#vpDock QLabel[vpRole="link"] { font-size: 12px; }
#vpDock QWidget#vpFooter { background: %surface; border-top: 1px solid %divider; }
#vpDock QWidget#vpListFrame { border-top: 1px solid %divider; }
#vpDock QScrollArea { background: transparent; border: none; }
#vpDock QScrollArea > QWidget > QWidget { background: transparent; }

#vpDock QPushButton {
	font-family: "%heading"; font-weight: 600; font-size: 14px; color: %text;
	background: transparent; border: 1px solid %divider; border-radius: 0;
	padding: 5px 10px; min-height: 18px;
}
#vpDock QPushButton:hover { background: %hover; }
#vpDock QPushButton:pressed { background: %press; }
#vpDock QPushButton:disabled { color: %n500; }
#vpDock QPushButton:focus { outline: none; }
#vpDock QPushButton[vpRole="primary"] { background: %accent; color: %onAccent; border-color: %accent; }
#vpDock QPushButton[vpRole="primary"]:hover { background: %accent600; }
#vpDock QPushButton[vpRole="primary"]:pressed { background: %accent700; }
#vpDock QPushButton[vpRole="primary"]:disabled { background: %accentDisabled; border-color: %accentDisabled; color: %disabledText; }
#vpDock QPushButton[vpRole="ghost"] { color: %link; border-color: transparent; padding: 4px 4px; }
#vpDock QPushButton[vpRole="ghost"]:hover { background: %ghostHover; }
#vpDock QPushButton[vpRole="ghost"]:pressed { background: %ghostPress; }
#vpDock QPushButton[vpRole="ghostMuted"] { color: %n700; border-color: transparent; padding: 2px 4px; font-size: 12px; }
#vpDock QPushButton[vpRole="ghostMuted"]:hover { background: %hover; }
#vpDock QPushButton[vpRole="icon"] { padding: 0; min-width: 34px; max-width: 34px; min-height: 34px; max-height: 34px; }
#vpDock QPushButton[vpRole="small"] { padding: 2px 8px; font-size: 12px; min-height: 18px; }
#vpDock QPushButton[vpRole="smallIcon"] { padding: 0; min-width: 24px; max-width: 24px; min-height: 24px; max-height: 24px; }
#vpDock QPushButton[vpRole="input"] {
	font-family: "%body"; font-weight: 400; font-size: 14px; text-align: left;
	background: %surface; border: 1px solid %divider; padding: 0 10px; min-height: 34px;
}
#vpDock QPushButton[vpRole="input"]:hover { border-color: %inputHover; background: %surface; }
#vpDock QLineEdit {
	font-family: "%body"; font-size: 14px; color: %text; background: %surface;
	border: 1px solid %divider; border-radius: 0; padding: 6px 10px; min-height: 20px;
	selection-background-color: %selection;
}
#vpDock QLineEdit:hover { border-color: %inputHover; }
#vpDock QLineEdit:focus { border-color: %accent; }

#vpDock QLabel[vpTag="accent"] { background: %accent100; color: %accent800; font-size: 11px; padding: 2px 8px; }
#vpDock QLabel[vpTag="neutral"] { background: %n100; color: %n800; font-size: 11px; padding: 2px 8px; }
#vpDock QLabel[vpTag="outline"] { border: 1px solid %accent; color: %accentText; font-size: 11px; padding: 1px 7px; }
#vpDock QLabel[vpTag="danger"] { border: 1px solid %danger; color: %danger; font-size: 11px; padding: 1px 7px; }

#vpDock QProgressBar { background: %n200; border: none; border-radius: 0; max-height: 4px; min-height: 4px; }
#vpDock QProgressBar::chunk { background: %accent; }

#vpDock QScrollBar:vertical { background: transparent; width: 8px; margin: 0; }
#vpDock QScrollBar::handle:vertical { background: %n400; min-height: 24px; }
#vpDock QScrollBar::add-line:vertical, #vpDock QScrollBar::sub-line:vertical { height: 0; }
#vpDock QScrollBar::add-page:vertical, #vpDock QScrollBar::sub-page:vertical { background: transparent; }

QMenu#vpSiteMenu { font-family: "%body"; font-size: 13px; }
)");

	QColor inputHover = text;
	inputHover.setAlphaF(0.45f);
	QColor selection = accent;
	selection.setAlphaF(0.30f);
	const QColor accentDisabled = mix(accent, bg, 0.45);

	const QList<QPair<QString, QString>> values = {
		{QStringLiteral("%bg"), css(bg)},
		{QStringLiteral("%surface"), css(surface)},
		{QStringLiteral("%text"), css(text)},
		{QStringLiteral("%divider"), css(divider)},
		{QStringLiteral("%hover"), css(hoverTint)},
		{QStringLiteral("%press"), css(pressTint)},
		{QStringLiteral("%ghostHover"), css(ghostHover)},
		{QStringLiteral("%ghostPress"), css(ghostPress)},
		{QStringLiteral("%inputHover"), css(inputHover)},
		{QStringLiteral("%selection"), css(selection)},
		{QStringLiteral("%accentDisabled"), css(accentDisabled)},
		{QStringLiteral("%disabledText"), css(disabledText)},
		{QStringLiteral("%accent600"), css(accent600)},
		{QStringLiteral("%accent700"), css(accent700)},
		{QStringLiteral("%accent800"), css(accent800)},
		{QStringLiteral("%accent100"), css(accent100)},
		{QStringLiteral("%accentText"), css(dark ? link : accent)},
		{QStringLiteral("%accent"), css(accent)},
		{QStringLiteral("%onAccent"), css(QColor(0xf2, 0xf2, 0xf3))},
		{QStringLiteral("%link"), css(link)},
		{QStringLiteral("%danger"), css(danger)},
		{QStringLiteral("%n100"), css(neutral100)},
		{QStringLiteral("%n200"), css(neutral200)},
		{QStringLiteral("%n400"), css(neutral400)},
		{QStringLiteral("%n500"), css(neutral500)},
		{QStringLiteral("%n600"), css(neutral600)},
		{QStringLiteral("%n700"), css(neutral700)},
		{QStringLiteral("%n800"), css(neutral800)},
		{QStringLiteral("%heading"), heading},
		{QStringLiteral("%body"), body},
	};
	// Longest keys first so %accent600 is not eaten by %accent.
	QList<QPair<QString, QString>> ordered = values;
	std::sort(ordered.begin(), ordered.end(),
		  [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
	for (const auto &[key, value] : ordered)
		qss.replace(key, value);
	return qss;
}

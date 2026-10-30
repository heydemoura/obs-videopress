// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QFont>
#include <QPalette>
#include <QString>

// Tokens of the "Industry" design system the dock is drawn in (claude.ai/design project
// "VideoPress Dock"): steel-blue accent on a technical ground, Barlow Condensed headings
// over Barlow, square hairline-bordered controls. On a light OBS theme the tokens are
// used as designed; on a dark one the ground and neutrals come from OBS's palette so the
// dock sits in the window, while the accent stays the same.
struct DockTheme {
	bool dark = false;

	QColor bg;
	QColor surface;
	QColor text;
	QColor divider;    // hairlines between regions
	QColor rowDivider; // softer rule between list rows
	QColor neutral100;
	QColor neutral200;
	QColor neutral400;
	QColor neutral500;
	QColor neutral600;
	QColor neutral700;
	QColor neutral800;
	QColor neutral900;
	QColor accent;
	QColor accent100;
	QColor accent600;
	QColor accent700;
	QColor accent800;
	QColor link;   // accent text that stays readable on the ground
	QColor danger; // failed uploads

	static DockTheme fromPalette(const QPalette &palette);

	// Font families of the bundled Barlow faces, with system fallbacks.
	static QString headingFamily();
	static QString bodyFamily();
	static QFont headingFont(qreal pixelSize, int weight = QFont::DemiBold);
	static QFont bodyFont(qreal pixelSize, int weight = QFont::Normal);

	// Registers the Barlow faces shipped in the plugin's data/fonts. Call once at load.
	static void loadBundledFonts();

	// Stylesheet for the dock and its children, scoped to #vpDock.
	QString styleSheet() const;

	static QColor mix(const QColor &a, const QColor &b, qreal amountOfA);
	static QString css(const QColor &color);
};

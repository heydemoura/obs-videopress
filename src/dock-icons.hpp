// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QIcon>
#include <QRectF>

class QPainter;

// The Lucide icons the dock design uses (https://lucide.dev, ISC license), drawn from
// their 24x24 path data at the design system's 1.5 stroke.
enum class DockIcon {
	ChevronDown,
	Plus,
	Minus,
	Upload,
	Video,
	CheckCircle,
	ArrowUpCircle,
	Clock,
	AlertCircle,
	Copy,
	ExternalLink,
	Check,
};

void paintDockIcon(QPainter &painter, DockIcon icon, const QRectF &rect, const QColor &color, qreal strokeWidth = 1.5);
QIcon dockIcon(DockIcon icon, const QColor &color, int size = 16);

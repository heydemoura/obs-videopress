// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dock-icons.hpp"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

namespace {

// Rounded corner from the current point through `corner` to `to` (Lucide's a2 2 arcs).
void roundTo(QPainterPath &path, QPointF corner, QPointF to)
{
	path.quadTo(corner, to);
}

QPainterPath circle10()
{
	QPainterPath p;
	p.addEllipse(QPointF(12, 12), 10, 10);
	return p;
}

QPainterPath pathFor(DockIcon icon)
{
	QPainterPath p;
	switch (icon) {
	case DockIcon::ChevronDown:
		p.moveTo(6, 9);
		p.lineTo(12, 15);
		p.lineTo(18, 9);
		break;
	case DockIcon::Plus:
		p.moveTo(12, 5);
		p.lineTo(12, 19);
		p.moveTo(5, 12);
		p.lineTo(19, 12);
		break;
	case DockIcon::Minus:
		p.moveTo(5, 12);
		p.lineTo(19, 12);
		break;
	case DockIcon::Upload:
		// M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4  m17 8-5-5-5 5  M12 3v12
		p.moveTo(21, 15);
		p.lineTo(21, 19);
		roundTo(p, {21, 21}, {19, 21});
		p.lineTo(5, 21);
		roundTo(p, {3, 21}, {3, 19});
		p.lineTo(3, 15);
		p.moveTo(17, 8);
		p.lineTo(12, 3);
		p.lineTo(7, 8);
		p.moveTo(12, 3);
		p.lineTo(12, 15);
		break;
	case DockIcon::Video:
		// m16 13 5.2 3.5 ... V7.9 ... L16 11 ; rect 2,6 14x12
		p.moveTo(16, 13);
		p.lineTo(21.2, 16.5);
		p.lineTo(22, 16.1);
		p.lineTo(22, 7.9);
		p.lineTo(21.2, 7.5);
		p.lineTo(16, 11);
		p.addRect(2, 6, 14, 12);
		break;
	case DockIcon::CheckCircle:
		p = circle10();
		p.moveTo(9, 12);
		p.lineTo(11, 14);
		p.lineTo(15, 10);
		break;
	case DockIcon::ArrowUpCircle:
		p = circle10();
		p.moveTo(16, 12);
		p.lineTo(12, 8);
		p.lineTo(8, 12);
		p.moveTo(12, 16);
		p.lineTo(12, 8);
		break;
	case DockIcon::Clock:
		p = circle10();
		p.moveTo(12, 6);
		p.lineTo(12, 12);
		p.lineTo(16, 14);
		break;
	case DockIcon::AlertCircle:
		p = circle10();
		p.moveTo(12, 8);
		p.lineTo(12, 12);
		p.moveTo(12, 16);
		p.lineTo(12.01, 16);
		break;
	case DockIcon::Copy:
		// rect 8,8 14x14 ; M4 16c-1.1 0-2-.9-2-2V4c0-1.1.9-2 2-2h10c1.1 0 2 .9 2 2
		p.addRect(8, 8, 14, 14);
		p.moveTo(4, 16);
		roundTo(p, {2, 16}, {2, 14});
		p.lineTo(2, 4);
		roundTo(p, {2, 2}, {4, 2});
		p.lineTo(14, 2);
		roundTo(p, {16, 2}, {16, 4});
		break;
	case DockIcon::ExternalLink:
		// M15 3h6v6 ; M10 14 21 3 ; M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6
		p.moveTo(15, 3);
		p.lineTo(21, 3);
		p.lineTo(21, 9);
		p.moveTo(10, 14);
		p.lineTo(21, 3);
		p.moveTo(18, 13);
		p.lineTo(18, 19);
		roundTo(p, {18, 21}, {16, 21});
		p.lineTo(5, 21);
		roundTo(p, {3, 21}, {3, 19});
		p.lineTo(3, 8);
		roundTo(p, {3, 6}, {5, 6});
		p.lineTo(11, 6);
		break;
	case DockIcon::Check:
		p.moveTo(20, 6);
		p.lineTo(9, 17);
		p.lineTo(4, 12);
		break;
	}
	return p;
}

} // namespace

void paintDockIcon(QPainter &painter, DockIcon icon, const QRectF &rect, const QColor &color, qreal strokeWidth)
{
	painter.save();
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.translate(rect.topLeft());
	painter.scale(rect.width() / 24.0, rect.height() / 24.0);
	QPen pen(color, strokeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
	painter.setPen(pen);
	painter.setBrush(Qt::NoBrush);
	painter.drawPath(pathFor(icon));
	painter.restore();
}

QIcon dockIcon(DockIcon icon, const QColor &color, int size)
{
	const qreal dpr = 2.0; // crisp on HiDPI, downscaled cleanly elsewhere
	QPixmap pixmap(QSize(size, size) * dpr);
	pixmap.setDevicePixelRatio(dpr);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	paintDockIcon(painter, icon, QRectF(0, 0, size, size), color);
	painter.end();
	return QIcon(pixmap);
}

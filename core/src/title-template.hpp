// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QString>

namespace videopress {

struct TitleContext {
	QString profile;
	QString scene;
	QDateTime recordedAt;
	QString fileBaseName; // fallback when the template renders to nothing
};

// Replaces {profile}, {scene}, {date} (yyyy-MM-dd) and {time} (HH:mm); unknown tokens are
// left as typed. Whitespace is collapsed and stray separators trimmed.
QString renderTitle(const QString &templ, const TitleContext &context);

} // namespace videopress

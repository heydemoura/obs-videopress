// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "title-template.hpp"

namespace videopress {

QString renderTitle(const QString &templ, const TitleContext &context)
{
	QString title = templ;
	title.replace(QLatin1String("{profile}"), context.profile);
	title.replace(QLatin1String("{scene}"), context.scene);
	if (context.recordedAt.isValid()) {
		title.replace(QLatin1String("{date}"), context.recordedAt.toString(QStringLiteral("yyyy-MM-dd")));
		title.replace(QLatin1String("{time}"), context.recordedAt.toString(QStringLiteral("HH:mm")));
	} else {
		title.replace(QLatin1String("{date}"), QString());
		title.replace(QLatin1String("{time}"), QString());
	}

	title = title.simplified();

	// An empty token can leave a dangling separator, e.g. "{scene} – {date}" with no scene.
	static const QString separators = QStringLiteral("-–—|:·,");
	while (!title.isEmpty() && separators.contains(title.front()))
		title = title.mid(1).trimmed();
	while (!title.isEmpty() && separators.contains(title.back())) {
		title.chop(1);
		title = title.trimmed();
	}

	return title.isEmpty() ? context.fileBaseName : title;
}

} // namespace videopress

// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPair>
#include <QTcpServer>
#include <QUrl>

#include <functional>

class QTcpSocket;

// Minimal HTTP/1.1 server for tests: keep-alive, Content-Length bodies, one handler.
struct MockRequest {
	QByteArray method;
	QByteArray path;
	QHash<QByteArray, QByteArray> headers; // lower-cased names
	QByteArray body;

	QByteArray header(const char *name) const { return headers.value(QByteArray(name).toLower()); }
};

struct MockResponse {
	int status = 200;
	QList<QPair<QByteArray, QByteArray>> headers;
	QByteArray body;

	MockResponse &withHeader(const QByteArray &name, const QByteArray &value)
	{
		headers.append({name, value});
		return *this;
	}
};

class MockHttpServer : public QObject {
	Q_OBJECT

public:
	using Handler = std::function<MockResponse(const MockRequest &)>;

	explicit MockHttpServer(QObject *parent = nullptr);
	~MockHttpServer() override;

	bool listen();
	QUrl baseUrl() const;
	void setHandler(Handler handler) { m_handler = std::move(handler); }
	const QList<MockRequest> &requests() const { return m_requests; }

private:
	void onReadyRead(QTcpSocket *socket);

	QTcpServer m_server;
	Handler m_handler;
	QHash<QTcpSocket *, QByteArray> m_buffers;
	QList<MockRequest> m_requests;
};

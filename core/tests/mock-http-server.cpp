// SPDX-FileCopyrightText: 2026 Automattic Inc. and obs-videopress contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mock-http-server.hpp"

#include <QHostAddress>
#include <QTcpSocket>

namespace {

QByteArray reasonPhrase(int status)
{
	switch (status) {
	case 200:
		return "OK";
	case 201:
		return "Created";
	case 204:
		return "No Content";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 409:
		return "Conflict";
	case 500:
		return "Internal Server Error";
	default:
		return "Status";
	}
}

} // namespace

MockHttpServer::MockHttpServer(QObject *parent) : QObject(parent)
{
	connect(&m_server, &QTcpServer::newConnection, this, [this] {
		while (QTcpSocket *socket = m_server.nextPendingConnection()) {
			connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
			connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
				m_buffers.remove(socket);
				socket->deleteLater();
			});
		}
	});
}

MockHttpServer::~MockHttpServer()
{
	// The sockets are children of m_server and emit disconnected() while it is destroyed,
	// after m_buffers is already gone.
	for (QTcpSocket *socket : m_server.findChildren<QTcpSocket *>())
		socket->disconnect(this);
}

bool MockHttpServer::listen()
{
	return m_server.listen(QHostAddress::LocalHost, 0);
}

QUrl MockHttpServer::baseUrl() const
{
	return QUrl(QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()));
}

void MockHttpServer::onReadyRead(QTcpSocket *socket)
{
	QByteArray &buffer = m_buffers[socket];
	buffer.append(socket->readAll());

	for (;;) {
		const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
		if (headerEnd < 0)
			return;

		MockRequest request;
		const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
		const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
		request.method = requestLine.value(0);
		request.path = requestLine.value(1);
		for (qsizetype i = 1; i < lines.size(); ++i) {
			const qsizetype colon = lines[i].indexOf(':');
			if (colon > 0)
				request.headers.insert(lines[i].left(colon).trimmed().toLower(),
						       lines[i].mid(colon + 1).trimmed());
		}

		const qsizetype contentLength = request.header("content-length").toLongLong();
		const qsizetype bodyStart = headerEnd + 4;
		if (buffer.size() < bodyStart + contentLength)
			return;
		request.body = buffer.mid(bodyStart, contentLength);
		buffer.remove(0, bodyStart + contentLength);

		m_requests.append(request);
		const MockResponse response = m_handler ? m_handler(request) : MockResponse{404, {}, {}};

		QByteArray out = "HTTP/1.1 " + QByteArray::number(response.status) + ' ' +
				 reasonPhrase(response.status) + "\r\n";
		for (const auto &header : response.headers)
			out += header.first + ": " + header.second + "\r\n";
		out += "Content-Length: " + QByteArray::number(request.method == "HEAD" ? 0 : response.body.size()) +
		       "\r\n";
		out += "Connection: keep-alive\r\n\r\n";
		if (request.method != "HEAD")
			out += response.body;
		socket->write(out);
	}
}

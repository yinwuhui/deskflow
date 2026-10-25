/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025-2026 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "IpcServer.h"

#include "base/Log.h"
#include "common/VersionInfo.h"

#include <QLocalServer>
#include <QLocalSocket>

namespace deskflow::core::ipc {

IpcServer::IpcServer(QObject *parent, const QString &serverName, const QString &typeName)
    : QObject(parent),
      m_server{new QLocalServer(this)}, // NOSONAR - Qt memory
      m_serverName(serverName),
      m_typeName(typeName.toUtf8())
{
  // do nothing
}

IpcServer::~IpcServer()
{
  m_server->close();
}

void IpcServer::listen()
{
  // IPC server normally runs as system, but GUI runs as regular user, so we need to allow world access.
  m_server->setSocketOptions(m_typeName == "core" ? QLocalServer::UserAccessOption : QLocalServer::WorldAccessOption);

  connect(m_server, &QLocalServer::newConnection, this, &IpcServer::handleNewConnection);
  QLocalServer::removeServer(m_serverName);
  if (m_server->listen(m_serverName)) {
    LOG_DEBUG("%s ipc server listening on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  } else {
    LOG_ERR("%s ipc server failed to listen on: %s", m_typeName.constData(), m_serverName.toUtf8().constData());
  }
}

void IpcServer::handleNewConnection()
{
  QLocalSocket *clientSocket = m_server->nextPendingConnection();
  if (!clientSocket) {
    LOG_ERR("%s ipc server failed to get new connection", m_typeName.constData());
    return;
  }

  LOG_DEBUG("%s ipc server got new connection", m_typeName.constData());
  m_clients.insert(clientSocket);

  connect(clientSocket, &QLocalSocket::readyRead, this, &IpcServer::handleReadyRead);
  connect(clientSocket, &QLocalSocket::disconnected, this, &IpcServer::handleDisconnected);
  connect(clientSocket, &QLocalSocket::errorOccurred, this, &IpcServer::handleErrorOccurred);
}

void IpcServer::handleReadyRead()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_VERBOSE("%s ipc server ready to read data", m_typeName.constData());

  auto &buffer = m_buffers[clientSocket];
  buffer += clientSocket->readAll();
  if (buffer.size() > 65536) {
    clientSocket->abort();
    return;
  }
  while (buffer.contains('\n')) {
    const auto index = buffer.indexOf('\n');
    const auto message = QString::fromUtf8(buffer.left(index));
    buffer.remove(0, index + 1);
    processMessage(clientSocket, message);
  }
}

void IpcServer::handleDisconnected()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  LOG_DEBUG("%s ipc server client disconnected", m_typeName.constData());
  m_clients.remove(clientSocket);
  m_buffers.remove(clientSocket);
  m_authenticated.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::handleErrorOccurred()
{
  const auto clientSocket = qobject_cast<QLocalSocket *>(sender());
  if (clientSocket->error() == QLocalSocket::PeerClosedError) {
    LOG_DEBUG("%s ipc server client closed connection", m_typeName.constData());
  } else {
    LOG_ERR("%s ipc server client error: %s", m_typeName.constData(), clientSocket->errorString().toUtf8().constData());
  }

  m_clients.remove(clientSocket);
  m_buffers.remove(clientSocket);
  m_authenticated.remove(clientSocket);
  clientSocket->deleteLater();
}

void IpcServer::processMessage(QLocalSocket *clientSocket, const QString &message)
{
  LOG_VERBOSE(
      "%s ipc server got message: %s", m_typeName.constData(),
      ((message.startsWith("transport") || message.startsWith("inputControl")) ? "transport [redacted]" : message.toUtf8().constData())
  );
  const auto split = message.indexOf('=');
  const auto parts = split < 0 ? QStringList{message} : QStringList{message.left(split), message.mid(split + 1)};
  if (parts.first() != "hello" && !m_authenticated.contains(clientSocket))
    return;
  if (parts.first() == "inputControl" && parts.size() == 2) {
    Q_EMIT inputControlCommand(parts[1]);
    return;
  }
  if (parts.first() == "transport" && parts.size() == 2) {
    Q_EMIT transportCommand(parts[1]);
    return;
  }
  if (parts.isEmpty()) {
    LOG_ERR(
        "%s ipc server got invalid message: %s", m_typeName.constData(),
        ((message.startsWith("transport") || message.startsWith("inputControl")) ? "transport [redacted]" : message.toUtf8().constData())
    );
    writeToClientSocket(clientSocket, QStringLiteral("error"));
    return;
  }

  if (const auto &command = parts.at(0); command == QStringLiteral("hello")) {
    if (parts.size() < 2) {
      LOG_ERR("%s ipc client hello missing version", m_typeName.constData());
      writeToClientSocket(clientSocket, "error=missing version");
      clientSocket->flush();
      clientSocket->disconnectFromServer();
      return;
    }

    const auto versionId = QStringLiteral("%1+%2").arg(kVersion, kVersionGitSha);
    const auto clientVersion = parts.at(1);
    LOG_DEBUG("%s ipc server got hello message (version: %s)", m_typeName.constData(), versionId.toUtf8().constData());

    if (clientVersion != versionId) {
      LOG_WARN(
          "%s ipc client version mismatch (client: %s, server: %s)", m_typeName.constData(),
          clientVersion.toUtf8().constData(), versionId.toUtf8().constData()
      );
      writeToClientSocket(clientSocket, QStringLiteral("versionMismatch=%1").arg(versionId));
      clientSocket->flush();
      return;
    }

    LOG_DEBUG("%s ipc server sending hello back", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("hello=%1").arg(versionId));

    m_authenticated.insert(clientSocket);
    Q_EMIT transportCommand(QStringLiteral("{\"kind\":\"snapshot\"}"));

    // Replay messages that were queued before any clients connected.
    LOG_VERBOSE("ipc server replaying %d pending messages", m_pendingMessages.size());
    for (const auto &pending : std::as_const(m_pendingMessages)) {
      LOG_VERBOSE("%s ipc server replaying: %s", m_typeName.constData(), pending.toUtf8().constData());
      writeToClientSocket(clientSocket, pending);
    }
    m_pendingMessages.clear();
  } else if (command == QStringLiteral("noop")) {
    LOG_DEBUG("%s ipc server got noop message", m_typeName.constData());
    writeToClientSocket(clientSocket, QStringLiteral("ok"));
  } else {
    processCommand(clientSocket, command, parts);
  }

  clientSocket->flush();
}

void IpcServer::broadcastCommand(const QString &command, const QString &args)
{
  Q_EMIT commandBroadcast(command, args);
  const auto message = args.isEmpty() ? command : QStringLiteral("%1=%2").arg(command, args);

  if (m_clients.isEmpty()) {
    LOG_VERBOSE(
        "%s ipc server has no clients, message queued: %s", m_typeName.constData(),
        ((message.startsWith("transport") || message.startsWith("inputControl")) ? "transport [redacted]" : message.toUtf8().constData())
    );
    if (command == "transportState") {
      for (int i = m_pendingMessages.size() - 1; i >= 0; --i)
        if (m_pendingMessages[i].startsWith("transportState="))
          m_pendingMessages.removeAt(i);
    }
    if (command == "transportSnapshot")
      m_pendingMessages.removeIf([](const QString &item) { return item.startsWith("transportSnapshot="); });
    if (m_pendingMessages.size() >= 128)
      m_pendingMessages.removeFirst();
    m_pendingMessages.append(message);
    return;
  }

  LOG_VERBOSE(
      "%s ipc server broadcasting message to %d clients: %s", m_typeName.constData(), m_clients.size(),
      ((message.startsWith("transport") || message.startsWith("inputControl")) ? "transport [redacted]" : message.toUtf8().constData())
  );
  for (auto *client : std::as_const(m_clients)) {
    writeToClientSocket(client, message);
    client->flush();
  }
}

void IpcServer::writeToClientSocket(QLocalSocket *&clientSocket, const QString &message) const
{
  QByteArray messageData = message.toUtf8() + '\n';
  qint64 bytesWritten = clientSocket->write(messageData);
  if (bytesWritten != messageData.size()) {
    LOG_ERR("%s ipc server failed to write full message to client socket", m_typeName.constData());
  } else {
    LOG_VERBOSE(
        "%s ipc server wrote message to client socket: %s", m_typeName.constData(),
        ((message.startsWith("transport") || message.startsWith("inputControl")) ? "transport [redacted]" : message.toUtf8().constData())
    );
  }
}

} // namespace deskflow::core::ipc

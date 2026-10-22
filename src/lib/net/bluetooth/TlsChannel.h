// SPDX-License-Identifier: MIT
#pragma once
#include <QIODevice>
#include <QTimer>
#include <functional>
#include <openssl/ssl.h>

// Mutual TLS over any reliable QIODevice (TCP or RFCOMM), pinned to one peer.
class TlsChannel : public QIODevice
{
  Q_OBJECT
public:
  static constexpr qint64 Limit = 256 * 1024;
  TlsChannel(QIODevice *wire, bool server, const QString &pem, const QByteArray &peerSha256, QObject *parent);
  ~TlsChannel() override;
  static bool validIdentity(const QString &pem);
  void setVerifier(std::function<bool(const QByteArray &)> verifier)
  {
    m_verifier = std::move(verifier);
  }
  QByteArray peerFingerprint() const
  {
    return m_peerFingerprint;
  }
  void start();
  void abort(const QString &reason);
  bool encrypted() const
  {
    return m_encrypted && !m_closed;
  }
  bool isSequential() const override
  {
    return true;
  }
  qint64 bytesAvailable() const override
  {
    return m_plain.size() + QIODevice::bytesAvailable();
  }
  qint64 bytesToWrite() const override;
Q_SIGNALS:
  void authenticated();
  void disconnected(const QString &reason);

protected:
  qint64 readData(char *data, qint64 maxSize) override;
  qint64 writeData(const char *data, qint64 size) override;

private:
  void drive();
  void drain();
  QIODevice *m_wire;
  SSL_CTX *m_context = nullptr;
  SSL *m_ssl = nullptr;
  QByteArray m_pin, m_plain, m_cipher, m_peerFingerprint;
  std::function<bool(const QByteArray &)> m_verifier;
  QTimer m_deadline;
  bool m_encrypted = false, m_closed = false, m_driving = false, m_resumeQueued = false;
};

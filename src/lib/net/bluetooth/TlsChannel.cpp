// SPDX-License-Identifier: MIT
#include "TlsChannel.h"
#include <QBluetoothSocket>
#include <QCryptographicHash>
#include <QTcpSocket>
#include <cstring>

TlsChannel::TlsChannel(QIODevice *wire, bool server, const QString &pem, const QByteArray &pin, QObject *parent)
    : QIODevice(parent),
      m_wire(wire),
      m_pin(pin)
{
  wire->setParent(this);
  m_context = SSL_CTX_new(TLS_method());
  const auto file = pem.toUtf8();
  if (m_context && SSL_CTX_set_min_proto_version(m_context, TLS1_2_VERSION) == 1 &&
      SSL_CTX_use_certificate_chain_file(m_context, file.constData()) == 1 &&
      SSL_CTX_use_PrivateKey_file(m_context, file.constData(), SSL_FILETYPE_PEM) == 1 &&
      SSL_CTX_check_private_key(m_context) == 1 && pin.size() == 32) {
    // Deskflow uses self-signed certificates. Identity is verified by SHA256 after the handshake.
    SSL_CTX_set_verify(m_context, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX *) {
      return 1;
    });
    SSL_CTX_set_options(m_context, SSL_OP_NO_RENEGOTIATION);
    m_ssl = SSL_new(m_context);
    if (m_ssl) {
      SSL_set_bio(m_ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
      SSL_set_mode(m_ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
      server ? SSL_set_accept_state(m_ssl) : SSL_set_connect_state(m_ssl);
    }
  }
  m_deadline.setSingleShot(true);
  m_deadline.setInterval(8000);
  connect(&m_deadline, &QTimer::timeout, this, [this] { abort("TLS handshake timeout"); });
  connect(wire, &QIODevice::readyRead, this, &TlsChannel::drive);
  connect(wire, &QIODevice::bytesWritten, this, [this] {
    drain();
    if (encrypted())
      Q_EMIT bytesWritten(0);
  });
  if (auto *tcp = qobject_cast<QTcpSocket *>(wire)) {
    tcp->setReadBufferSize(Limit);
    tcp->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    connect(tcp, &QTcpSocket::disconnected, this, [this] { abort("TCP disconnected"); });
    connect(tcp, &QTcpSocket::errorOccurred, this, [this, tcp] { abort(tcp->errorString()); });
  } else if (auto *bt = qobject_cast<QBluetoothSocket *>(wire)) {
    connect(bt, &QBluetoothSocket::disconnected, this, [this] { abort("Bluetooth disconnected"); });
    connect(bt, &QBluetoothSocket::errorOccurred, this, [this, bt] { abort(bt->errorString()); });
  }
}

TlsChannel::~TlsChannel()
{
  SSL_free(m_ssl);
  SSL_CTX_free(m_context);
}

void TlsChannel::start()
{
  if (!m_ssl) {
    abort("Cannot load local TLS identity or peer pin");
    return;
  }
  open(QIODevice::ReadWrite);
  m_deadline.start();
  drive();
}

void TlsChannel::abort(const QString &reason)
{
  if (m_closed)
    return;
  m_closed = true;
  m_deadline.stop();
  m_plain.clear();
  m_cipher.clear();
  QIODevice::close();
  if (auto *tcp = qobject_cast<QTcpSocket *>(m_wire))
    tcp->abort();
  else if (auto *bt = qobject_cast<QBluetoothSocket *>(m_wire))
    bt->abort();
  else
    m_wire->close();
  Q_EMIT disconnected(reason);
}

qint64 TlsChannel::bytesToWrite() const
{
  return m_cipher.size() + m_wire->bytesToWrite() + (m_ssl ? BIO_ctrl_pending(SSL_get_wbio(m_ssl)) : 0);
}

void TlsChannel::drain()
{
  if (m_closed || !m_ssl)
    return;
  char buffer[16384];
  while (BIO_ctrl_pending(SSL_get_wbio(m_ssl))) {
    const int n = BIO_read(SSL_get_wbio(m_ssl), buffer, sizeof buffer);
    if (n <= 0)
      break;
    m_cipher.append(buffer, n);
    if (m_cipher.size() + m_wire->bytesToWrite() > Limit) {
      abort("TLS send queue limit");
      return;
    }
  }
  while (!m_cipher.isEmpty() && !m_closed) {
    const auto n = m_wire->write(m_cipher);
    if (n < 0) {
      abort("Transport write failed");
      return;
    }
    if (!n)
      break;
    m_cipher.remove(0, n);
  }
}

void TlsChannel::drive()
{
  if (m_closed || !m_ssl || m_driving)
    return;
  m_driving = true;
  while (m_wire->bytesAvailable() && !m_closed) {
    const auto bytes = m_wire->read(16384);
    if (bytes.isEmpty())
      break;
    if (BIO_write(SSL_get_rbio(m_ssl), bytes.constData(), int(bytes.size())) != bytes.size()) {
      abort("TLS input failed");
      break;
    }
    // Process each bounded input chunk instead of accumulating a burst in the BIO.
    if (!SSL_is_init_finished(m_ssl)) {
      const int result = SSL_do_handshake(m_ssl);
      if (result != 1) {
        const int error = SSL_get_error(m_ssl, result);
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
          abort("TLS handshake failed");
      }
    }
    if (SSL_is_init_finished(m_ssl) && !m_closed) {
      if (!m_encrypted) {
        X509 *cert = SSL_get1_peer_certificate(m_ssl);
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int size = 0;
        const bool matches = cert && X509_digest(cert, EVP_sha256(), hash, &size) == 1 &&
                             QByteArray(reinterpret_cast<char *>(hash), size) == m_pin;
        X509_free(cert);
        if (!matches) {
          abort("Peer certificate fingerprint mismatch");
          break;
        }
        m_encrypted = true;
        m_deadline.stop();
        Q_EMIT authenticated();
      }
      char plain[16384];
      while (!m_closed) {
        const int n = SSL_read(m_ssl, plain, sizeof plain);
        if (n <= 0) {
          const int error = SSL_get_error(m_ssl, n);
          if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            abort("TLS stream ended or failed");
          break;
        }
        m_plain.append(plain, n);
        if (m_plain.size() > Limit) {
          abort("TLS receive queue limit");
          break;
        }
        Q_EMIT readyRead();
      }
    }
    drain();
  }
  // The initiating side must send ClientHello before there is any input.
  if (!m_closed && !SSL_is_init_finished(m_ssl)) {
    const int n = SSL_do_handshake(m_ssl);
    const int error = n == 1 ? SSL_ERROR_NONE : SSL_get_error(m_ssl, n);
    if (error != SSL_ERROR_NONE && error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
      abort("TLS handshake failed");
  }
  drain();
  m_driving = false;
}

qint64 TlsChannel::readData(char *data, qint64 size)
{
  const auto n = qMin(size, qint64(m_plain.size()));
  std::memcpy(data, m_plain.constData(), size_t(n));
  m_plain.remove(0, n);
  return n;
}

qint64 TlsChannel::writeData(const char *data, qint64 size)
{
  if (!encrypted())
    return -1;
  if (bytesToWrite() > Limit / 2)
    return 0;
  const int n = SSL_write(m_ssl, data, int(qMin<qint64>(size, 16384)));
  if (n <= 0) {
    const int error = SSL_get_error(m_ssl, n);
    if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
      abort("TLS write failed");
    return m_closed ? -1 : 0;
  }
  drain();
  return m_closed ? -1 : n;
}

bool TlsChannel::validIdentity(const QString &pem)
{
  auto *context = SSL_CTX_new(TLS_method());
  if (!context)
    return false;
  const auto file = pem.toUtf8();
  const bool valid = SSL_CTX_use_certificate_chain_file(context, file.constData()) == 1 &&
                     SSL_CTX_use_PrivateKey_file(context, file.constData(), SSL_FILETYPE_PEM) == 1 &&
                     SSL_CTX_check_private_key(context) == 1;
  SSL_CTX_free(context);
  return valid;
}

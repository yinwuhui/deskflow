/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2012 Synergy App Ltd
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "VersionChecker.h"

#include "common/Settings.h"
#include "common/VersionInfo.h"

#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QVersionNumber>
#include <climits>
#include <tuple>

VersionChecker::VersionChecker(QObject *parent) : QObject(parent), m_network{new QNetworkAccessManager(this)}
{
  connect(m_network, &QNetworkAccessManager::finished, this, &VersionChecker::replyFinished, Qt::UniqueConnection);
}

void VersionChecker::checkLatest()
{
  if (m_checking)
    return;
  m_checking = true;
  const QString url = Settings::value(Settings::Gui::UpdateCheckUrl).toString();
  qDebug("checking for updates at: %s", qPrintable(url));
  auto request = QNetworkRequest(url);
  auto userAgent = QString("%1 %2 on %3").arg(kAppName, kVersion, QSysInfo::prettyProductName());
  request.setHeader(QNetworkRequest::UserAgentHeader, userAgent);
  request.setRawHeader("X-Deskflow-Version", kVersion);
  request.setRawHeader("X-Deskflow-Language", qPrintable(QLocale::system().name()));
  request.setTransferTimeout(15000);
  m_network->get(request);
}

void VersionChecker::replyFinished(QNetworkReply *reply)
{
  m_checking = false;
  reply->deleteLater();
  const auto httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
  if (reply->error() != QNetworkReply::NoError || httpStatus < 200 || httpStatus >= 300) {
    Q_EMIT checkFinished(false, {}, tr("更新检查失败，请稍后重试。"));
    return;
  }
  const auto payload = reply->read(257);
  const auto newestVersion = QString::fromUtf8(payload).trimmed();
  static const QRegularExpression versionPattern(QStringLiteral("^[0-9]{1,6}(?:\\.[0-9]{1,6}){2,3}(?:-[A-Za-z0-9.]+)?$")
  );
  if (payload.size() > 256 || !versionPattern.match(newestVersion).hasMatch()) {
    Q_EMIT checkFinished(false, {}, tr("更新服务返回了无法识别的版本信息。"));
    return;
  }
  if (compareVersions(kVersion, newestVersion) > 0) {
    Q_EMIT updateFound(newestVersion);
    Q_EMIT checkFinished(true, newestVersion, {});
  } else
    Q_EMIT checkFinished(true, {}, {});
}

int VersionChecker::getStageVersion(QString stage)
{
  const char *stableName = "stable";
  const char *rcName = "rc";
  const char *betaName = "beta";

  // use max int for stable so it's always the highest value.
  const int stableValue = INT_MAX;
  const int rcValue = 2;
  const int betaValue = 1;
  const int otherValue = 0;

  if (stage.isEmpty() || stage == stableName) {
    return stableValue;
  } else if (stage.startsWith(rcName, Qt::CaseInsensitive)) {
    static QRegularExpression re("^rc([0-9]+)$", QRegularExpression::CaseInsensitiveOption);
    auto match = re.match(stage);
    if (match.hasMatch()) {
      // return the rc value plus the rc number (e.g. 2 + 1)
      // this should be ok since stable is max int.
      return rcValue + match.captured(1).toInt();
    }
  } else if (stage == betaName) {
    return betaValue;
  }

  return otherValue;
}

int VersionChecker::compareVersions(const QString &left, const QString &right)
{
  const auto leftParts = left.split('-');
  const auto rightParts = right.split('-');
  const auto order = QVersionNumber::compare(
      QVersionNumber::fromString(rightParts.first()), QVersionNumber::fromString(leftParts.first())
  );
  if (order != 0)
    return order > 0 ? 1 : -1;
  const auto leftStage = getStageVersion(leftParts.value(1));
  const auto rightStage = getStageVersion(rightParts.value(1));
  return rightStage == leftStage ? 0 : rightStage > leftStage ? 1 : -1;
}

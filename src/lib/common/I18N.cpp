/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "I18N.h"

#include "common/Constants.h"
#include "common/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QList>
#include <QMap>
#include <QObject>
#include <QTranslator>

I18N *I18N::instance()
{
  static I18N m;
  return &m;
}

I18N::I18N(QObject *parent) : QObject{parent}
{
  const auto appDir = QCoreApplication::applicationDirPath();
  const auto homeDir = QDir::homePath();

  const QList<QDir> appTrDirs{
      {QStringLiteral("%1/%2").arg(appDir, QStringLiteral("translations"))},
      {QStringLiteral("%1/../translations").arg(appDir)},
      {QStringLiteral("%1/../Resources/translations").arg(appDir)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, kAppId)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, kAppId)},
      {QStringLiteral("/usr/local/share/%1/translations").arg(kAppId)},
      {QStringLiteral("/usr/share/%1/translations").arg(kAppId)}
  };
  const QStringList appTrFilter{QStringLiteral("%1*.qm").arg(kAppId)};

  for (const auto &dir : appTrDirs) {
    if (!dir.entryList(appTrFilter, QDir::Files, QDir::Name).isEmpty()) {
      m_appTrPath = dir.absolutePath();
      break;
    }
  }

  if (m_appTrPath.isEmpty()) {
    qInfo() << "no app translations found";
  }

  const auto qt = QStringLiteral("qt");
  const auto qt6 = QStringLiteral("qt6");

  const QList<QDir> qtTrDirs{
      {QStringLiteral("%1/%2").arg(appDir, QStringLiteral("translations"))},
      {QStringLiteral("%1/../Resources/translations").arg(appDir)},
      {QStringLiteral("%1/../qt-depends/translations").arg(appDir)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, qt6)},
      {QStringLiteral("%1/../share/%2/translations").arg(appDir, qt)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, qt6)},
      {QStringLiteral("%1/.local/share/%2/translations").arg(homeDir, qt)},
      {QStringLiteral("/usr/local/share/%2/translations").arg(qt6)},
      {QStringLiteral("/usr/local/share/%2/translations").arg(qt)},
      {QStringLiteral("/usr/share/%2/translations").arg(qt6)},
      {QStringLiteral("/usr/share/%2/translations").arg(qt)}
  };
  const QStringList qtTrFilter{QStringLiteral("qt_*.qm")};

  for (const auto &dir : qtTrDirs) {
    if (!dir.entryList(qtTrFilter, QDir::Files, QDir::Name).isEmpty()) {
      m_qtTrPath = dir.absolutePath();
      break;
    }
  }

  if (m_qtTrPath.isEmpty()) {
    qInfo() << "no qt translations found";
  }

  detectLanguages();

  loadLanguage(Settings::value(Settings::Core::Language).toString());
}

void I18N::loadLanguage(const QString &requested)
{
  for (const auto &translator : std::as_const(m_currentTranslations))
    QCoreApplication::removeTranslator(translator);
  qDeleteAll(m_currentTranslations);
  m_currentTranslations.clear();
  m_followSystem = requested.isEmpty();
  m_currentLang = requested;
  const auto install = [this](QTranslator *translator, bool loaded) {
    if (loaded) {
      m_currentTranslations.append(translator);
      QCoreApplication::installTranslator(translator);
    } else
      delete translator;
  };
  if (m_followSystem) {
    auto *appTranslator = new QTranslator(this);
    const bool loaded = appTranslator->load(QLocale(), kAppId, "_", m_appTrPath);
    m_currentLang = loaded ? m_nameMap.key(appTranslator->translate("i18n", "LocalizedName")) : QString();
    if (m_currentLang.isEmpty())
      m_currentLang = "en";
    install(appTranslator, loaded);
    auto *qtTranslator = new QTranslator(this);
    const bool qtLoaded = qtTranslator->load(QLocale(), "qt", "_", m_qtTrPath);
    install(qtTranslator, qtLoaded);
  } else {
    const auto paths = m_translations.value(requested);
    for (const auto &path : paths) {
      auto *translator = new QTranslator(this);
      const bool loaded = !path.isEmpty() && translator->load(path);
      install(translator, loaded);
    }
  }
}

QStringList I18N::detectedLanguages()
{
  return instance()->m_nameMap.values();
}

QString I18N::nativeTo639Name(QString nativeName)
{
  return instance()->m_nameMap.key(nativeName);
}

QString I18N::toNativeName(QString shortName)
{
  return instance()->m_nameMap.value(shortName);
}

QString I18N::currentLanguage()
{
  return instance()->m_currentLang;
}

void I18N::setLanguage(const QString &langName)
{
  auto *self = instance();
  if ((langName.isEmpty() && self->m_followSystem) ||
      (!langName.isEmpty() && !self->m_followSystem && langName == self->m_currentLang))
    return;
  if (!langName.isEmpty() && !self->m_translations.contains(langName))
    return;
  self->loadLanguage(langName);
  Settings::setValue(Settings::Core::Language, langName);
  Q_EMIT self->languageChanged(self->m_currentLang);
}

void I18N::reDetectLanguages()
{
  instance()->detectLanguages();
}

void I18N::detectLanguages()
{
  const auto oldList = m_translations;
  m_translations.clear();
  m_nameMap.clear();

  QStringList nameFilter = {QStringLiteral("%1_*.qm").arg(kAppId)};
  QMap<QString, QString> appTranslations;
  QStringList detectedLangCodes;
  QDir dir(m_appTrPath);
  QStringList langList = dir.entryList(nameFilter, QDir::Files, QDir::Name);

  for (const QString &translation : std::as_const(langList)) {
    QTranslator translator;
    std::ignore = translator.load(translation, dir.absolutePath());
    const auto longCode = translator.language();
    //: Replace with your Language name
    //: This is a required string
    QString nativeLang = translator.translate("i18n", "LocalizedName");
    if (nativeLang.isEmpty())
      nativeLang = QStringLiteral("English");

    QString shortCode;
    if (longCode.startsWith(QStringLiteral("zh")) || longCode.startsWith(QStringLiteral("pt")))
      shortCode = longCode;
    else
      shortCode = longCode.mid(0, 2);

    appTranslations.insert(shortCode, translator.filePath());
    m_nameMap.insert(shortCode, nativeLang);
    detectedLangCodes.append(QStringLiteral("qt_%1.qm").arg(shortCode));
  }

  dir.setPath(m_qtTrPath);
  const static auto qtTrNameLen = 3; // length of qt_
  langList = dir.entryList(detectedLangCodes, QDir::Files, QDir::Name);

  QMap<QString, QString> qtTranslations;
  for (const QString &translation : std::as_const(langList)) {
    QString lang = translation.mid(qtTrNameLen, translation.lastIndexOf(QLatin1Char('.')) - qtTrNameLen);
    qtTranslations.insert(lang, QStringLiteral("%1/%2").arg(m_qtTrPath, translation));
  }

  const QStringList keys = appTranslations.keys();
  for (const QString &lang : keys)
    m_translations.insert(lang, {appTranslations.value(lang), qtTranslations.value(lang)});

  if (oldList != m_translations)
    Q_EMIT languagesChanged(m_translations.keys());
}

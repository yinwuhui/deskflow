// SPDX-License-Identifier: MIT
#pragma once
#include "common/Settings.h"
#include <QDir>
#include <QFileInfo>
#include <QSettings>
// Durable interlocks, never display authorization or a reconnect token. Each
// caller owns its QSettings instance; GUI/core threads do not share one.
namespace UsageInputState {
inline QString filePath(){return QFileInfo(Settings::settingsFile()).dir().filePath("usage-input.ini");}
inline bool displayOwned(const QString& file={}){
  QSettings settings(file.isEmpty()?filePath():file,QSettings::IniFormat);settings.sync();settings.beginGroup("displayOwns");
  for(const auto& key:settings.childKeys())if(settings.value(key).toBool())return true;return false;
}
inline bool setDisplayOwned(const QString& peer,bool owned,const QString& file={}){
  QSettings settings(file.isEmpty()?filePath():file,QSettings::IniFormat);if(owned)settings.setValue("displayOwns/"+peer,true);else settings.remove("displayOwns/"+peer);settings.sync();return settings.status()==QSettings::NoError;
}
inline bool setPaused(const QString& peer,bool paused){
  QSettings settings(filePath(),QSettings::IniFormat);settings.setValue(peer,paused);settings.sync();return settings.status()==QSettings::NoError;
}
}

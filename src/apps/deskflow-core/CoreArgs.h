/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QCommandLineOption>
/**
 * @brief The CoreArgs class
 * This class contains args for the CoreArgParser
 */
struct CoreArgs
{
  inline static const auto helpOption = QCommandLineOption({"h", "help"}, "Display Help on the command line");
  inline static const auto versionOption = QCommandLineOption({"v", "version"}, "Display version information");
  inline static const auto multiInstanceOption =
      QCommandLineOption("new-instance", "Skip the check for a running instance, always makes a new instance");
  inline static const auto configOption =
      QCommandLineOption({"s", "settings"}, "override configuration file to use", "configFile");

  inline static const auto autoFailoverOption = QCommandLineOption(
      "auto-failover", "Keep network and Bluetooth ready; automatically reconnect on the other channel"
  );
  inline static const auto bluetoothPeerOption = QCommandLineOption(
      "bluetooth-peer", "Use RFCOMM to connect to this Bluetooth peer (server: allowed peer)", "address"
  );
  inline static const auto bluetoothScanOption =
      QCommandLineOption("bluetooth-scan", "Scan classic Bluetooth devices, then exit (no coremode required)");
  inline static const auto bluetoothServicesOption =
      QCommandLineOption("bluetooth-services", "Find Deskflow RFCOMM services on a peer, then exit", "address");
  inline static const auto options = {helpOption,          versionOption,          multiInstanceOption,
                                      configOption,        autoFailoverOption,     bluetoothPeerOption,
                                      bluetoothScanOption, bluetoothServicesOption};
};

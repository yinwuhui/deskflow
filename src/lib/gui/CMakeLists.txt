# SPDX-FileCopyrightText: (C) 2024 - 2026 Deskflow Developers
# SPDX-FileCopyrightText: (C) 2024 Symless Ltd
# SPDX-License-Identifier: MIT

set(target gui)

set(CMAKE_INCLUDE_CURRENT_DIR ON)

if(APPLE)
  set (platform_sources
    OSXHelpers.h
    OSXHelpers.mm
  )
endif()

add_library(${target} STATIC
  ${platform_sources}
  ComputerSetupModel.cpp
  ComputerSetupModel.h
  Diagnostic.cpp
  Diagnostic.h
  FileTail.cpp
  FileTail.h
  Logger.cpp
  Logger.h
  LoginStartup.cpp
  LoginStartup.h
  MainWindow.cpp
  MainWindow.h
  MainWindow.ui
  Messages.cpp
  Messages.h
  StyleUtils.h
  TlsUtility.cpp
  TlsUtility.h
  VersionChecker.cpp
  VersionChecker.h
  config/ComputerConfig.h
  config/Computer.cpp
  config/Computer.h
  config/ComputerList.cpp
  config/ComputerList.h
  config/ServerConfig.cpp
  config/ServerConfig.h
  config/ScreenLayout.cpp
  config/ScreenLayout.h
  widgets/ScreenLayoutPage.cpp
  widgets/ScreenLayoutPage.h
  widgets/DeviceListPage.cpp
  widgets/DeviceListPage.h
  core/PeerUsageModel.cpp
  core/PeerUsageModel.h
  widgets/PeerUsageControl.cpp
  widgets/PeerUsageControl.h
  widgets/DeviceScreenInfo.cpp
  widgets/DeviceScreenInfo.h
  widgets/DeviceConnectionInfo.cpp
  widgets/DeviceConnectionInfo.h
  widgets/DisplayRequestPanel.cpp
  widgets/DisplayRequestPanel.h
  core/CoreProcess.cpp
  core/CoreProcess.h
  core/NetworkMonitor.cpp
  core/NetworkMonitor.h
  dialogs/AboutDialog.cpp
  dialogs/AboutDialog.h
  dialogs/AboutDialog.ui
  dialogs/ActionDialog.cpp
  dialogs/ActionDialog.h
  dialogs/ActionDialog.ui
  dialogs/ClientConfigDialog.h
  dialogs/ClientConfigDialog.cpp
  dialogs/ClientConfigDialog.ui
  dialogs/ComputerSettingsDialog.cpp
  dialogs/ComputerSettingsDialog.h
  dialogs/ComputerSettingsDialog.ui
  dialogs/FingerprintDialog.cpp
  dialogs/FingerprintDialog.h
  dialogs/HelpDialog.cpp
  dialogs/HelpDialog.h
  dialogs/HotkeyDialog.cpp
  dialogs/HotkeyDialog.h
  dialogs/HotkeyDialog.ui
  dialogs/ServerConfigDialog.cpp
  dialogs/ServerConfigDialog.h
  dialogs/ServerConfigDialog.ui
  dialogs/ServerBrowserDialog.cpp
  dialogs/ServerBrowserDialog.h
  dialogs/SettingsDialog.cpp
  dialogs/SettingsDialog.h
  dialogs/SettingsDialog.ui
  ipc/IpcClient.cpp
  ipc/IpcClient.h
  ipc/DaemonIpcClient.cpp
  ipc/DaemonIpcClient.h
  ipc/CoreIpcClient.cpp
  ipc/CoreIpcClient.h
  validators/AliasValidator.cpp
  validators/AliasValidator.h
  validators/ComputerDuplicationsValidator.cpp
  validators/ComputerDuplicationsValidator.h
  validators/ComputerLineNameValidator.cpp
  validators/ComputerLineNameValidator.h
  validators/ComputerNameValidator.cpp
  validators/ComputerNameValidator.h
  validators/EmptyStringValidator.cpp
  validators/EmptyStringValidator.h
  validators/IStringValidator.cpp
  validators/IStringValidator.h
  validators/LineEditValidator.cpp
  validators/LineEditValidator.h
  validators/SpacesValidator.cpp
  validators/SpacesValidator.h
  validators/ValidationError.cpp
  validators/ValidationError.h
  validators/IpAddressValidator.cpp
  validators/IpAddressValidator.h
  widgets/ComputerSetupView.cpp
  widgets/ComputerSetupView.h
  widgets/HomeIcons.h
  widgets/HomeCheckBox.h
  widgets/HomeComboBox.h
  widgets/BrandIcons.h
  widgets/HomeModeButton.h
  widgets/MacWindowControls.h
  widgets/HomeDeviceList.h
  widgets/FingerprintPreview.cpp
  widgets/FingerprintPreview.h
  widgets/KeySequenceWidget.cpp
  widgets/KeySequenceWidget.h
  widgets/LogDock.cpp
  widgets/LogDock.h
  widgets/LogWidget.h
  widgets/LogWidget.cpp
  widgets/NewComputerWidget.cpp
  widgets/NewComputerWidget.h
  widgets/RemoveComputerWidget.cpp
  widgets/RemoveComputerWidget.h
  widgets/SearchWidget.h
  widgets/SearchWidget.cpp
  widgets/StatusBar.cpp
  widgets/StatusBar.h
  widgets/SettingsPage.cpp
  widgets/SettingsPage.h
  widgets/SettingsDialogButtonBox.cpp
  widgets/SettingsDialogButtonBox.h
)

target_link_libraries(${target}
  PUBLIC
    common
    platform
    net
    Qt6::Widgets
)

target_sources(gui PRIVATE core/TransportNotifications.cpp core/TransportNotifications.h)

target_sources(gui PRIVATE core/BackupConnectionPanel.cpp core/BackupConnectionPanel.h
  core/NearbyBluetoothModel.cpp core/NearbyBluetoothModel.h widgets/DiscoveryComboBox.h widgets/DiscoveryListView.h)
target_sources(gui PRIVATE widgets/LocalBluetoothInfo.cpp widgets/LocalBluetoothInfo.h)
if(BUILD_BLUETOOTH)
  target_link_libraries(gui PUBLIC deskflow-bluetooth)
  target_compile_definitions(gui PRIVATE DESKFLOW_BLUETOOTH)
  if(APPLE)
    target_link_libraries(gui PRIVATE Qt6::QDarwinBluetoothPermissionPlugin)
  endif()
endif()

if(BUILD_VIRTUAL_DISPLAY)
  target_link_libraries(gui PUBLIC deskflow-display-service)
  target_compile_definitions(gui PUBLIC DESKFLOW_DISPLAY)
endif()

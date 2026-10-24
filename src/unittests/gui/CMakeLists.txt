# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

add_subdirectory(config)
add_subdirectory(core)

create_test(
  NAME IconTests
  DEPENDS gui
  SOURCE IconTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(IconTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_sources(IconTests PRIVATE ../../apps/res/deskflow.qrc)

create_test(
  NAME DeviceListPageTests
  DEPENDS gui
  SOURCE DeviceListPageTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(DeviceListPageTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_sources(DeviceListPageTests PRIVATE ../../apps/res/deskflow.qrc)

create_test(
  NAME MainWindowHomeTests
  DEPENDS gui
  SOURCE MainWindowHomeTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(MainWindowHomeTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_sources(MainWindowHomeTests PRIVATE ../../apps/res/deskflow.qrc)

foreach(scale IN ITEMS 1 1.25 1.5 2)
  add_test(NAME FixedWindowScale${scale} COMMAND MainWindowHomeTests fixedWindowRejectsResizeAndMaximize)
  set_tests_properties(FixedWindowScale${scale} PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_SCALE_FACTOR=${scale}")
endforeach()

create_test(
  NAME SettingsPageTests
  DEPENDS gui
  SOURCE SettingsPageTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(SettingsPageTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_sources(SettingsPageTests PRIVATE ../../apps/res/deskflow.qrc)

create_test(
  NAME ScreenLayoutTests
  DEPENDS gui
  LIBS server base arch
  SOURCE ScreenLayoutTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
set_tests_properties(ScreenLayoutTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_sources(ScreenLayoutTests PRIVATE ../../apps/res/deskflow.qrc)

create_test(
  NAME LoggerTests
  DEPENDS gui
  SOURCE LoggerTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

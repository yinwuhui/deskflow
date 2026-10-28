# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
add_library(deskflow-display-infra-core STATIC
  core/InfrastructureTypes.cpp
  frame/FrameModel.cpp
  core/InfrastructureSession.cpp)
target_compile_features(deskflow-display-infra-core PUBLIC cxx_std_20)
target_include_directories(deskflow-display-infra-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
  target_compile_options(deskflow-display-infra-core PRIVATE /W4)
else()
  target_compile_options(deskflow-display-infra-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(deskflow-display STATIC Protocol.cpp Session.cpp Coordinator.cpp UsageCoordinator.cpp)
target_link_libraries(deskflow-display PUBLIC deskflow-display-infra-core)
target_compile_features(deskflow-display PUBLIC cxx_std_20)
target_include_directories(deskflow-display PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
if(MSVC)
  target_compile_options(deskflow-display PRIVATE /W4)
else()
  target_compile_options(deskflow-display PRIVATE -Wall -Wextra -Wpedantic)
endif()

# The core remains independently buildable without Qt. The product facade is
# linked into the application only when BUILD_VIRTUAL_DISPLAY is enabled.
if(TARGET Qt6::Core)
  add_library(deskflow-display-infra-runtime STATIC
    control/InfrastructureControl.cpp
    control/ControlAdmission.cpp
    supervision/WorkerProtocol.cpp
    supervision/WorkerProcess.cpp
    supervision/NativeProcessIdentity.cpp
    supervision/MediaWorkerContracts.cpp
    supervision/PlatformWorkerContracts.cpp
    resource/DurableResourceLedger.cpp
    resource/ResourceExecutor.cpp)
  target_link_libraries(deskflow-display-infra-runtime PUBLIC deskflow-display-infra-core Qt6::Core Qt6::Network)
  if(WIN32)
    target_link_libraries(deskflow-display-infra-runtime PRIVATE advapi32)
  endif()
  add_library(deskflow-display-service STATIC ScreenInventoryService.cpp ScreenInventoryService.h ProductDisplayRuntime.cpp ProductDisplayRuntime.h CoreInputChannel.cpp CoreInputChannel.h InputRouteAdapter.cpp InputRouteAdapter.h UsageDisplayBridge.cpp UsageDisplayBridge.h PeerUsageService.cpp PeerUsageService.h DisplayService.cpp DisplayService.h ResourceJournal.cpp ResourceJournal.h MediaSupervisor.cpp MediaSupervisor.h)
  target_link_libraries(deskflow-display-service PUBLIC deskflow-display deskflow-display-infra-runtime Qt6::Core Qt6::Network Qt6::Gui PRIVATE common)
  if(APPLE)
    target_link_libraries(deskflow-display-service PRIVATE "-framework CoreGraphics")
  endif()
  if(TARGET Qt6::DBus)
    target_link_libraries(deskflow-display-service PRIVATE Qt6::DBus)
    target_compile_definitions(deskflow-display-service PRIVATE DESKFLOW_SCREEN_DBUS)
  endif()
  set_target_properties(deskflow-display-service PROPERTIES AUTOMOC ON)
  target_compile_definitions(deskflow-display-service PRIVATE DESKFLOW_DISPLAY_RUNTIME_SOURCE="${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype")
  install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/display_session.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/display_transport.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/h264_access_units.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/virtual_display_probe.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/product_probe.py"
    DESTINATION share/busymouse/display)
endif()

if(TARGET Qt6::Widgets)
  add_executable(busymouse-display-viewer DisplayViewer.cpp)
  target_link_libraries(busymouse-display-viewer PRIVATE Qt6::Widgets Qt6::Network)
  install(TARGETS busymouse-display-viewer RUNTIME DESTINATION bin)
endif()

if(APPLE AND TARGET Qt6::Core)
  enable_language(OBJCXX)
  add_executable(busymouse-display-probe "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/macos/VirtualDisplayProbe.mm")
  set_target_properties(busymouse-display-probe PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(busymouse-display-probe PRIVATE -fobjc-arc)
  target_link_libraries(busymouse-display-probe PRIVATE "-framework IOKit" "-framework Cocoa" "-framework CoreGraphics" "-framework CoreMedia" "-framework CoreVideo" "-framework ScreenCaptureKit" "-framework VideoToolbox")
  install(TARGETS busymouse-display-probe RUNTIME DESTINATION bin)
endif()

if(APPLE AND BUILD_OSX_BUNDLE AND TARGET busymouse-display-probe)
  foreach(helper busymouse-display-probe busymouse-display-viewer)
    set_target_properties(${helper} PROPERTIES BUILD_WITH_INSTALL_RPATH TRUE
      INSTALL_RPATH "@loader_path/../Libraries;@loader_path/../Frameworks"
      RUNTIME_OUTPUT_DIRECTORY $<TARGET_BUNDLE_CONTENT_DIR:${CMAKE_PROJECT_PROPER_NAME}>/MacOS)
    if(APPLE_CODESIGN_DEV)
      configure_mac_codesign(${helper})
    endif()
  endforeach()
endif()

if(APPLE AND TARGET deskflow-display-infra-runtime)
  add_library(deskflow-display-platform-macos STATIC platform/macos/MacVirtualOutput.mm platform/macos/MacResourceProvider.cpp)
  target_link_libraries(deskflow-display-platform-macos PUBLIC deskflow-display-infra-runtime PRIVATE
    "-framework Cocoa" "-framework CoreGraphics" "-framework IOKit" "-framework Security")
  target_include_directories(deskflow-display-platform-macos PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/macos")
  set_target_properties(deskflow-display-platform-macos PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(deskflow-display-platform-macos PRIVATE -fobjc-arc)
  add_executable(busymouse-display-platform-helper platform/macos/MacPlatformHelper.cpp)
  target_link_libraries(busymouse-display-platform-helper PRIVATE deskflow-display-platform-macos)
  install(TARGETS busymouse-display-platform-helper RUNTIME DESTINATION bin)
endif()

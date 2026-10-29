# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
add_library(deskflow-display-infra-core STATIC
  core/InfrastructureTypes.cpp
  frame/FrameModel.cpp
  core/InfrastructureSession.cpp
  core/SessionMailbox.cpp)
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
    control/ControlBodyRegistry.cpp
    supervision/WorkerProtocol.cpp
    supervision/WorkerProcess.cpp
    supervision/NativeProcessIdentity.cpp
    supervision/MediaWorkerContracts.cpp
    supervision/MediaWorkPermit.cpp
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
  add_library(deskflow-display-platform-macos STATIC platform/macos/MacVirtualOutput.mm platform/macos/MacResourceProvider.cpp platform/macos/MacCaptureSource.mm)
  target_link_libraries(deskflow-display-platform-macos PUBLIC deskflow-display-infra-runtime PRIVATE
    "-framework Cocoa" "-framework CoreGraphics" "-framework IOKit" "-framework Security"
    "-framework ScreenCaptureKit" "-framework CoreMedia" "-framework CoreVideo")
  target_include_directories(deskflow-display-platform-macos PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/virtual-display-prototype/macos")
  set_target_properties(deskflow-display-platform-macos PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_options(deskflow-display-platform-macos PRIVATE -fobjc-arc)
  add_executable(busymouse-display-platform-helper platform/macos/MacPlatformHelper.cpp)
  target_link_libraries(busymouse-display-platform-helper PRIVATE deskflow-display-platform-macos)
  install(TARGETS busymouse-display-platform-helper RUNTIME DESTINATION bin)
endif()

# Explicit developer SDK opt-in. No profile is published available by this target.
set(DESKFLOW_GSTREAMER_SDK "" CACHE PATH "Explicit GStreamer SDK prefix for native display engine development")
set(DESKFLOW_GSTREAMER_RUNTIME "" CACHE PATH "Private copied runtime used by native display engine tests")
if(DESKFLOW_GSTREAMER_SDK AND TARGET deskflow-display-infra-runtime AND NOT WIN32)
  if(NOT IS_DIRECTORY "${DESKFLOW_GSTREAMER_RUNTIME}/lib/gstreamer-1.0")
    message(FATAL_ERROR "Native display engine requires an explicit private runtime copy")
  endif()
  find_package(PkgConfig REQUIRED)
  set(display_saved_pkg_path "$ENV{PKG_CONFIG_PATH}")
  file(GLOB display_pc_arch_dirs "${DESKFLOW_GSTREAMER_SDK}/lib/*/pkgconfig")
  set(display_pc_dirs "${DESKFLOW_GSTREAMER_SDK}/lib/pkgconfig" ${display_pc_arch_dirs})
  list(JOIN display_pc_dirs ":" display_pc_path)
  set(ENV{PKG_CONFIG_PATH} "${display_pc_path}")
  pkg_check_modules(DISPLAY_GST REQUIRED IMPORTED_TARGET gstreamer-app-1.0 gstreamer-video-1.0 gstreamer-rtp-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-pbutils-1.0)
  set(ENV{PKG_CONFIG_PATH} "${display_saved_pkg_path}")
  # libnice's public headers need GLib only; distro nice.pc also requires
  # unrelated static/UPnP development packages. Bind the explicit shared SDK
  # library without weakening or rewriting its dependency metadata.
  file(GLOB display_sdk_arch_lib_dirs "${DESKFLOW_GSTREAMER_SDK}/lib/*-linux-gnu")
  find_path(DISPLAY_NICE_INCLUDE nice/agent.h PATHS "${DESKFLOW_GSTREAMER_SDK}/include" NO_DEFAULT_PATH REQUIRED)
  find_library(DISPLAY_NICE_LIBRARY nice PATHS "${DESKFLOW_GSTREAMER_SDK}/lib" ${display_sdk_arch_lib_dirs} NO_DEFAULT_PATH REQUIRED)
  # The SDK .pc files inject a source-directory rpath ahead of BUILD_RPATH.
  # Remove it so developer tests load the explicit private runtime copy first.
  get_target_property(display_gst_link_options PkgConfig::DISPLAY_GST INTERFACE_LINK_OPTIONS)
  if(display_gst_link_options)
    list(FILTER display_gst_link_options EXCLUDE REGEX "^-Wl,-rpath,")
    set_property(TARGET PkgConfig::DISPLAY_GST PROPERTY INTERFACE_LINK_OPTIONS "${display_gst_link_options}")
  endif()
  if(APPLE)
    set(display_gst_suffix dylib)
  else()
    set(display_gst_suffix so)
  endif()
  file(GLOB display_arch_plugin_dirs "${DESKFLOW_GSTREAMER_SDK}/lib/*/gstreamer-1.0")
  set(display_plugin_dirs "${DESKFLOW_GSTREAMER_SDK}/lib/gstreamer-1.0" ${display_arch_plugin_dirs})
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/gstreamer")
  set(display_manifest "#pragma once\nstruct GStreamerBuildPlugin { const char *file; const char *sha256; };\ninline constexpr GStreamerBuildPlugin gstreamerBuildPlugins[] = {\n")
  foreach(plugin coreelements debugutilsbad typefindfunctions app videoconvertscale videorate videoparsersbad x264 libav rtp rtpmanager rtpmanagerbad nice dtls srtp webrtc rswebrtc rsrtp)
    set(plugin_file "libgst${plugin}.${display_gst_suffix}")
    unset(display_plugin_source)
    foreach(directory IN LISTS display_plugin_dirs)
      if(EXISTS "${directory}/${plugin_file}")
        set(display_plugin_source "${directory}/${plugin_file}")
        break()
      endif()
    endforeach()
    if(NOT display_plugin_source)
      message(FATAL_ERROR "Required native display plugin missing: ${plugin_file}")
    endif()
    file(SHA256 "${display_plugin_source}" plugin_sha)
    string(APPEND display_manifest "{\"${plugin_file}\",\"${plugin_sha}\"},\n")
  endforeach()
  string(APPEND display_manifest "};\n")
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gstreamer/GStreamerBuildManifest.h" "${display_manifest}")
  add_library(deskflow-display-native-webrtc STATIC media/gstreamer/NativeWebRtcEngine.cpp)
  target_link_libraries(deskflow-display-native-webrtc PUBLIC deskflow-display-infra-runtime PRIVATE PkgConfig::DISPLAY_GST "${DISPLAY_NICE_LIBRARY}" ${CMAKE_DL_LIBS})
  target_include_directories(deskflow-display-native-webrtc PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gstreamer" "${DISPLAY_NICE_INCLUDE}")
  target_compile_definitions(deskflow-display-native-webrtc PRIVATE GST_USE_UNSTABLE_API)
  add_executable(NativeWebRtcLoopback "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/display-infrastructure-lab/NativeWebRtcLoopback.cpp")
  target_link_libraries(NativeWebRtcLoopback PRIVATE deskflow-display-native-webrtc PkgConfig::DISPLAY_GST)
  set_target_properties(NativeWebRtcLoopback PROPERTIES BUILD_RPATH "${DESKFLOW_GSTREAMER_RUNTIME}/lib")
  if(UNIX AND NOT APPLE)
    # The private SDK has transitive shared dependencies outside system paths.
    target_link_options(NativeWebRtcLoopback PRIVATE "-Wl,--disable-new-dtags")
  endif()
endif()
if(TARGET deskflow-display-native-webrtc AND TARGET deskflow-display-platform-macos AND TARGET Qt6::Widgets)
  add_executable(NativeMacWebRtcLoopback "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/display-infrastructure-lab/NativeMacWebRtcLoopback.mm")
  target_link_libraries(NativeMacWebRtcLoopback PRIVATE deskflow-display-native-webrtc deskflow-display-platform-macos Qt6::Widgets "-framework AppKit")
  target_compile_options(NativeMacWebRtcLoopback PRIVATE -fobjc-arc)
  set_target_properties(NativeMacWebRtcLoopback PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON BUILD_RPATH "${DESKFLOW_GSTREAMER_RUNTIME}/lib")
endif()

# Explicit private SDK developer build only; no production capability publication.
if(TARGET deskflow-display-native-webrtc AND TARGET Qt6::Widgets)
  add_executable(busymouse-display-media-receiver ../../apps/display-media-worker/Receiver.cpp)
  target_link_libraries(busymouse-display-media-receiver PRIVATE deskflow-display-native-webrtc Qt6::Widgets)
  target_compile_definitions(busymouse-display-media-receiver PRIVATE DISPLAY_PRIVATE_RUNTIME="${DESKFLOW_GSTREAMER_RUNTIME}")
  set_target_properties(busymouse-display-media-receiver PROPERTIES BUILD_RPATH "${DESKFLOW_GSTREAMER_RUNTIME}/lib")
  if(UNIX AND NOT APPLE)
    target_link_options(busymouse-display-media-receiver PRIVATE "-Wl,--disable-new-dtags")
  endif()
endif()
if(TARGET deskflow-display-native-webrtc)
  add_executable(NativeReceiverProcessSmoke ../../../tools/display-infrastructure-lab/NativeReceiverProcessSmoke.cpp)
  target_link_libraries(NativeReceiverProcessSmoke PRIVATE deskflow-display-native-webrtc)
  set_target_properties(NativeReceiverProcessSmoke PROPERTIES BUILD_RPATH "${DESKFLOW_GSTREAMER_RUNTIME}/lib")
  if(UNIX AND NOT APPLE)
    target_link_options(NativeReceiverProcessSmoke PRIVATE "-Wl,--disable-new-dtags")
  endif()
endif()

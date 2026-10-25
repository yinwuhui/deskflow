# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
add_library(deskflow-display STATIC Protocol.cpp Session.cpp Coordinator.cpp UsageCoordinator.cpp)
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
  add_library(deskflow-display-service STATIC CoreInputChannel.cpp CoreInputChannel.h InputRouteAdapter.cpp InputRouteAdapter.h UsageDisplayBridge.cpp UsageDisplayBridge.h PeerUsageService.cpp PeerUsageService.h DisplayService.cpp DisplayService.h ResourceJournal.cpp ResourceJournal.h MediaSupervisor.cpp MediaSupervisor.h)
  target_link_libraries(deskflow-display-service PUBLIC deskflow-display Qt6::Core Qt6::Network PRIVATE common)
  set_target_properties(deskflow-display-service PROPERTIES AUTOMOC ON)
endif()

if(TARGET Qt6::Widgets)
  add_executable(busymouse-display-viewer DisplayViewer.cpp)
  target_link_libraries(busymouse-display-viewer PRIVATE Qt6::Widgets)
  install(TARGETS busymouse-display-viewer RUNTIME DESTINATION bin)
endif()

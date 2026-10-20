# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

create_test(
  NAME NetworkMonitorTests
  DEPENDS gui
  SOURCE NetworkMonitorTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME ApplicationStateTests
  DEPENDS gui
  SOURCE ApplicationStateTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME ConfigurationBackupTests
  DEPENDS gui
  SOURCE ConfigurationBackupTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME IdentityTests
  DEPENDS gui
  SOURCE IdentityTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
create_test(
  NAME CoreValidationTests
  DEPENDS gui
  LIBS server
  SOURCE CoreValidationTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)
target_compile_definitions(CoreValidationTests PRIVATE DESKFLOW_TEST_CORE_PATH="$<TARGET_FILE:deskflow-core>")
add_dependencies(CoreValidationTests deskflow-core)

create_test(
  NAME MaintenanceTests
  DEPENDS gui
  SOURCE MaintenanceTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME SourceChangeTransactionTests
  DEPENDS gui
  SOURCE SourceChangeTransactionTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

create_test(
  NAME LayoutMigrationTests
  DEPENDS gui
  SOURCE LayoutMigrationTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/gui"
)

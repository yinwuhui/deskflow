# SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
# SPDX-License-Identifier: MIT

if(WIN32)
  set(extra_libs version)
endif()

create_test(
  NAME SecureUtilsTests
  DEPENDS net
  LIBS base arch mt io ${extra_libs}
  SOURCE SecureUtilsTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
)

create_test(
  NAME FingerprintTests
  DEPENDS net
  LIBS base arch mt io ${extra_libs}
  SOURCE FingerprintTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
)

create_test(
  NAME FingerprintDatabaseTests
  DEPENDS net
  LIBS base arch mt io ${extra_libs}
  SOURCE FingerprintDatabaseTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
)



create_test(
  NAME MdnsLifecycleTests
  DEPENDS net
  LIBS base arch mt io ${extra_libs}
  SOURCE MdnsLifecycleTests.cpp
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
)

if(BUILD_BLUETOOTH)
  create_test(
    NAME RfcommBackendTests
    DEPENDS deskflow-bluetooth
    SOURCE "${CMAKE_SOURCE_DIR}/tools/rfcomm-prototype/tests.cpp"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net"
  )
endif()

if(BUILD_BLUETOOTH)
  create_test(NAME FailoverTests DEPENDS deskflow-bluetooth
    SOURCE "${CMAKE_SOURCE_DIR}/tools/rfcomm-prototype/FailoverTests.cpp"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net")
endif()

if(BUILD_BLUETOOTH)
  create_test(NAME ConnectionManagerTests DEPENDS deskflow-bluetooth
    SOURCE ConnectionManagerTests.cpp
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/src/lib/net")
endif()

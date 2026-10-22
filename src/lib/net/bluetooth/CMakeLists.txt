# SPDX-License-Identifier: MIT
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Network Bluetooth)
find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
add_library(deskflow-bluetooth STATIC
  ConnectionManager.cpp ConnectionManager.h
  TlsChannel.cpp TlsChannel.h FailoverLink.cpp FailoverLink.h
  FailoverSession.cpp FailoverSession.h AutoTransport.cpp AutoTransport.h
  RfcommSession.cpp RfcommSession.h RfcommBridge.h
  BluetoothDiscovery.cpp BluetoothDiscovery.h
  BluetoothPairing.cpp BluetoothPairing.h)
target_include_directories(deskflow-bluetooth PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(deskflow-bluetooth PUBLIC Qt6::Core Qt6::Network Qt6::Bluetooth OpenSSL::SSL OpenSSL::Crypto)
if(APPLE)
  set_target_properties(deskflow-bluetooth PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_sources(deskflow-bluetooth PRIVATE BluetoothPairingMac.mm)
  set_source_files_properties(BluetoothPairingMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(deskflow-bluetooth PRIVATE "-framework IOBluetooth" "-framework Foundation")
endif()

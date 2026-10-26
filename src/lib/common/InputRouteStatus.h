// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
struct InputRouteStatus {
  bool connected=false,supported=false,suspended=false,localFocus=false,acknowledged=false;
  bool permissionsReady=false,responseReceived=false,otherRoutesIsolated=false;
  uint32_t lastSequence=0;
};

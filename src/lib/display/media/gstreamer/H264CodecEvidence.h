// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <gst/gst.h>
namespace deskflow::display::infra {
enum class H264Evidence
{
  Pending,
  ConstrainedBaseline,
  Rejected
};
struct H264CodecEvidence
{
  H264Evidence state = H264Evidence::Rejected;
  uint8_t profile = 0, flags = 0, level = 0;
};
// Internal SDK boundary: inspect the parser/encoder-produced AVCC profile header,
// not a potentially compatibility-adjusted caps label. This is not a replacement
// for the upstream complete SPS/bitstream parser. No borrowed data escapes.
H264CodecEvidence inspectH264CodecEvidence(const GstCaps *);
} // namespace deskflow::display::infra

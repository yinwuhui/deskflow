#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize one endpoint's log; recovery ends at bridge readiness, not TLS/input readiness."""
import argparse
import datetime
import json
import math
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("log")
args = parser.parse_args()
samples, recoveries = [], []
ready_count = 0
closed = None
with open(args.log, encoding="utf-8", errors="replace") as stream:
    for line in stream:
        match = re.search(r"rtt_ms=\s*([0-9]+(?:\.[0-9]+)?)", line)
        if match:
            samples.append(float(match[1]))
        stamp = re.search(r"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z)", line)
        if not stamp:
            continue
        moment = datetime.datetime.fromisoformat(stamp[1].replace("Z", "+00:00"))
        if "session_closed" in line and ready_count and closed is None:
            closed = moment
        if "session_ready" in line:
            ready_count += 1
            if closed is not None:
                recoveries.append(round((moment - closed).total_seconds() * 1000, 3))
                closed = None
samples.sort()
def percentile(fraction):
    return samples[max(0, math.ceil(len(samples) * fraction) - 1)] if samples else None
print(json.dumps({
    "rtt_samples": len(samples),
    "rtt_p50_ms": percentile(0.50),
    "rtt_p95_ms": percentile(0.95),
    "rtt_max_ms": samples[-1] if samples else None,
    "ready_sessions": ready_count,
    "bridge_recovery_ms": recoveries,
    "disconnected_at_end": closed is not None,
    "note": "RTT is transport round-trip time, not physical input-to-display latency."
}, ensure_ascii=False, indent=2))

# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""Bounded Annex B access-unit splitter for an encoder configured with AUDs."""
import re
import struct

MAX_AU = 4 * 1024 * 1024 - 9
START = re.compile(b"\x00\x00(?:\x00)?\x01")
AUD = re.compile(b"\x00\x00(?:\x00)?\x01\x09")


def nal_types(data):
    result = []
    previous = None
    def append(start, end):
        if start.end() >= end:
            raise ValueError("Empty NAL unit")
        kind = data[start.end()] & 31
        if kind == 0 or kind > 23 or data[start.end()] & 0x80:
            raise ValueError("Invalid H.264 NAL header")
        result.append(kind)
        if len(result) > 256:
            raise ValueError("NAL count exceeds negotiated adapter limit")
    for start in START.finditer(data):
        if previous is None and start.start() != 0:
            raise ValueError("Access unit is not Annex B")
        if previous is not None:
            append(previous, start.start())
        previous = start
    if previous is None:
        raise ValueError("Access unit is not Annex B")
    append(previous, len(data))
    return result


class AccessUnitSplitter:
    def __init__(self):
        self.buffer = bytearray()
        self.offset = 0
        self.started = False

    def feed(self, chunk):
        if len(chunk) > 65536:
            raise ValueError("Encoder read chunk too large")
        self.buffer.extend(chunk)
        units = []
        while True:
            match = AUD.search(self.buffer, self.offset)
            if not match:
                self.offset = max(self.offset, len(self.buffer) - 5)
                break
            if self.started:
                data = bytes(self.buffer[:match.start()])
                if len(data) > MAX_AU:
                    raise ValueError("Encoded access unit exceeds budget")
                nal_types(data)
                units.append(data)
                del self.buffer[:match.start()]
                self.offset = match.end() - match.start()
            else:
                if match.start() != 0:
                    raise ValueError("Encoder must put AUD before its first access unit")
                self.started = True
                self.offset = match.end()
        if len(self.buffer) > MAX_AU:
            raise ValueError("Unterminated access unit exceeds budget")
        return units

    def finish(self):
        if not self.buffer:
            return []
        if not self.started:
            raise ValueError("Encoder did not emit an AUD")
        data = bytes(self.buffer)
        nal_types(data)
        self.buffer.clear()
        return [data]


def emit_framed(stream, unit):
    if not 0 < len(unit) <= MAX_AU:
        raise ValueError("Invalid access unit size")
    stream.write(struct.pack("!I", len(unit)))
    stream.write(unit)
    stream.flush()

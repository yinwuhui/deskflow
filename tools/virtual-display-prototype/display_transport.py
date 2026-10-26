# SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
# SPDX-License-Identifier: MIT
"""DFVD/1 experimental transport. Explicit peer certificates, no TOFU or plaintext.

This Python adapter exercises the wire format before the native product bridge.
Its header is byte-compatible with src/lib/display/Protocol.cpp.
"""
import collections
import dataclasses
import hashlib
import hmac
import json
import math
import os
from pathlib import Path
import secrets
import select
import socket
import ssl
import sys
import struct
import subprocess
import time

HEADER = struct.Struct("!4sBBHI16sIQ")
ZERO = bytes(16)
CONTROL_LIMIT = 65536
INPUT_LIMIT = 4096
VIDEO_LIMIT = 4 * 1024 * 1024
HELLO, CAPS, START, ACCEPT, REJECT, CONFIG, READY, PRESENT = range(1, 9)
KEYFRAME, LEASE_REQUEST, LEASE_GRANT, LEASE_REVOKE = range(9, 13)
RECONFIGURE, RECONFIGURE_ACK, STATS, PING, PONG, STOP, STOPPED, BIND = range(13, 21)
INPUT, RELEASE, VIDEO = 64, 65, 128


class TransportError(RuntimeError):
    pass


def payload_limit(kind):
    if 1 <= kind <= 20:
        return CONTROL_LIMIT
    if kind in (INPUT, RELEASE):
        return INPUT_LIMIT
    if kind == VIDEO:
        return VIDEO_LIMIT
    raise TransportError("Unknown message type")


@dataclasses.dataclass(frozen=True)
class Frame:
    kind: int
    payload: bytes = b""
    session: bytes = ZERO
    epoch: int = 0
    sequence: int = 1

    def encode(self):
        if len(self.payload) > payload_limit(self.kind) or len(self.session) != 16:
            raise TransportError("Invalid frame size or session")
        if not 0 < self.sequence < 2**64 or not 0 <= self.epoch < 2**32:
            raise TransportError("Invalid sequence or epoch")
        return HEADER.pack(b"DFVD", 1, 0, self.kind, len(self.payload), self.session,
                           self.epoch, self.sequence) + self.payload


class Reader:
    def __init__(self, channel):
        if channel not in ("control", "input", "video"):
            raise ValueError("Invalid channel")
        self.channel = channel
        self.buffer = bytearray()
        self.sequence = 0
        self.failed = False

    def feed(self, data):
        if self.failed:
            raise TransportError("Reader poisoned")
        try:
            if len(data) > CONTROL_LIMIT:
                raise TransportError("Read chunk exceeds 64 KiB")
            self.buffer.extend(data)
            messages = []
            while len(self.buffer) >= HEADER.size:
                magic, major, minor, kind, length, session, epoch, sequence = HEADER.unpack_from(self.buffer)
                if (magic, major, minor) != (b"DFVD", 1, 0):
                    raise TransportError("Invalid protocol or version")
                if length > payload_limit(kind):
                    raise TransportError("Payload exceeds type limit")
                allowed = ((self.channel == "control" and 1 <= kind <= 19) or
                           (self.channel == "input" and kind in (INPUT, RELEASE)) or
                           (self.channel == "video" and kind == VIDEO) or
                           (self.channel != "control" and kind == BIND and self.sequence == 0))
                if not allowed or sequence <= self.sequence:
                    raise TransportError("Invalid channel or repeated sequence")
                if len(self.buffer) < HEADER.size + length:
                    break
                payload = bytes(self.buffer[HEADER.size:HEADER.size + length])
                del self.buffer[:HEADER.size + length]
                self.sequence = sequence
                messages.append(Frame(kind, payload, session, epoch, sequence))
            return messages
        except Exception:
            self.failed = True
            self.buffer.clear()
            raise

    def finish(self):
        if self.failed or self.buffer:
            self.failed = True
            raise TransportError("Poisoned or truncated connection")


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise TransportError("Duplicate JSON key")
        result[key] = value
    return result


def decode_json(payload):
    if len(payload) > CONTROL_LIMIT:
        raise TransportError("JSON payload too large")
    def constant(_):
        raise TransportError("Nonfinite JSON number")
    try:
        value = json.loads(payload.decode("utf-8"), object_pairs_hook=_pairs,
                           parse_constant=constant)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise TransportError("Invalid JSON") from error
    pending = [(value, 0)]
    count = 0
    while pending:
        item, depth = pending.pop()
        count += 1
        if depth > 16 or count > 1024:
            raise TransportError("JSON complexity limit")
        if isinstance(item, dict):
            pending.extend((child, depth + 1) for child in item.values())
            if any(len(key) > 128 for key in item):
                raise TransportError("JSON key too long")
        elif isinstance(item, list):
            pending.extend((child, depth + 1) for child in item)
        elif isinstance(item, str) and (len(item) > 4096 or "\0" in item):
            raise TransportError("Invalid JSON string")
        elif isinstance(item, float) and not math.isfinite(item):
            raise TransportError("Nonfinite JSON number")
    if not isinstance(value, dict):
        raise TransportError("Expected JSON object")
    return value


def require_fields(value, required, optional=()):
    if not isinstance(value, dict) or not set(required) <= value.keys() or value.keys() - set(required) - set(optional):
        raise TransportError("Missing or unknown message fields")
    return value


def fingerprint(certificate):
    text = Path(certificate).read_text(encoding="ascii")
    if text.count("-----BEGIN CERTIFICATE-----") != 1:
        raise TransportError("Expected exactly one peer certificate")
    return hashlib.sha256(ssl.PEM_cert_to_DER_cert(text)).digest()


def tls_context(identity, peer_certificate, server):
    if not ssl.HAS_TLSv1_3:
        raise TransportError("TLS 1.3 unavailable in this Python runtime")
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER if server else ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = False  # Device identity is the explicit certificate pin, not DNS.
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(cafile=str(peer_certificate))
    context.load_cert_chain(str(Path(identity) / "cert.pem"), str(Path(identity) / "key.pem"))
    return context


def authenticate(context, wire, peer_certificate, server=False):
    wire.settimeout(4)
    wire.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    wire.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 65536)
    wire.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    try:
        secure = context.wrap_socket(wire, server_side=server)
        if secure.version() != "TLSv1.3" or not hmac.compare_digest(
                hashlib.sha256(secure.getpeercert(binary_form=True)).digest(), fingerprint(peer_certificate)):
            secure.close()
            raise TransportError("Peer certificate or TLS version mismatch")
        return secure
    except Exception:
        wire.close()
        raise


class FramedSocket:
    """Single owner thread per SSL object; child sockets are independent."""
    def __init__(self, wire, channel, session=None, epoch=None):
        self.wire = wire
        self.reader = Reader(channel)
        self.pending = collections.deque()
        self.session = session
        self.epoch = epoch
        self.sequence = 0
        self.tx_bytes = self.rx_bytes = 0

    def bind(self, session, epoch):
        if len(session) != 16 or session == ZERO or type(epoch) is not int or epoch < 1:
            raise TransportError("Invalid session binding")
        self.session, self.epoch = session, epoch

    def send(self, kind, value=None, payload=None):
        if payload is None:
            payload = json.dumps(value or {}, separators=(",", ":"), allow_nan=False).encode()
        self.sequence += 1
        frame = Frame(kind, payload, self.session or ZERO, self.epoch or 0, self.sequence)
        encoded = frame.encode()
        deadline = time.monotonic() + 1
        for offset in range(0, len(encoded), 16384):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Frame send deadline exceeded")
            self.wire.settimeout(remaining)
            chunk = encoded[offset:offset + 16384]
            self.wire.sendall(chunk)
            self.tx_bytes += len(chunk)

    def readable(self):
        return bool(self.pending or self.wire.pending() or select.select([self.wire], [], [], 0)[0])

    def receive(self, timeout=4):
        deadline = time.monotonic() + timeout
        while not self.pending:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Frame receive timed out")
            self.wire.settimeout(remaining)
            data = self.wire.recv(CONTROL_LIMIT)
            if not data:
                self.reader.finish()
                raise EOFError("Peer disconnected")
            self.rx_bytes += len(data)
            self.pending.extend(self.reader.feed(data))
        frame = self.pending.popleft()
        # Validate at delivery too, including frames coalesced before binding.
        if self.session is not None and frame.session != self.session:
            raise TransportError("Session mismatch")
        if self.epoch is not None and frame.epoch != self.epoch:
            raise TransportError("Epoch mismatch")
        return frame

    def json(self, expected, timeout=4):
        frame = self.receive(timeout)
        if frame.kind != expected:
            raise TransportError("Unexpected message type")
        return decode_json(frame.payload)

    def close(self):
        try:
            self.wire.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.wire.close()


class BindingTokens:
    def __init__(self, session, peer_pin, epoch, ttl=10, clock=time.monotonic):
        self.session, self.peer_pin, self.epoch = session, peer_pin, epoch
        self.clock = clock
        self.deadline = clock() + ttl
        self.tokens = {name: secrets.token_hex(32) for name in ("video", "input")}

    def consume(self, channel, token, session, peer_pin, epoch):
        if (self.clock() >= self.deadline or session != self.session or epoch != self.epoch or
                not hmac.compare_digest(peer_pin, self.peer_pin) or channel not in self.tokens or
                not isinstance(token, str) or not hmac.compare_digest(token, self.tokens[channel])):
            raise TransportError("Invalid, expired or replayed channel binding")
        del self.tokens[channel]

    def revoke(self):
        self.tokens.clear()


def init_identity(directory):
    directory = Path(directory)
    directory.mkdir(mode=0o700, parents=False, exist_ok=False)
    config = directory / "request.cnf"
    config.write_text("[req]\ndistinguished_name=dn\nx509_extensions=v3\nprompt=no\n"
                      "[dn]\nCN=deskflow-display-peer\n[v3]\nbasicConstraints=critical,CA:TRUE\n"
                      "keyUsage=critical,digitalSignature,keyEncipherment,keyCertSign\nsubjectKeyIdentifier=hash\n")
    try:
        result = subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                                 "-days", "365", "-config", str(config), "-keyout", str(directory / "key.pem"),
                                 "-out", str(directory / "cert.pem")], capture_output=True, timeout=30, umask=0o077)
        if result.returncode:
            raise TransportError("Certificate generation failed")
        (directory / "key.pem").chmod(0o600)
        return fingerprint(directory / "cert.pem").hex()
    finally:
        config.unlink(missing_ok=True)


def tcp_rtt_ms(wire, system=None):
    """Passive TCP smoothed RTT. Linux uses usec; Darwin uses integer msec.

    Prefix layouts follow linux/tcp.h::tcp_info and netinet/tcp.h::tcp_connection_info.
    This is transport RTT, not capture-to-display latency. Never sends probe traffic.
    """
    system = sys.platform if system is None else system
    try:
        if system.startswith('linux'):
            fields = struct.Struct('=8B16I')
            data = wire.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 256)
            if len(data) < fields.size or data[0] != 1: return None
            return fields.unpack_from(data)[-1] / 1000.0
        if system == 'darwin':
            fields = struct.Struct('=4B11I')
            data = wire.getsockopt(socket.IPPROTO_TCP, 0x106, 256)  # TCP_CONNECTION_INFO
            if len(data) < fields.size or data[0] != 4: return None
            return float(fields.unpack_from(data)[-1])
    except (OSError, AttributeError, ValueError, struct.error):
        pass
    return None


class TransferMetrics:
    """Local framed application-byte rates; no OS-wide counters or active speed test."""
    def __init__(self, channels, clock=time.monotonic, wall_clock=time.time):
        self.channels, self.clock, self.wall_clock = channels, clock, wall_clock
        self.previous = self.totals()
        self.last_sample = clock()
        self.rtt = None
        self.rtt_at = None

    def totals(self):
        return (sum(c.tx_bytes for c in self.channels), sum(c.rx_bytes for c in self.channels))

    def record_rtt(self, milliseconds):
        if type(milliseconds) in (int, float) and math.isfinite(milliseconds) and milliseconds >= 0:
            self.rtt, self.rtt_at = float(milliseconds), self.clock()

    def sample(self):
        now = self.clock()
        interval = now - self.last_sample
        if interval < 1: return None
        totals = self.totals()
        result = dict(metricsFresh=True, sampledAtMs=str(int(self.wall_clock() * 1000)),
                      txBytesPerSecond=(totals[0]-self.previous[0])/interval,
                      rxBytesPerSecond=(totals[1]-self.previous[1])/interval,
                      rttMs=None, rttSource='tcp')
        if self.rtt_at is not None and 0 <= now-self.rtt_at <= 5:
            result.update(rttMs=self.rtt, rttSource='heartbeat')
        elif self.channels:
            result['rttMs'] = tcp_rtt_ms(self.channels[0].wire)
        self.last_sample, self.previous = now, totals
        return result

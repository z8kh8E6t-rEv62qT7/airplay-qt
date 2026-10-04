#!/usr/bin/env python3
"""Direct WAV -> native AirPlay 2 research sender; no system audio APIs."""
from __future__ import annotations

import argparse
from collections import Counter, OrderedDict
from collections.abc import Sequence
from contextlib import ExitStack
import ctypes as C
from dataclasses import dataclass
import hashlib
import hmac
import ipaddress
import json
import math
from pathlib import Path
import plistlib
import secrets
import select
import socket
import struct
import sys
import threading
import time
import uuid
import wave

from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

FRAMES = 352
MAX_BODY = 1 << 20
SRP_N = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74"
    "020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F1437"
    "4FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7ED"
    "EE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF05"
    "98DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB"
    "9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E462E36CE3B"
    "E39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2BCBF695581718"
    "3995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD33170D04507A33"
    "A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3970F85A6E1E4C7"
    "ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6BF12FFA06D98A0864"
    "D87602733EC86A64521F2B18177B200CBBE117577A615D6C770988C0BAD946E2"
    "08E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD2CAFFFFFFFFFFFFFFFF", 16)


class ProtocolError(Exception):
    pass


def report(event: str, **fields):
    print(json.dumps({"event": event, **fields}, ensure_ascii=False), flush=True)


def redacted(value):
    if isinstance(value, bytes):
        return {"bytes": len(value)}
    if isinstance(value, dict):
        return {str(k): redacted(v) for k, v in value.items()}
    if isinstance(value, (list, tuple)):
        return [redacted(v) for v in value]
    return value


def int_bytes(value: int) -> bytes:
    return value.to_bytes(max(1, (value.bit_length() + 7) // 8), "big")


def sha512(*parts: bytes) -> bytes:
    return hashlib.sha512(b"".join(parts)).digest()


def tlv_encode(fields: dict[int, bytes]) -> bytes:
    result = bytearray()
    for tag, value in fields.items():
        if not 0 <= tag <= 255:
            raise ValueError("TLV tag out of range")
        for start in range(0, max(1, len(value)), 255):
            chunk = value[start:start + 255]
            result.extend(bytes((tag, len(chunk))) + chunk)
    return bytes(result)


def tlv_decode(data: bytes) -> dict[int, bytes]:
    result: dict[int, bytes] = {}
    cursor = 0
    while cursor < len(data):
        if cursor + 2 > len(data):
            raise ProtocolError("Truncated TLV header")
        tag, size = data[cursor:cursor + 2]
        cursor += 2
        if cursor + size > len(data):
            raise ProtocolError("Truncated TLV value")
        result[tag] = result.get(tag, b"") + data[cursor:cursor + size]
        cursor += size
    return result


def srp_challenge(salt: bytes, server_public: bytes, private: int | None = None):
    """HAP SRP-6a: padded k/u, minimal-length integers in M1/HAMK/K."""
    if len(salt) != 16 or not 1 <= len(server_public) <= 384:
        raise ProtocolError("Invalid SRP challenge lengths")
    b_public = int.from_bytes(server_public, "big")
    if b_public % SRP_N == 0:
        raise ProtocolError("Invalid SRP server public key")
    private = private if private is not None else secrets.randbits(256) | (1 << 255)
    a_public = pow(5, private, SRP_N)
    pad = lambda n: n.to_bytes(384, "big")
    k = int.from_bytes(sha512(pad(SRP_N), pad(5)), "big")
    u = int.from_bytes(sha512(pad(a_public), pad(b_public)), "big")
    if not u:
        raise ProtocolError("Zero SRP scrambling parameter")
    x = int.from_bytes(sha512(salt, sha512(b"Pair-Setup:3939")), "big")
    shared = pow((b_public - k * pow(5, x, SRP_N)) % SRP_N, private + u * x, SRP_N)
    key = sha512(int_bytes(shared))
    n_xor_g = bytes(a ^ b for a, b in zip(sha512(int_bytes(SRP_N)), sha512(b"\x05")))
    public = int_bytes(a_public)
    proof = sha512(n_xor_g, sha512(b"Pair-Setup"), salt, public, int_bytes(b_public), key)
    return public, proof, sha512(public, proof, key), key


def control_key(shared: bytes, direction: str) -> bytes:
    return HKDF(algorithm=hashes.SHA512(), length=32, salt=b"Control-Salt",
                info=f"Control-{direction}-Encryption-Key".encode()).derive(shared)


class HapRecords:
    def __init__(self, write_key: bytes, read_key: bytes):
        self.writer, self.reader = ChaCha20Poly1305(write_key), ChaCha20Poly1305(read_key)
        self.tx = self.rx = 0

    @staticmethod
    def nonce(counter: int) -> bytes:
        if not 0 <= counter < 1 << 64:
            raise ProtocolError("HAP nonce exhausted")
        return b"\0" * 4 + counter.to_bytes(8, "little")

    def encode(self, data: bytes) -> bytes:
        packets = []
        for start in range(0, len(data), 1024):
            chunk = data[start:start + 1024]
            size = struct.pack("<H", len(chunk))
            packets.append(size + self.writer.encrypt(self.nonce(self.tx), chunk, size))
            self.tx += 1
        return b"".join(packets)

    def decode(self, size: bytes, encrypted: bytes) -> bytes:
        length = struct.unpack("<H", size)[0]
        if not 1 <= length <= 1024 or len(encrypted) != length + 16:
            raise ProtocolError("Invalid HAP record size")
        try:
            result = self.reader.decrypt(self.nonce(self.rx), encrypted, size)
        except InvalidTag as error:
            raise ProtocolError("HAP record authentication failed") from error
        self.rx += 1
        return result


def receive_exact(sock: socket.socket, count: int) -> bytes:
    data = bytearray()
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ProtocolError("Connection closed while reading a message")
        data.extend(chunk)
    return bytes(data)


def receive_hap_record(sock: socket.socket, records: HapRecords) -> bytes:
    size = receive_exact(sock, 2)
    length = int.from_bytes(size, "little")
    if not 1 <= length <= 1024:
        raise ProtocolError("Invalid encrypted control record length")
    return records.decode(size, receive_exact(sock, length + 16))


class Rtsp:
    def __init__(self, sock: socket.socket, identity: str):
        self.sock, self.identity = sock, identity
        self.records: HapRecords | None = None
        self.buffer = bytearray()
        self.cseq = 0
        self.session: str | None = None

    def _fill(self):
        if self.records:
            self.buffer.extend(receive_hap_record(self.sock, self.records))
        else:
            chunk = self.sock.recv(8192)
            if not chunk:
                raise ProtocolError("RTSP peer disconnected")
            self.buffer.extend(chunk)

    def response(self):
        while b"\r\n\r\n" not in self.buffer:
            if len(self.buffer) > 16384:
                raise ProtocolError("RTSP headers too large")
            self._fill()
        end = self.buffer.index(b"\r\n\r\n")
        if end > 16384:
            raise ProtocolError("RTSP headers too large")
        lines = bytes(self.buffer[:end]).decode("iso-8859-1").split("\r\n")
        status = lines[0].split(" ", 2)
        if len(status) < 2 or status[0] not in ("RTSP/1.0", "HTTP/1.1", "HTTP/1.0"):
            raise ProtocolError("Malformed RTSP status")
        headers = {}
        for line in lines[1:]:
            name, sep, value = line.partition(":")
            if not sep or name.lower() in headers:
                raise ProtocolError("Malformed or duplicate RTSP header")
            headers[name.lower()] = value.strip()
        try:
            length = int(headers.get("content-length", "0"))
            code = int(status[1])
        except ValueError as error:
            raise ProtocolError("Non-numeric RTSP status/length") from error
        if not 0 <= length <= MAX_BODY or "transfer-encoding" in headers:
            raise ProtocolError("Unsupported RTSP body framing")
        total = end + 4 + length
        while len(self.buffer) < total:
            self._fill()
        body = bytes(self.buffer[end + 4:total])
        del self.buffer[:total]
        return code, headers, body

    def request(self, method: str, path: str, body: bytes = b"", headers=None):
        self.cseq += 1
        fields = {"CSeq": str(self.cseq), "User-Agent": "AirPlay/550.10",
                  "DACP-ID": self.identity, "Active-Remote": "1", "Content-Length": str(len(body))}
        if self.session:
            fields["Session"] = self.session
        fields.update(headers or {})
        raw = (f"{method} {path} RTSP/1.0\r\n" + "".join(f"{k}: {v}\r\n" for k, v in fields.items()) + "\r\n").encode() + body
        self.sock.sendall(self.records.encode(raw) if self.records else raw)
        code, incoming, payload = self.response()
        if incoming.get("cseq", str(self.cseq)) != str(self.cseq):
            raise ProtocolError("RTSP response CSeq mismatch")
        if code != 200:
            raise ProtocolError(f"{method} {path}: receiver returned {code}")
        if "session" in incoming:
            self.session = incoming["session"].split(";", 1)[0]
        return payload

    def plist(self, method: str, path: str, value):
        payload = plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)
        data = self.request(method, path, payload, {"Content-Type": "application/x-apple-binary-plist"})
        return plistlib.loads(data) if data else {}

    def pair(self) -> bytes:
        headers = {"Content-Type": "application/pairing+tlv8", "X-Apple-HKP": "4"}
        challenge = tlv_decode(self.request("POST", "/pair-setup", tlv_encode({6: b"\x01", 0: b"\x00", 19: b"\x10"}), headers))
        if 7 in challenge or challenge.get(6) != b"\x02":
            raise ProtocolError("Transient pair-setup rejected; no authentication fallback")
        public, proof, expected, key = srp_challenge(challenge.get(2, b""), challenge.get(3, b""))
        response = tlv_decode(self.request("POST", "/pair-setup", tlv_encode({6: b"\x03", 3: public, 4: proof}), headers))
        if 7 in response or response.get(6) != b"\x04" or not hmac.compare_digest(response.get(4, b""), expected):
            raise ProtocolError("Server SRP proof verification failed")
        if self.buffer:
            raise ProtocolError("Unexpected plaintext after pairing response")
        self.records = HapRecords(control_key(key, "Write"), control_key(key, "Read"))
        self.shared_secret = key
        return key[:32]


def alac_frame(pcm: bytes) -> bytes:
    """ALAC verbatim stereo CPE: 16-bit samples, explicit frame count, ID_END."""
    if not pcm or len(pcm) % 4 or len(pcm) > FRAMES * 4:
        raise ValueError("ALAC frame must contain 1..352 complete stereo samples")
    words = struct.unpack("<" + "H" * (len(pcm) // 2), pcm)
    bits, count = 0, 0
    out = bytearray()
    def put(value, width):
        nonlocal bits, count
        bits = (bits << width) | value
        count += width
        while count >= 8:
            count -= 8
            out.append((bits >> count) & 255)
            bits &= (1 << count) - 1
    for value, width in ((1, 3), (0, 4), (0, 12), (1, 1), (0, 2), (1, 1), (len(pcm) // 4, 32)):
        put(value, width)
    for word in words:
        put(word, 16)
    put(7, 3)
    if count:
        put(0, 8 - count)
    return bytes(out)


def fourcc(text: str) -> int:
    return int.from_bytes(text.encode("ascii"), "big")


class AudioDescription(C.Structure):
    _fields_ = [("sample_rate", C.c_double)] + [(name, C.c_uint32) for name in
                ("format_id", "format_flags", "bytes_per_packet", "frames_per_packet",
                 "bytes_per_frame", "channels", "bits_per_channel", "reserved")]


class AudioBuffer(C.Structure):
    _fields_ = [("channels", C.c_uint32), ("size", C.c_uint32), ("data", C.c_void_p)]


class AudioBuffers(C.Structure):
    _fields_ = [("count", C.c_uint32), ("buffers", AudioBuffer * 1)]


class PacketDescription(C.Structure):
    _fields_ = [("offset", C.c_int64), ("frames", C.c_uint32), ("size", C.c_uint32)]


AudioInputCallback = C.CFUNCTYPE(C.c_int32, C.c_void_p, C.POINTER(C.c_uint32),
                                C.POINTER(AudioBuffers), C.POINTER(C.POINTER(PacketDescription)), C.c_void_p)


class AacEldEncoder:
    """AudioToolbox codec only: no device, AudioUnit, mixer or playback API."""
    def __init__(self, reader: wave.Wave_read):
        rate, _ = validate_wav(reader)
        if sys.platform != "darwin" or rate != 48000:
            raise ValueError("APAT/AAC-ELD requires macOS and a 48 kHz stereo WAV")
        self.reader = reader
        self.converter = C.c_void_p()
        self.input_buffer = None
        self.callback_error: Exception | None = None
        self.api = C.CDLL("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox")
        signatures = {
            "AudioConverterNew": [C.POINTER(AudioDescription), C.POINTER(AudioDescription), C.POINTER(C.c_void_p)],
            "AudioConverterDispose": [C.c_void_p],
            "AudioConverterGetProperty": [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32), C.c_void_p],
            "AudioConverterGetPropertyInfo": [C.c_void_p, C.c_uint32, C.POINTER(C.c_uint32), C.POINTER(C.c_ubyte)],
            "AudioConverterSetProperty": [C.c_void_p, C.c_uint32, C.c_uint32, C.c_void_p],
            "AudioConverterFillComplexBuffer": [C.c_void_p, AudioInputCallback, C.c_void_p,
                                                C.POINTER(C.c_uint32), C.POINTER(AudioBuffers),
                                                C.POINTER(PacketDescription)],
        }
        for name, args in signatures.items():
            fn = getattr(self.api, name)
            fn.argtypes, fn.restype = args, C.c_int32
        source = AudioDescription(48000, fourcc("lpcm"), 12, 4, 1, 4, 2, 16, 0)
        target = AudioDescription(48000, fourcc("aace"), 0, 0, 480, 0, 2, 0, 0)
        self._check(self.api.AudioConverterNew(C.byref(source), C.byref(target), C.byref(self.converter)), "AudioConverterNew")
        try:
            bitrate = C.c_uint32(160000)
            self._check(self.api.AudioConverterSetProperty(self.converter, fourcc("brat"), C.sizeof(bitrate), C.byref(bitrate)), "set AAC bitrate")
            self.output = AudioDescription()
            self._get("acod", self.output)
            if self.output.frames_per_packet != 480 or self.output.channels != 2 or self.output.sample_rate != 48000:
                raise ProtocolError("System AAC-ELD encoder did not accept 480-frame stereo/48 kHz output")
            maximum = C.c_uint32()
            self._get("xops", maximum)
            if not 1 <= maximum.value <= 65536:
                raise ProtocolError("Invalid AAC packet size from AudioToolbox")
            self.maximum = maximum.value
            size = C.c_uint32()
            self._check(self.api.AudioConverterGetPropertyInfo(self.converter, fourcc("cmgc"), C.byref(size), None), "query AAC cookie")
            if not 0 < size.value <= 65536:
                raise ProtocolError("Invalid AAC cookie size")
            cookie = C.create_string_buffer(size.value)
            self._check(self.api.AudioConverterGetProperty(self.converter, fourcc("cmgc"), C.byref(size), cookie), "read AAC cookie")
            self.cookie = cookie.raw[:size.value]
            self.callback = AudioInputCallback(self._input)
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _check(status, operation):
        if status:
            raise ProtocolError(f"{operation}: OSStatus {status} (0x{status & 0xffffffff:08x})")

    def _get(self, name, value):
        size = C.c_uint32(C.sizeof(value))
        self._check(self.api.AudioConverterGetProperty(self.converter, fourcc(name), C.byref(size), C.byref(value)), f"get {name}")

    def _input(self, converter, packets, buffers, descriptions, context):
        try:
            requested = min(packets[0], 16384)
            pcm = self.reader.readframes(requested)
            if len(pcm) % 4:
                raise ProtocolError("Truncated WAV while encoding AAC")
            packets[0] = len(pcm) // 4
            self.input_buffer = C.create_string_buffer(pcm) if pcm else None
            buffers.contents.count = 1
            buffer = buffers.contents.buffers[0]
            buffer.channels, buffer.size = 2, len(pcm)
            buffer.data = C.cast(self.input_buffer, C.c_void_p) if self.input_buffer else None
            if descriptions:
                descriptions[0] = C.POINTER(PacketDescription)()
            return 0
        except Exception as error:
            self.callback_error = error
            packets[0] = 0
            buffers.contents.buffers[0].size = 0
            buffers.contents.buffers[0].data = None
            return -1

    def packets(self):
        while True:
            output = C.create_string_buffer(self.maximum)
            buffers = AudioBuffers(1, (AudioBuffer * 1)(AudioBuffer(2, self.maximum, C.cast(output, C.c_void_p))))
            count, description = C.c_uint32(1), PacketDescription()
            status = self.api.AudioConverterFillComplexBuffer(self.converter, self.callback, None,
                                                               C.byref(count), C.byref(buffers), C.byref(description))
            if self.callback_error:
                raise ProtocolError(f"AAC input callback failed: {self.callback_error}") from self.callback_error
            self._check(status, "encode AAC packet")
            if count.value == 0:
                break
            if count.value != 1 or not 0 <= description.offset <= self.maximum or not 0 < description.size <= self.maximum - description.offset:
                raise ProtocolError("AudioToolbox returned an invalid packet description")
            yield bytes(output[description.offset:description.offset + description.size]), 480

    def close(self):
        if self.converter:
            self.api.AudioConverterDispose(self.converter)
            self.converter = C.c_void_p()


def audio_packet(key: bytes, pcm: bytes, sequence: int, timestamp: int, counter: int, first: bool) -> bytes:
    header = struct.pack("!BBHII", 0x80, 0xE0 if first else 0x60, sequence & 0xffff, timestamp & 0xffffffff, 0)
    nonce = HapRecords.nonce(counter)
    return header + ChaCha20Poly1305(key).encrypt(nonce, alac_frame(pcm), header[4:12]) + nonce[4:]


def validate_wav(reader: wave.Wave_read) -> tuple[int, int]:
    if reader.getnchannels() != 2 or reader.getsampwidth() != 2 or reader.getcomptype() != "NONE":
        raise ValueError("WAV must be uncompressed 16-bit stereo PCM")
    if reader.getframerate() not in (44100, 48000) or reader.getnframes() == 0:
        raise ValueError("WAV must contain audio at 44100 or 48000 Hz")
    return reader.getframerate(), reader.getnframes()


def make_tone(path: Path, rate: int, level_dbfs: float = -30.0):
    if not math.isfinite(level_dbfs) or not -90 <= level_dbfs <= 0:
        raise ValueError("Test tone level must be between -90 and 0 dBFS")
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setparams((2, 2, rate, 0, "NONE", "not compressed"))
        # Two seconds silence, three left, one silence, three right, two silence.
        amplitude = round(32767 * 10 ** (level_dbfs / 20))
        for second in range(11):
            channel = 0 if 2 <= second < 5 else 1 if 6 <= second < 9 else -1
            block = bytearray()
            for i in range(rate):
                position = i
                edge = min(1.0, i / (rate * .01), (rate - 1 - i) / (rate * .01))
                value = round(amplitude * edge * math.sin(2 * math.pi * 440 * position / rate)) if channel >= 0 else 0
                block.extend(struct.pack("<hh", value if channel == 0 else 0, value if channel == 1 else 0))
            output.writeframes(block)
    report("tone_created", path=str(path), rate=rate, seconds=11, peak_dbfs=level_dbfs)


def ptp_header(kind: int, length: int, clock: int, sequence: int, flags=0x408, interval=-3) -> bytes:
    header = bytearray(34)
    struct.pack_into("!BBH", header, 0, 0x10 | kind, 2, length)
    struct.pack_into("!H", header, 6, flags)
    struct.pack_into("!QHHBb", header, 20, clock, 0x8005, sequence & 0xffff,
                     5 if kind == 12 else 0, interval)
    return bytes(header)


def ptp_time(nanoseconds: int) -> bytes:
    seconds, fraction = divmod(nanoseconds, 1_000_000_000)
    return seconds.to_bytes(6, "big") + struct.pack("!I", fraction)


class PtpClock:
    """One unicast gPTP timeline shared by both explicitly selected receivers."""
    def __init__(self, bind: str, hosts: list[str], identity: int):
        self.bind, self.hosts, self.identity = bind, set(hosts), identity
        self.resources = ExitStack()
        self.stop = threading.Event()
        self.thread: threading.Thread | None = None
        self.error: Exception | None = None
        self.stats = Counter()
        self.event: socket.socket | None = None
        self.general: socket.socket | None = None

    def start(self):
        try:
            sockets = []
            for port in (319, 320):
                sock = self.resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
                # Exclusive ownership: do not silently share another sender's PTP ports.
                sock.bind(("0.0.0.0", port))
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_IF, socket.inet_aton(self.bind))
                membership = socket.inet_aton("224.0.1.129") + socket.inet_aton(self.bind)
                sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP, membership)
                sock.setblocking(False)
                sockets.append(sock)
            self.event, self.general = sockets
            self.thread = threading.Thread(target=self._run, name="airplay-poc-ptp", daemon=True)
            self.thread.start()
        except BaseException:
            self.resources.close()
            raise

    def healthy(self):
        if self.error:
            raise ProtocolError(f"PTP service failed: {self.error}") from self.error

    def _send(self, sock: socket.socket, port: int, packet: bytes, host: str | None = None):
        for receiver in (host,) if host else self.hosts:
            sock.sendto(packet, (receiver, port))

    def _announce(self, sequence):
        body = bytearray(42)
        body[13:19] = bytes((128, 6, 0x21, 0x43, 0x6a, 128))
        struct.pack_into("!Q", body, 19, self.identity)
        body[29] = 0x20
        struct.pack_into("!HHQ", body, 30, 8, 8, self.identity)
        self._send(self.general, 320, ptp_header(11, 76, self.identity, sequence, interval=0) + body)

    def _sync(self, sequence):
        self._send(self.event, 319, ptp_header(0, 44, self.identity, sequence, flags=0x608) + bytes(10))
        stamp = ptp_time(time.time_ns())
        ieee = struct.pack("!HH", 3, 28) + bytes.fromhex("0080c2000001") + bytes(22)
        apple = struct.pack("!HH", 3, 16) + bytes.fromhex("000d93000004") + struct.pack("!Q", self.identity) + bytes(2)
        self._send(self.general, 320, ptp_header(8, 96, self.identity, sequence) + stamp + ieee + apple)

    def _receive(self, packet: bytes, host: str):
        if host not in self.hosts or len(packet) < 34 or packet[1] & 15 != 2:
            return
        declared = int.from_bytes(packet[2:4], "big")
        if declared < 34 or declared > len(packet):
            return
        packet = packet[:declared]
        kind = packet[0] & 15
        sequence = int.from_bytes(packet[30:32], "big")
        self.stats[f"{host}:rx_{kind}"] += 1
        if kind in (1, 2) and len(packet) >= 44:
            stamp = ptp_time(time.time_ns())
            requester = packet[20:30]
            reply_kind, port, sock = (9, 320, self.general) if kind == 1 else (3, 319, self.event)
            self._send(sock, port, ptp_header(reply_kind, 54, self.identity, sequence, flags=0x608) + stamp + requester, host)
            if kind == 2:
                self._send(self.general, 320, ptp_header(10, 54, self.identity, sequence) + ptp_time(time.time_ns()) + requester, host)
        elif kind == 12 and len(packet) >= 44:
            cursor, grants = 44, bytearray()
            while cursor + 4 <= len(packet) and len(grants) < 96:
                tag, size = struct.unpack_from("!HH", packet, cursor)
                cursor += 4
                if cursor + size > len(packet):
                    break
                value = packet[cursor:cursor + size]
                if tag == 4 and size >= 6:
                    grants.extend(struct.pack("!HH", 5, 8) + value[:6] + b"\0\x01")
                cursor += size
            if grants:
                reply = ptp_header(12, 44 + len(grants), self.identity, sequence, flags=0x400, interval=127)
                self._send(self.general, 320, reply + packet[20:30] + grants, host)

    def _run(self):
        sequence = 0
        next_sync = next_announce = time.monotonic()
        try:
            while not self.stop.is_set():
                now = time.monotonic()
                if now >= next_announce:
                    self._announce(sequence)
                    next_announce = now + 1
                if now >= next_sync:
                    self._sync(sequence)
                    sequence += 1
                    next_sync = now + .125
                ready, _, _ = select.select([self.event, self.general], [], [], .025)
                for sock in ready:
                    packet, source = sock.recvfrom(2048)
                    self._receive(packet, source[0])
        except Exception as error:
            self.error = error

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(timeout=2)
        self.resources.close()
        report("ptp_summary", counters=dict(self.stats), error=str(self.error) if self.error else None)


@dataclass(frozen=True)
class Endpoint:
    host: str
    port: int = 7000

    @classmethod
    def parse(cls, value: str):
        host, separator, port = value.partition(":")
        ipaddress.IPv4Address(host)
        number = int(port) if separator else 7000
        if not 1 <= number <= 65535:
            raise ValueError("Receiver port out of range")
        return cls(host, number)


def parse_txt(raw: bytes) -> dict[str, str]:
    result, cursor = {}, 0
    while cursor < len(raw):
        size = raw[cursor]
        cursor += 1
        if cursor + size > len(raw):
            raise ProtocolError("Truncated Bonjour TXT record")
        key, _, value = raw[cursor:cursor + size].decode("utf-8", "replace").partition("=")
        result[key] = value
        cursor += size
    return result


def open_tcp(endpoint: Endpoint, bind: str):
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        sock.settimeout(8)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.bind((bind, 0))
        sock.connect((endpoint.host, endpoint.port))
        return sock
    except BaseException:
        sock.close()
        raise


def receiver_info(endpoint: Endpoint, bind: str):
    with open_tcp(endpoint, bind) as sock:
        data = Rtsp(sock, "0000000000000001").request("GET", "/info")
    value = plistlib.loads(data)
    if not isinstance(value, dict):
        raise ProtocolError("/info did not return a dictionary")
    txt = parse_txt(value.get("txtAirPlay", b""))
    return {"host": endpoint.host, "port": endpoint.port, "name": value.get("name"),
            "device_id": value.get("deviceID", txt.get("deviceid")), "model": value.get("model"),
            "stereo_id": txt.get("tsid"), "stereo_members": txt.get("tsm"),
            "txt": {k: v for k, v in txt.items() if k != "pk"}, "info": redacted(value)}


def sync_packet(clock: int, first_rtp: int, audible_ns: int, now_ns: int, rate: int, first: bool):
    playhead = (first_rtp + (now_ns - audible_ns) * rate // 1_000_000_000) & 0xffffffff
    frame1 = (playhead + 11035) & 0xffffffff
    return struct.pack("!BBHIQIQ", 0x90 if first else 0x80, 0xd7, 6,
                       frame1, now_ns, (frame1 + 77175) & 0xffffffff, clock)


def data_stream_keys(shared: bytes, seed: int) -> tuple[bytes, bytes]:
    salt = f"DataStream-Salt{seed}".encode("ascii")
    keys = [HKDF(algorithm=hashes.SHA512(), length=32, salt=salt,
                 info=f"DataStream-{direction}-Encryption-Key".encode()).derive(shared)
            for direction in ("Input", "Output")]
    return keys[0], keys[1]


def data_stream_message(command: bytes, payload: bytes, token: int, kind=b"sync", group=0, status=0):
    if len(command) != 4 or len(kind) != 4 or len(payload) > MAX_BODY:
        raise ValueError("Invalid APTransport TCP message")
    return struct.pack("!I4sQ4sQI", 32 + len(payload), kind, group, command, token, status) + payload


class MediaControl:
    """APTransport's 32-byte TCP envelope inside HAP encrypted records."""
    def __init__(self, sock: socket.socket, shared: bytes, seed: int):
        incoming, outgoing = data_stream_keys(shared, seed)
        self.records = HapRecords(outgoing, incoming)
        self.sock = sock
        self.buffer = bytearray()
        self.token = secrets.randbits(63)

    def receive(self):
        while len(self.buffer) < 32:
            self.buffer.extend(receive_hap_record(self.sock, self.records))
        size, kind, group, command, token, status = struct.unpack("!I4sQ4sQI", self.buffer[:32])
        if not 32 <= size <= MAX_BODY + 32:
            raise ProtocolError("APTransport TCP frame size out of range")
        while len(self.buffer) < size:
            self.buffer.extend(receive_hap_record(self.sock, self.records))
        payload = bytes(self.buffer[32:size])
        del self.buffer[:size]
        return kind, group, command, token, status, payload

    def handle_event(self, kind, group, command, token, data):
        event = {"command": command.hex(), "kind": kind.hex(), "bytes": len(data)}
        if command == b"rrmt" and data:
            try:
                event["payload"] = redacted(plistlib.loads(data))
            except (ValueError, TypeError, OverflowError):
                event["payload_error"] = "invalid plist"
        report("media_control_event", **event)
        if kind == b"sync":
            reply = data_stream_message(bytes(4), b"", token, kind=b"rply", group=group)
            self.sock.sendall(self.records.encode(reply))

    def drain_events(self):
        for _ in range(16):
            if not self.buffer and not select.select([self.sock], [], [], 0)[0]:
                return
            kind, group, command, token, _status, data = self.receive()
            self.handle_event(kind, group, command, token, data)

    def request(self, command: bytes, value):
        self.token += 1
        payload = plistlib.dumps(value, fmt=plistlib.FMT_BINARY, sort_keys=False)
        message = data_stream_message(command, payload, self.token)
        self.sock.sendall(self.records.encode(message))
        for _ in range(32):
            kind, group, received_command, token, status, data = self.receive()
            if kind == b"rply" and token == self.token:
                if status:
                    raise ProtocolError(f"Media control {command!r} failed with status {status}")
                response = plistlib.loads(data) if data else {}
                if not isinstance(response, dict):
                    raise ProtocolError("Media control response is not a dictionary")
                if response.get("status", 0):
                    raise ProtocolError(f"Media control receiver status {response['status']}")
                return response
            self.handle_event(kind, group, received_command, token, data)
        raise ProtocolError("Media control reply did not arrive within the message bound")


class ApatCrypto:
    """SRTP/SRTCP share one send counter; all nonces are explicit on wire."""
    def __init__(self, read_key: bytes, write_key: bytes):
        self.reader, self.writer = ChaCha20Poly1305(read_key), ChaCha20Poly1305(write_key)
        self.counter = 0

    def _seal(self, header: bytes, payload: bytes):
        nonce = HapRecords.nonce(self.counter)
        self.counter += 1
        return self.writer.encrypt(nonce, payload, header), nonce[4:]

    def rtp(self, packet: bytes):
        if len(packet) < 12:
            raise ValueError("Short RTP packet")
        encrypted, nonce = self._seal(packet[:12], packet[12:])
        return packet[:12] + encrypted + nonce

    def rtcp(self, packet: bytes, index: int):
        if len(packet) < 8 or index >= 1 << 31:
            raise ValueError("Invalid SRTCP packet/index")
        index_bytes = struct.pack("!I", 0x80000000 | index)
        sealed, nonce = self._seal(packet[:8] + index_bytes, packet[8:])
        return packet[:8] + sealed[:-16] + index_bytes + sealed[-16:] + nonce

    def open_rtcp(self, packet: bytes):
        if len(packet) < 36:
            raise ProtocolError("Short SRTCP feedback")
        index = packet[-28:-24]
        nonce = bytes(4) + packet[-8:]
        if not int.from_bytes(index, "big") & 0x80000000:
            raise ProtocolError("Unencrypted SRTCP feedback is not supported by this experiment")
        try:
            plain = self.reader.decrypt(nonce, packet[8:-28] + packet[-24:-8], packet[:8] + index)
        except InvalidTag as error:
            raise ProtocolError("SRTCP feedback authentication failed") from error
        return packet[:8] + plain


def congestion_feedback(packet: bytes, ssrc: int):
    """Count received/missing RTP packets in RFC 8888 RTCP PT=205/FMT=11."""
    if len(packet) < 16 or packet[0] != 0x8b or packet[1] != 205:
        return None
    size = (int.from_bytes(packet[2:4], "big") + 1) * 4
    if size != len(packet):
        raise ProtocolError("Invalid congestion feedback RTCP length")
    cursor, end = 8, size - 4  # Final word is the report timestamp.
    result = Counter()
    while cursor < end:
        if cursor + 8 > end:
            raise ProtocolError("Truncated congestion feedback block")
        stream, _begin, count = struct.unpack_from("!IHH", packet, cursor)
        cursor += 8
        if count > 16384 or cursor + ((count + 1) // 2) * 4 > end:
            raise ProtocolError("Invalid congestion feedback report count")
        if stream == ssrc:
            for offset in range(count):
                received = struct.unpack_from("!H", packet, cursor + 2 * offset)[0] & 0x8000
                result["received" if received else "missing"] += 1
            result["blocks"] += 1
        cursor += ((count + 1) // 2) * 4
    if cursor != end:
        raise ProtocolError("Misaligned congestion feedback block")
    return result


def apat_media_packets(encoded: bytes, sample_time: int, sequence: int, ssrc: int,
                       history: Sequence[tuple[int, bytes]] = ()):
    # APAP's 15-byte source header becomes this 8-byte RTP timing extension.
    # Its empty extension is a zero UIntV terminator before the encoded unit.
    payload = struct.pack("!II", sample_time >> 32, 48000) + b"\0" + encoded
    fragments = [payload[i:i + 1400] for i in range(0, len(payload), 1400)]
    result = []
    if len(fragments) == 1:
        # Native APAT includes up to three older APAP units. Their redundant
        # PT=116 form omits the 8-byte timing extension; RFC 2198 supplies
        # their timestamps as offsets from the primary PT=112 unit.
        older = []
        packet_size = 12 + 1 + len(payload) + 24
        for old_time, old_encoded in reversed(history[-3:]):
            offset = sample_time - old_time
            unit = b"\0" + old_encoded
            if (old_time >> 32) != (sample_time >> 32) or not 0 < offset <= 16383 or len(unit) > 1023:
                continue
            if packet_size + 4 + len(unit) > 1452:
                continue
            older.append((offset, unit))
            packet_size += 4 + len(unit)
        headers = bytearray()
        units = bytearray()
        for offset, unit in reversed(older):
            headers.extend((0xf4, offset >> 6,
                            ((offset & 63) << 2) | (len(unit) >> 8), len(unit) & 255))
            units.extend(unit)
        header = struct.pack("!BBHII", 0x80, 121, sequence & 0xffff,
                             sample_time & 0xffffffff, ssrc)
        return [header + headers + b"\x70" + units + payload]
    # Oversized access units retain the existing APAP fragmentation path.
    for index, fragment in enumerate(fragments):
        kind = 112 if len(fragments) == 1 else 113 if index == 0 else 115 if index == len(fragments) - 1 else 114
        header = struct.pack("!BBHII", 0x80, 121, (sequence + index) & 0xffff, sample_time & 0xffffffff, ssrc)
        result.append(header + bytes((kind,)) + fragment)
    return result


def apat_discard_report(ssrc: int, first_sequence: int):
    # RTCP XR, block 25 (discard RLE), empty interval at the initial RTP head.
    block = struct.pack("!BBHIHH", 25, 0, 2, ssrc, first_sequence & 0xffff, first_sequence & 0xffff)
    return struct.pack("!BBHI", 0x80, 207, 4, ssrc) + block


def apat_anchor(stream_id: int, clock: int, sample_time: int, audible_ns: int):
    if not isinstance(stream_id, int) or isinstance(stream_id, bool):
        raise ProtocolError("APAT dynamic streamID is missing")
    seconds, nanoseconds = divmod(audible_ns, 1_000_000_000)
    fraction = (nanoseconds << 64) // 1_000_000_000
    if fraction >= 1 << 63:
        fraction -= 1 << 64
    return {"streamID": stream_id, "rate": 1,
            "mediaTimeValue": sample_time, "mediaTimeScale": 48000,
            "firstAudibleMediaTimeValue": sample_time, "firstAudibleMediaTimeScale": 48000,
            "networkTimeTimelineID": clock, "networkTimeSecs": seconds,
            "networkTimeFrac": fraction, "networkTimeFlags": 0}


def apat_magic_cookie(stream_id: int, cookie: bytes):
    if not isinstance(stream_id, int) or isinstance(stream_id, bool):
        raise ProtocolError("APAT dynamic streamID is missing")
    if not cookie:
        raise ProtocolError("AAC-ELD encoder did not provide a magic cookie")
    cookie_id = int.from_bytes(hashlib.sha256(cookie).digest()[:8], "big") & ((1 << 63) - 1)
    return {"streamID": stream_id, "magicCookieID": cookie_id, "magicCookie": cookie}


class Peer:
    def __init__(self, endpoint: Endpoint, bind: str, identity: str):
        self.endpoint, self.bind, self.identity = endpoint, bind, identity
        self.resources = ExitStack()
        self.rtsp: Rtsp | None = None
        self.active = False
        self.url = f"rtsp://{bind}/{secrets.randbits(32)}"
        self.history: OrderedDict[int, bytes] = OrderedDict()
        self.stats = Counter()

    def _plist(self, method: str, value):
        report("request", host=self.endpoint.host, method=method, payload=redacted(value))
        response = self.rtsp.plist(method, self.url, value)
        report("response", host=self.endpoint.host, method=method, payload=redacted(response))
        return response

    def setup(self, rate: int, clock_id: int, group: str, stereo: bool, transport="realtime", spf=FRAMES):
        self.transport = transport
        sock = self.resources.enter_context(open_tcp(self.endpoint, self.bind))
        self.rtsp = Rtsp(sock, self.identity)
        self.rtsp.request("GET", "/info")
        self.key = self.rtsp.pair()
        report("paired", host=self.endpoint.host, method="transient")
        timing = {"ID": str(uuid.uuid4()).upper(), "DeviceType": 0,
                  "ClockID": clock_id, "Addresses": [self.bind], "SupportsClockPortMatchingOverride": False}
        device_id = ":".join(self.identity[i:i + 2] for i in range(0, 16, 2))
        session = {"deviceID": device_id, "macAddress": device_id, "name": "AirFlash WAV PoC",
                   "sessionUUID": str(uuid.uuid4()).upper(), "timingProtocol": "PTP",
                   "groupUUID": group, "groupContainsGroupLeader": False,
                   "isMultiSelectAirPlay": True, "senderSupportsRelay": False,
                   "timingPeerInfo": timing, "timingPeerList": [timing]}
        if stereo:
            session["senderPerceivedClusterType"] = 1
        if transport == "apat":
            session["isResponsiveAudioConnection"] = True
        response = self._plist("SETUP", session)
        self.active = True
        event_port = response.get("eventPort")
        if not isinstance(event_port, int) or isinstance(event_port, bool) or not 1 <= event_port <= 65535:
            raise ProtocolError("Session SETUP omitted a valid eventPort")
        self.resources.enter_context(open_tcp(Endpoint(self.endpoint.host, event_port), self.bind))
        self.data = self.resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
        self.control = self.resources.enter_context(socket.socket(socket.AF_INET, socket.SOCK_DGRAM))
        for udp in (self.data, self.control):
            udp.bind((self.bind, 0))
            udp.setblocking(False)
        self.rtsp.request("RECORD", self.url)
        stream = {"type": 96, "audioFormat": 1 << (18 if rate == 44100 else 20), "audioMode": "default",
                  "ct": 2, "sr": rate, "spf": spf, "isMedia": True,
                  "dataPort": self.data.getsockname()[1], "controlPort": self.control.getsockname()[1],
                  "latencyMin": 11025, "latencyMax": 88200, "shk": self.key,
                  "streamConnectionID": secrets.randbits(63), "supportsDynamicStreamID": False}
        if transport == "apat":
            self.apat_seed = secrets.randbits(63)
            self.media_control_seed = secrets.randbits(63)
            stream = {"type": 103, "audioFormat": 1 << 25, "audioFormatIndex": 25,
                      "audioMode": "default", "audioType": "media", "ct": 8, "sr": 48000, "spf": 480,
                      "isMedia": True, "supportsDynamicStreamID": True,
                      "supportsRTPPacketRedundancy": True,
                      "streamConnectionID": secrets.randbits(63),
                      "streamConnections": {
                          "streamConnectionTypeAPAT": {"streamConnectionKeyTransportProtocol": "UDP",
                                                       "streamConnectionKeyEncryptionSeed": self.apat_seed},
                          "streamConnectionTypeMediaDataControl": {"streamConnectionKeyEncryptionSeed": self.media_control_seed}}}
        response = self._plist("SETUP", {"streams": [stream]})
        streams = response.get("streams", [])
        if not isinstance(streams, list) or len(streams) != 1 or not isinstance(streams[0], dict):
            raise ProtocolError("Invalid stream SETUP response")
        if transport == "apat":
            connections = streams[0].get("streamConnections", {})
            if not isinstance(connections, dict):
                raise ProtocolError("Invalid APAT streamConnections response")
            data_connection = connections.get("streamConnectionTypeAPAT", {})
            data_port = data_connection.get("streamConnectionKeyPort")
            if not isinstance(data_port, int) or isinstance(data_port, bool) or not 1 <= data_port <= 65535:
                raise ProtocolError("Receiver did not negotiate an APAT UDP port; no fallback")
            protocol = data_connection.get("streamConnectionKeyTransportProtocol", "UDP")
            if protocol != "UDP":
                raise ProtocolError(f"Unexpected APAT transport {protocol!r}")
            self.data_address = (self.endpoint.host, data_port)
            control_connection = connections.get("streamConnectionTypeMediaDataControl", {})
            control_port = control_connection.get("streamConnectionKeyPort")
            if not isinstance(control_port, int) or isinstance(control_port, bool) or not 1 <= control_port <= 65535:
                raise ProtocolError("Receiver did not negotiate a media data control port")
            media_socket = self.resources.enter_context(open_tcp(Endpoint(self.endpoint.host, control_port), self.bind))
            self.media_control = MediaControl(media_socket, self.rtsp.shared_secret, self.media_control_seed)
            self.apat_crypto = ApatCrypto(*data_stream_keys(self.rtsp.shared_secret, self.apat_seed))
            self.stream_id = streams[0].get("streamID")
            if not isinstance(self.stream_id, int) or isinstance(self.stream_id, bool):
                raise ProtocolError("APAT SETUP omitted the negotiated dynamic streamID")
            self._plist("SETPEERS", [self.endpoint.host, self.bind])
            return
        ports = [streams[0].get("dataPort"), streams[0].get("controlPort")]
        if not all(isinstance(p, int) and not isinstance(p, bool) and 1 <= p <= 65535 for p in ports):
            raise ProtocolError("Missing audio/control ports")
        self.data_address = (self.endpoint.host, ports[0])
        self.control_address = (self.endpoint.host, ports[1])
        self._plist("SETPEERS", [self.endpoint.host, self.bind])

    def initialize_volume(self, current_db: float):
        if isinstance(current_db, bool) or not isinstance(current_db, (float, int)):
            raise ProtocolError("Receiver did not provide a numeric initialVolume")
        if not math.isfinite(current_db) or not -144 <= current_db <= 0:
            raise ProtocolError("Receiver initialVolume is outside the AirPlay dB range")
        body = f"volume: {current_db:.6f}\r\n".encode("ascii")
        self.rtsp.request("SET_PARAMETER", self.url, body, {"Content-Type": "text/parameters"})
        self.current_volume_db = current_db
        report("volume_initialized", host=self.endpoint.host, db=current_db, source="receiver_initialVolume")

    def send(self, pcm: bytes, sequence: int, timestamp: int, counter: int):
        packet = audio_packet(self.key, pcm, sequence, timestamp, counter, counter == 0)
        self.data.sendto(packet, self.data_address)
        self.history[sequence & 0xffff] = packet
        self.history.move_to_end(sequence & 0xffff)
        if len(self.history) > 1024:
            self.history.popitem(last=False)
        self.stats["sent"] += 1

    def start_apat(self, clock: int, rtp: int, audible_ns: int, ssrc: int,
                   sequence: int, cookie: bytes):
        self.apat_ssrc = ssrc
        discard = self.apat_crypto.rtcp(apat_discard_report(ssrc, sequence), 0)
        self.data.sendto(discard, self.data_address)
        cookie_request = apat_magic_cookie(self.stream_id, cookie)
        report("media_control_request", host=self.endpoint.host, command="magc",
               payload=redacted(cookie_request))
        cookie_response = self.media_control.request(b"magc", cookie_request)
        report("media_control_response", host=self.endpoint.host, command="magc",
               payload=redacted(cookie_response))
        request = apat_anchor(self.stream_id, clock, rtp, audible_ns)
        report("media_control_request", host=self.endpoint.host, command="srat", payload=request)
        response = self.media_control.request(b"srat", request)
        report("media_control_response", host=self.endpoint.host, command="srat", payload=redacted(response))

    def send_apat(self, packets: list[bytes]):
        for packet in packets:
            encrypted = self.apat_crypto.rtp(packet)
            self.data.sendto(encrypted, self.data_address)
            self.stats["sent"] += 1

    def apat_feedback(self):
        for _ in range(64):
            try:
                packet, source = self.data.recvfrom(8192)
            except BlockingIOError:
                return
            if source[0] != self.endpoint.host:
                continue
            try:
                plain = self.apat_crypto.open_rtcp(packet)
                self.stats[f"feedback_type_{plain[1]}"] += 1
                if plain[1] == 205 and self.stats["feedback_type_205"] <= 2:
                    report("apat_feedback_header", host=self.endpoint.host,
                           fmt=plain[0] & 31, length=len(plain), prefix=plain[:24].hex())
                feedback = congestion_feedback(plain, self.apat_ssrc)
                if feedback is not None:
                    self.stats.update({f"ccfb_{key}": value for key, value in feedback.items()})
                    if self.stats["feedback_type_205"] <= 2:
                        report("apat_feedback_detail", host=self.endpoint.host,
                               fmt=plain[0] & 31, length=len(plain), counters=dict(feedback))
            except ProtocolError as error:
                self.stats["feedback_decode_errors"] += 1
                if self.stats["feedback_decode_errors"] <= 2:
                    report("apat_feedback_error", host=self.endpoint.host, detail=str(error))

    def retransmit(self):
        for _ in range(64):
            try:
                request, source = self.control.recvfrom(1024)
            except BlockingIOError:
                return
            if source[0] != self.endpoint.host or len(request) < 8 or request[1] & 0x7f != 0x55:
                continue
            req_seq, first, count = struct.unpack_from("!HHH", request, 2)
            if not 1 <= count <= 1024:
                continue
            self.stats["retransmit_requested"] += count
            for index in range(count):
                packet = self.history.get((first + index) & 0xffff)
                if packet:
                    self.control.sendto(struct.pack("!BBH", 0x80, 0xd6, req_seq) + packet, source)
                    self.stats["retransmit_sent"] += 1
                else:
                    self.stats["retransmit_expired"] += 1

    def close(self):
        try:
            if self.active and self.rtsp:
                self.rtsp.sock.settimeout(1)
                try:
                    self.rtsp.request("TEARDOWN", self.url)
                except (OSError, ProtocolError) as error:
                    report("teardown_warning", host=self.endpoint.host, detail=str(error))
        finally:
            self.resources.close()
            report("peer_summary", host=self.endpoint.host, counters=dict(self.stats))


def local_address(endpoint: Endpoint, bind: str | None):
    if bind:
        return str(ipaddress.IPv4Address(bind))
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.connect((endpoint.host, endpoint.port))
        return sock.getsockname()[0]


def transmit_apat(encoder: AacEldEncoder, peers: list[Peer], clock: PtpClock, lead: float):
    first_rtp, sequence = secrets.randbits(32), secrets.randbits(16)
    ssrc = (secrets.randbelow(65535) + 1) << 16
    audible_ns = time.time_ns() + round(lead * 1_000_000_000)
    for peer in peers:
        peer.start_apat(clock.identity, first_rtp, audible_ns, ssrc, sequence, encoder.cookie)
        peer.initialize_volume(peer.current_volume_db)
    if audible_ns - time.time_ns() < 300_000_000:
        raise ProtocolError("APAT anchor negotiation exhausted the playback lead")
    start = time.monotonic()
    frames = packets_sent = 0
    history: list[tuple[int, bytes]] = []
    report("streaming", mode="APAT/UDP/AAC-ELD", rate=48000, frames_per_packet=480,
           first_rtp=first_rtp, first_sequence=sequence, ssrc=ssrc,
           audible_ns=audible_ns, mixer=False)
    for encoded, duration in encoder.packets():
        due = start + frames / 48000
        while time.monotonic() < due:
            for peer in peers:
                peer.apat_feedback()
            time.sleep(min(.003, max(0, due - time.monotonic())))
        clock.healthy()
        if time.monotonic() - due > .1:
            raise ProtocolError("APAT sender fell over 100 ms behind its timeline")
        media_time = first_rtp + frames
        packets = apat_media_packets(encoded, media_time, sequence, ssrc, history)
        for peer in peers:
            peer.send_apat(packets)
            peer.apat_feedback()
        sequence += len(packets)
        packets_sent += len(packets)
        if len(packets) == 1:
            history.append((media_time, encoded))
            del history[:-3]
        else:
            history.clear()
        frames += duration
    remaining = max(0, (audible_ns - time.time_ns()) / 1_000_000_000 + frames / 48000)
    tail_end = time.monotonic() + remaining + .25
    while time.monotonic() < tail_end:
        clock.healthy()
        for peer in peers:
            peer.apat_feedback()
        time.sleep(.01)
    for peer in peers:
        peer.media_control.drain_events()
    report("transmission_complete", frames=frames, packets_per_peer=packets_sent,
           listening_result="not_verified", mode="APAT/UDP/AAC-ELD")


def play(path: Path, endpoints: list[Endpoint], bind: str, lead: float, transport="realtime",
         samples=FRAMES, spf=FRAMES):
    with wave.open(str(path), "rb") as reader, ExitStack() as cleanup:
        rate, total = validate_wav(reader)
        encoder = None
        if transport == "apat":
            encoder = AacEldEncoder(reader)
            cleanup.callback(encoder.close)
            report("encoder_ready", codec="AAC-ELD", rate=rate, frames_per_packet=encoder.output.frames_per_packet,
                   cookie_bytes=len(encoder.cookie), mixer=False)
        info = [receiver_info(ep, bind) for ep in endpoints]
        for entry in info:
            report("receiver", **entry)
            if transport == "apat":
                formats = entry["info"].get("supportedAudioFormatsExtended", {}).get("bufferStream")
                if not isinstance(formats, list) or 25 not in formats:
                    raise ProtocolError(f"{entry['host']} does not advertise AAC-ELD/48 kHz for buffered streams")
            else:
                formats = entry["info"].get("supportedFormats", {}).get("audioStream")
                requested = 1 << (18 if rate == 44100 else 20)
                if isinstance(formats, int) and not formats & requested:
                    raise ProtocolError(f"{entry['host']} does not advertise 16-bit stereo ALAC at {rate} Hz for type=96")
        stereo = len(endpoints) == 2
        if stereo:
            if not info[0]["stereo_id"] or info[0]["stereo_id"] != info[1]["stereo_id"]:
                raise ProtocolError("Selected receivers do not advertise the same stereo pair")
            if not info[0]["device_id"] or info[0]["device_id"] == info[1]["device_id"]:
                raise ProtocolError("Two distinct receiver identities are required")
        clock_id = secrets.randbits(63) | (1 << 62)
        identity = f"{clock_id:016X}"
        group = str(uuid.uuid4()).upper()
        clock = PtpClock(bind, [ep.host for ep in endpoints], clock_id)
        cleanup.callback(clock.close)
        clock.start()
        peers = []
        for endpoint, entry in zip(endpoints, info):
            peer = Peer(endpoint, bind, identity)
            cleanup.callback(peer.close)
            peer.setup(rate, clock_id, group, stereo, transport, spf)
            peer.initialize_volume(entry["info"].get("initialVolume"))
            peers.append(peer)
            clock.healthy()
        # Let PTP exchanges settle before committing a common audible timeline.
        time.sleep(2)
        clock.healthy()
        if transport == "apat":
            transmit_apat(encoder, peers, clock, lead)
            return
        first_rtp, first_seq = secrets.randbits(32), secrets.randbits(16)
        start_mono = time.monotonic()
        audible_ns = time.time_ns() + round(lead * 1_000_000_000)
        sent_frames = counter = 0
        next_sync = start_mono
        report("streaming", rate=rate, total_frames=total, lead_seconds=lead,
               samples=samples, spf=spf,
               first_rtp=first_rtp, first_sequence=first_seq, audible_ns=audible_ns,
               clock_id=f"{clock_id:016X}", mode="96/ALAC/PTP", mixer=False)
        while sent_frames < total:
            clock.healthy()
            due = start_mono + sent_frames / rate
            while time.monotonic() < due:
                for peer in peers:
                    peer.retransmit()
                time.sleep(min(.003, max(0, due - time.monotonic())))
            if time.monotonic() - due > .1:
                raise ProtocolError("Sender fell over 100 ms behind its committed timeline")
            now = time.monotonic()
            if now >= next_sync:
                packet = sync_packet(clock_id, first_rtp, audible_ns, time.time_ns(), rate, counter == 0)
                for peer in peers:
                    peer.control.sendto(packet, peer.control_address)
                next_sync = now + .5
            pcm = reader.readframes(min(samples, total - sent_frames))
            if not pcm or len(pcm) % 4:
                raise ProtocolError("Truncated WAV payload")
            for peer in peers:
                peer.send(pcm, first_seq + counter, first_rtp + sent_frames, counter)
                peer.retransmit()
            sent_frames += len(pcm) // 4
            counter += 1
        # Keep clock and retransmit service alive until the queued tail is audible.
        tail_end = start_mono + total / rate + lead + .25
        while time.monotonic() < tail_end:
            clock.healthy()
            if time.monotonic() >= next_sync:
                packet = sync_packet(clock_id, first_rtp, audible_ns, time.time_ns(), rate, False)
                for peer in peers:
                    peer.control.sendto(packet, peer.control_address)
                next_sync = time.monotonic() + .5
            for peer in peers:
                peer.retransmit()
            time.sleep(.01)
        report("transmission_complete", frames=sent_frames, packets_per_peer=counter,
               listening_result="not_verified")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    tone = commands.add_parser("tone", help="Create low-level left/silence/right WAV")
    tone.add_argument("path", type=Path)
    tone.add_argument("--rate", type=int, choices=(44100, 48000), default=48000)
    tone.add_argument("--level-dbfs", type=float, default=-30, help="Test tone peak level (-90..0 dBFS)")
    probe = commands.add_parser("probe", help="Read explicit receivers' /info")
    send = commands.add_parser("play", help="Transmit WAV without the system mixer")
    for command in (probe, send):
        command.add_argument("--host", action="append", required=True, help="IPv4[:port], repeat for the stereo partner")
        command.add_argument("--bind", help="Local IPv4 address; otherwise use the route to the first receiver")
    send.add_argument("path", type=Path)
    send.add_argument("--transport", choices=("realtime", "apat"), default="realtime",
                      help="Validated ALAC baseline or experimental native APAT/AAC-ELD")
    send.add_argument("--lead", type=float, default=2.0,
                      help="Audible start lead in seconds (realtime 0..2; APAT 1..2)")
    send.add_argument("--samples", type=int, default=FRAMES,
                      help="Realtime ALAC samples per packet, integer >= 0 (default: 352)")
    send.add_argument("--spf", type=int, default=FRAMES,
                      help="Realtime SETUP spf, independent of --samples, integer >= 0 (default: 352)")
    args = parser.parse_args(argv)
    try:
        if args.command == "tone":
            make_tone(args.path, args.rate, args.level_dbfs)
            return 0
        if args.command == "play" and args.transport == "realtime":
            for name in ("samples", "spf"):
                if getattr(args, name) < 0:
                    raise ValueError(f"--{name} must be greater than or equal to 0")
        endpoints = [Endpoint.parse(value) for value in args.host]
        if not 1 <= len(endpoints) <= 2 or len(set(ep.host for ep in endpoints)) != len(endpoints):
            raise ValueError("Specify one receiver or two distinct members of one stereo pair")
        bind = local_address(endpoints[0], args.bind)
        if args.command == "probe":
            for endpoint in endpoints:
                report("receiver", **receiver_info(endpoint, bind))
        else:
            minimum_lead = 1 if args.transport == "apat" else 0
            if not math.isfinite(args.lead) or not minimum_lead <= args.lead <= 2:
                raise ValueError(f"Lead must be between {minimum_lead} and two seconds for {args.transport}")
            play(args.path, endpoints, bind, args.lead, args.transport, args.samples, args.spf)
        return 0
    except KeyboardInterrupt:
        report("interrupted")
        return 130
    except (OSError, ValueError, ProtocolError, plistlib.InvalidFileException, wave.Error) as error:
        report("error", kind=type(error).__name__, detail=str(error))
        return 1


if __name__ == "__main__":
    sys.exit(main())

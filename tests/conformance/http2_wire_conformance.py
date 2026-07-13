#!/usr/bin/env python3
"""RFC 9113 wire conformance tests for the Ruvia HTTP/2 server.

Each case uses a fresh TCP connection, writes HTTP/2 frames directly, and verifies
the resulting frame type, stream identifier, and error code on the wire. The shape is
inspired by h2spec, while the expectations below follow RFC 9113.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import socket
import struct
import subprocess
import sys
import time
from typing import Callable


PREFACE = b"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"

DATA = 0x0
HEADERS = 0x1
PRIORITY = 0x2
RST_STREAM = 0x3
SETTINGS = 0x4
PUSH_PROMISE = 0x5
PING = 0x6
GOAWAY = 0x7
WINDOW_UPDATE = 0x8
CONTINUATION = 0x9

END_STREAM = 0x1
ACK = 0x1
END_HEADERS = 0x4
PADDED = 0x8

NO_ERROR = 0x0
PROTOCOL_ERROR = 0x1
FLOW_CONTROL_ERROR = 0x3
STREAM_CLOSED = 0x5
FRAME_SIZE_ERROR = 0x6
COMPRESSION_ERROR = 0x9


class ConformanceFailure(AssertionError):
    pass


@dataclass(frozen=True)
class Frame:
    frame_type: int
    flags: int
    stream_id: int
    payload: bytes


def frame(frame_type: int, flags: int = 0, stream_id: int = 0, payload: bytes = b"") -> bytes:
    if len(payload) > 0xFFFFFF:
        raise ValueError("HTTP/2 frame payload is too large")
    return (
        len(payload).to_bytes(3, "big")
        + bytes((frame_type, flags))
        + (stream_id & 0x7FFFFFFF).to_bytes(4, "big")
        + payload
    )


def setting(identifier: int, value: int) -> bytes:
    return struct.pack("!HI", identifier, value)


def hpack_string(value: bytes) -> bytes:
    if len(value) >= 127:
        raise ValueError("test HPACK helper supports strings shorter than 127 bytes")
    return bytes((len(value),)) + value


def hpack_literal(name: bytes, value: bytes) -> bytes:
    return b"\x00" + hpack_string(name) + hpack_string(value)


def request_block(*extra: tuple[bytes, bytes]) -> bytes:
    # RFC 7541 static indices: :method GET=2, :scheme http=6, :path /=4.
    block = bytearray(b"\x82\x86\x84")
    # Literal without indexing, indexed name 1 (:authority).
    block.extend(b"\x01")
    block.extend(hpack_string(b"127.0.0.1"))
    for name, value in extra:
        block.extend(hpack_literal(name, value))
    return bytes(block)


def request_headers(stream_id: int, *extra: tuple[bytes, bytes], end_stream: bool = True) -> bytes:
    flags = END_HEADERS | (END_STREAM if end_stream else 0)
    return frame(HEADERS, flags, stream_id, request_block(*extra))


class H2Connection:
    def __init__(self, host: str, port: int, handshake: bool = True):
        self.socket = socket.create_connection((host, port), timeout=2.0)
        self.socket.settimeout(2.0)
        if handshake:
            self.handshake()

    def __enter__(self) -> "H2Connection":
        return self

    def __exit__(self, *_: object) -> None:
        self.socket.close()

    def send(self, data: bytes) -> None:
        self.socket.sendall(data)

    def recv_exact(self, size: int) -> bytes:
        data = bytearray()
        while len(data) < size:
            chunk = self.socket.recv(size - len(data))
            if not chunk:
                raise EOFError("peer closed the connection")
            data.extend(chunk)
        return bytes(data)

    def read_frame(self) -> Frame:
        header = self.recv_exact(9)
        length = int.from_bytes(header[:3], "big")
        return Frame(
            frame_type=header[3],
            flags=header[4],
            stream_id=int.from_bytes(header[5:9], "big") & 0x7FFFFFFF,
            payload=self.recv_exact(length),
        )

    def handshake(self) -> None:
        self.send(PREFACE + frame(SETTINGS))
        first = self.read_frame()
        require(
            first.frame_type == SETTINGS and first.stream_id == 0 and not (first.flags & ACK),
            f"server preface must start with SETTINGS, received {describe(first)}",
        )
        self.send(frame(SETTINGS, ACK))

    def find(self, predicate: Callable[[Frame], bool], description: str) -> Frame:
        deadline = time.monotonic() + 2.0
        seen: list[str] = []
        while time.monotonic() < deadline:
            try:
                candidate = self.read_frame()
            except socket.timeout:
                break
            except EOFError:
                break
            seen.append(describe(candidate))
            if predicate(candidate):
                return candidate
        raise ConformanceFailure(
            f"expected {description}; observed {', '.join(seen) if seen else 'no frame'}"
        )

    def expect_goaway(self, error: int) -> Frame:
        result = self.find(lambda item: item.frame_type == GOAWAY, "GOAWAY")
        require(len(result.payload) >= 8, "GOAWAY payload is shorter than 8 bytes")
        actual = int.from_bytes(result.payload[4:8], "big")
        require(actual == error, f"expected GOAWAY error {error}, received {actual}")
        return result

    def expect_rst(self, stream_id: int, error: int) -> Frame:
        result = self.find(
            lambda item: item.frame_type == RST_STREAM and item.stream_id == stream_id,
            f"RST_STREAM on stream {stream_id}",
        )
        require(len(result.payload) == 4, "RST_STREAM payload must contain one error code")
        actual = int.from_bytes(result.payload, "big")
        require(actual == error, f"expected RST_STREAM error {error}, received {actual}")
        return result

    def expect_ping_ack(self, opaque: bytes) -> None:
        result = self.find(
            lambda item: item.frame_type == PING and (item.flags & ACK) != 0,
            "PING ACK",
        )
        require(result.stream_id == 0, "PING ACK must use stream 0")
        require(result.payload == opaque, "PING ACK must echo the opaque payload")

    def expect_stream_end(self, stream_id: int) -> None:
        self.find(
            lambda item: item.stream_id == stream_id
            and item.frame_type in (HEADERS, DATA)
            and (item.flags & END_STREAM) != 0,
            f"END_STREAM response on stream {stream_id}",
        )

    def expect_alive(self) -> None:
        opaque = b"RUVIAH2!"
        self.send(frame(PING, 0, 0, opaque))
        self.expect_ping_ack(opaque)


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ConformanceFailure(message)


def describe(value: Frame) -> str:
    return (
        f"type=0x{value.frame_type:02x} flags=0x{value.flags:02x} "
        f"stream={value.stream_id} length={len(value.payload)}"
    )


Test = tuple[str, str, Callable[[str, int], None]]
TESTS: list[Test] = []


def case(section: str, name: str) -> Callable[[Callable[[str, int], None]], Callable[[str, int], None]]:
    def register(function: Callable[[str, int], None]) -> Callable[[str, int], None]:
        TESTS.append((section, name, function))
        return function
    return register


@case("3.4", "server preface starts with SETTINGS")
def server_preface(host: str, port: int) -> None:
    with H2Connection(host, port):
        pass


@case("3.4", "invalid client connection preface is dropped")
def invalid_preface(host: str, port: int) -> None:
    with H2Connection(host, port, handshake=False) as connection:
        connection.send(b"PRI * HTTP/2.0\r\n\r\nXX\r\n\r\n")
        require(connection.socket.recv(1) == b"", "invalid preface did not close the connection")


@case("3.4", "first peer frame must be SETTINGS")
def first_frame_settings(host: str, port: int) -> None:
    with H2Connection(host, port, handshake=False) as connection:
        connection.send(PREFACE + frame(PING, 0, 0, b"12345678"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("4.1", "unknown frame type is ignored")
def unknown_frame(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(0xFA, 0xFF, 0, b"extension"))
        connection.expect_alive()


@case("4.2", "frame above local maximum causes FRAME_SIZE_ERROR")
def oversized_frame(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send((16385).to_bytes(3, "big") + bytes((DATA, 0)) + (1).to_bytes(4, "big"))
        connection.expect_goaway(FRAME_SIZE_ERROR)


@case("4.3", "invalid HPACK index causes COMPRESSION_ERROR")
def invalid_hpack_index(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(HEADERS, END_HEADERS | END_STREAM, 1, b"\x80"))
        connection.expect_goaway(COMPRESSION_ERROR)


@case("5.1", "DATA on an idle stream is a connection PROTOCOL_ERROR")
def data_idle(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(DATA, END_STREAM, 1, b"x"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("5.1", "HEADERS after peer END_STREAM causes STREAM_CLOSED")
def headers_half_closed_remote(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1) + request_headers(1))
        connection.expect_rst(1, STREAM_CLOSED)


@case("5.1", "closed-stream HEADERS are minimally processed under RFC 9113")
def headers_closed_are_discarded(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1))
        connection.expect_stream_end(1)
        connection.send(request_headers(1))
        connection.expect_alive()


@case("5.1.1", "even peer stream identifier causes PROTOCOL_ERROR")
def even_stream_id(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(2))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("5.1.1", "new peer stream identifiers must increase")
def non_increasing_stream_id(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(5) + request_headers(3))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("5.3.2", "deprecated PRIORITY self-dependency is ignored")
def priority_self_dependency(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(PRIORITY, 0, 1, (1).to_bytes(4, "big") + b"\x0f"))
        connection.expect_alive()


@case("6.1", "DATA stream identifier must be nonzero")
def data_stream_zero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(DATA, 0, 0, b"x"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.1", "invalid DATA padding causes PROTOCOL_ERROR")
def invalid_data_padding(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1, end_stream=False))
        connection.send(frame(DATA, PADDED, 1, b"\x02x"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.2", "HEADERS stream identifier must be nonzero")
def headers_stream_zero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(HEADERS, END_HEADERS, 0, request_block()))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.4", "RST_STREAM stream identifier must be nonzero")
def rst_stream_zero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(RST_STREAM, 0, 0, PROTOCOL_ERROR.to_bytes(4, "big")))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.4", "RST_STREAM on an idle stream causes PROTOCOL_ERROR")
def rst_idle_stream(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(RST_STREAM, 0, 1, PROTOCOL_ERROR.to_bytes(4, "big")))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.5", "SETTINGS ACK payload causes FRAME_SIZE_ERROR")
def settings_ack_payload(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, ACK, 0, b"\x00"))
        connection.expect_goaway(FRAME_SIZE_ERROR)


@case("6.5", "SETTINGS stream identifier must be zero")
def settings_stream_nonzero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, 0, 1, setting(3, 100)))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.5", "SETTINGS payload length must be a multiple of six")
def settings_bad_length(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, 0, 0, b"\x00\x03\x00"))
        connection.expect_goaway(FRAME_SIZE_ERROR)


@case("6.5.2", "invalid SETTINGS_ENABLE_PUSH causes PROTOCOL_ERROR")
def settings_enable_push(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, 0, 0, setting(2, 2)))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.5.2", "oversized initial window causes FLOW_CONTROL_ERROR")
def settings_initial_window(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, 0, 0, setting(4, 0x80000000)))
        connection.expect_goaway(FLOW_CONTROL_ERROR)


@case("6.5.2", "invalid maximum frame size causes PROTOCOL_ERROR")
def settings_max_frame_size(host: str, port: int) -> None:
    for value in (16383, 16777216):
        with H2Connection(host, port) as connection:
            connection.send(frame(SETTINGS, 0, 0, setting(5, value)))
            connection.expect_goaway(PROTOCOL_ERROR)


@case("6.5.2", "unknown setting is ignored")
def settings_unknown(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(SETTINGS, 0, 0, setting(0xFF, 1)))
        connection.expect_alive()


@case("6.7", "PING stream identifier must be zero")
def ping_stream_nonzero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(PING, 0, 1, b"12345678"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.7", "PING payload must contain eight octets")
def ping_bad_length(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(PING, 0, 0, b"short"))
        connection.expect_goaway(FRAME_SIZE_ERROR)


@case("6.8", "GOAWAY stream identifier must be zero")
def goaway_stream_nonzero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(GOAWAY, 0, 1, b"\x00" * 8))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.9", "WINDOW_UPDATE increment must be nonzero")
def window_update_zero(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(WINDOW_UPDATE, 0, 0, b"\x00" * 4))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.9", "WINDOW_UPDATE payload must contain four octets")
def window_update_bad_length(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(WINDOW_UPDATE, 0, 0, b"\x00" * 3))
        connection.expect_goaway(FRAME_SIZE_ERROR)


@case("6.10", "CONTINUATION without an open field block causes PROTOCOL_ERROR")
def continuation_without_headers(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(frame(CONTINUATION, END_HEADERS, 1, b"\x82"))
        connection.expect_goaway(PROTOCOL_ERROR)


@case("6.10", "an open field block admits only same-stream CONTINUATION")
def continuation_interleaving(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(
            frame(HEADERS, END_STREAM, 1, request_block()[:2])
            + frame(PING, 0, 0, b"12345678")
        )
        connection.expect_goaway(PROTOCOL_ERROR)


@case("8.2.1", "uppercase field name causes stream PROTOCOL_ERROR")
def uppercase_header_name(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1, (b"X-Test", b"value")))
        connection.expect_rst(1, PROTOCOL_ERROR)


@case("8.2.2", "connection-specific field causes stream PROTOCOL_ERROR")
def connection_specific_header(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1, (b"connection", b"close")))
        connection.expect_rst(1, PROTOCOL_ERROR)


@case("8.3.1", "unknown pseudo-field causes stream PROTOCOL_ERROR")
def unknown_pseudo_header(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1, (b":unknown", b"value")))
        connection.expect_rst(1, PROTOCOL_ERROR)


@case("8.1.1", "declared content length must match DATA length")
def content_length_mismatch(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        connection.send(request_headers(1, (b"content-length", b"1")))
        connection.expect_rst(1, PROTOCOL_ERROR)


@case("8.4", "client PUSH_PROMISE causes connection PROTOCOL_ERROR")
def push_promise_from_client(host: str, port: int) -> None:
    with H2Connection(host, port) as connection:
        payload = (2).to_bytes(4, "big") + request_block()
        connection.send(frame(PUSH_PROMISE, END_HEADERS, 1, payload))
        connection.expect_goaway(PROTOCOL_ERROR)


def reserve_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind(("127.0.0.1", 0))
        return int(listener.getsockname()[1])


def wait_until_ready(process: subprocess.Popen[bytes], port: int) -> None:
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"conformance server exited with {process.returncode}")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("conformance server did not start within 15 seconds")


def run(host: str, port: int) -> int:
    failures = 0
    for section, name, function in TESTS:
        label = f"RFC 9113 {section}: {name}"
        try:
            function(host, port)
            print(f"[PASS] {label}")
        except Exception as error:  # one case must not hide the remainder of the suite
            failures += 1
            print(f"[FAIL] {label}: {error}", file=sys.stderr)
    print(f"{len(TESTS)} cases, {len(TESTS) - failures} passed, {failures} failed")
    return 1 if failures else 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    port = reserve_port()
    server = subprocess.Popen([args.server, str(port)])
    try:
        wait_until_ready(server, port)
        return run("127.0.0.1", port)
    finally:
        server.terminate()
        try:
            server.wait(timeout=5)
        except subprocess.TimeoutExpired:
            server.kill()
            server.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())

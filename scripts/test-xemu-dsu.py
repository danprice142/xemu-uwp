#!/usr/bin/env python3
"""DSU protocol smoke test server for xemu's DSU client.

The harness acts as a four-slot DSU server, exercises the frontend browsing
and remote stream control extensions, and writes a machine-readable summary
for Xbox/UWP validation.
"""

from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
import time
import zlib
from pathlib import Path


DSU_PROTOCOL_VERSION = 1001
DSU_HEADER_SIZE = 20

DSU_MSG_VERSION = 0x100000
DSU_MSG_CONTROLLER = 0x100001
DSU_MSG_DATA = 0x100002
DSU_MSG_MOTOR_INFO = 0x110001
DSU_MSG_RUMBLE = 0x110002
DSU_MSG_KEYBOARD = 0x120000
DSU_MSG_MOUSE = 0x120001
DSU_MSG_COMMAND = 0x130000
DSU_MSG_STATE = 0x140000
DSU_MSG_STREAM_START = 0x150000
DSU_MSG_STREAM_STOP = 0x150001
DSU_MSG_STREAM_STATUS = 0x150002
DSU_MSG_PLAYLIST_LIST_REQUEST = 0x160000
DSU_MSG_PLAYLIST_CONTENTS_REQUEST = 0x160001
DSU_MSG_PLAYLIST_LIST = 0x160002
DSU_MSG_PLAYLIST_CONTENTS = 0x160003

XBOX_PLAYLIST = "Microsoft - Xbox.lpl"


def fixed_string(value: str, length: int) -> bytes:
    encoded = value.encode("utf-8")[: length - 1]
    return encoded + bytes(length - len(encoded))


def read_fixed_string(data: bytes, offset: int, length: int) -> str:
    return data[offset : offset + length].split(b"\0", 1)[0].decode(
        "utf-8", errors="replace"
    )


def build_packet(message_type: int, payload: bytes = b"", sender_id: int = 0) -> bytes:
    packet = bytearray(DSU_HEADER_SIZE + len(payload))
    packet[0:4] = b"DSUS"
    struct.pack_into("<HHII", packet, 4, DSU_PROTOCOL_VERSION, len(payload) + 4, 0, sender_id)
    struct.pack_into("<I", packet, 16, message_type)
    packet[DSU_HEADER_SIZE:] = payload
    struct.pack_into("<I", packet, 8, zlib.crc32(packet) & 0xFFFFFFFF)
    return bytes(packet)


def parse_packet(data: bytes) -> tuple[str, int, int, bytes] | None:
    if len(data) < DSU_HEADER_SIZE:
        return None
    magic = data[:4]
    if magic not in (b"DSUC", b"DSUS"):
        return None
    version, length, expected_crc, sender_id, message_type = struct.unpack_from(
        "<HHIII", data, 4
    )
    expected_size = length + 16
    if version != DSU_PROTOCOL_VERSION or expected_size != len(data):
        return None
    crc_data = bytearray(data)
    struct.pack_into("<I", crc_data, 8, 0)
    if (zlib.crc32(crc_data) & 0xFFFFFFFF) != expected_crc:
        return None
    return magic.decode("ascii"), sender_id, message_type, data[DSU_HEADER_SIZE:]


def controller_info(slot: int) -> bytes:
    payload = bytearray(12)
    payload[0:4] = bytes((slot, 2, 2, 1))
    payload[4:10] = bytes((0x12, 0x34, 0x56, 0x78, 0x90, slot))
    payload[10] = 5
    return bytes(payload)


def controller_frame(
    slot: int,
    packet_number: int,
    touch_active: bool,
    neutral: bool = False,
) -> bytes:
    payload = bytearray(80)
    payload[0:4] = bytes((slot, 2, 2, 1))
    payload[4:10] = bytes((0x12, 0x34, 0x56, 0x78, 0x90, slot))
    payload[10] = 5
    payload[11] = 1
    struct.pack_into("<I", payload, 12, packet_number)
    if neutral:
        struct.pack_into("<H", payload, 16, 0)
        payload[20:24] = bytes((128, 128, 128, 128))
    else:
        struct.pack_into("<H", payload, 16, 1 << slot)
        payload[20:24] = bytes((128 + slot * 8, 128, 128, 128))
        payload[24 + slot] = 96
        payload[34] = slot * 24
        payload[35] = slot * 20
        payload[36] = 1 if touch_active else 0
        payload[37] = slot
        struct.pack_into("<hh", payload, 38, 4800 + slot * 1200, 3600)
    timestamp_us = int(time.time() * 1_000_000)
    struct.pack_into("<Q", payload, 48, timestamp_us)
    struct.pack_into("<fff", payload, 56, 0.1 * slot, 0.2, 1.0)
    struct.pack_into("<fff", payload, 68, 0.40 + 0.05 * slot, 0.60, 0.10)
    return bytes(payload)


def keyboard_payload(slot: int, key_code: int, pressed: bool) -> bytes:
    payload = bytearray(51)
    payload[0] = slot
    struct.pack_into("<I", payload, 3, key_code if pressed else 0)
    if pressed and 0 <= key_code < 352:
        word = key_code // 32
        bit = key_code % 32
        struct.pack_into("<I", payload, 7 + word * 4, 1 << bit)
    return bytes(payload)


def mouse_payload(slot: int) -> bytes:
    return struct.pack("<BhhhhhhH", slot, 8, -4, 0, 0, 960, 540, 1)


def launch_payload(token: str) -> bytes:
    return struct.pack("<I", 1) + fixed_string(token, 256) + fixed_string("xemu", 256)


class DsuHarness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # Do not allow a second DSU harness to share this endpoint. Windows
        # otherwise permits multiple SO_REUSEADDR UDP sockets and can deliver
        # xemu subscriptions to a different process than the one sending
        # controller frames or stream commands.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.sock.bind((args.bind, args.port))
        self.sock.setblocking(False)
        self.stream_sock: socket.socket | None = None
        self.stream_capture = None
        self.stream_datagrams = 0
        self.stream_bytes = 0
        self.stream_sources: set[tuple[str, int]] = set()
        self.stream_sync_errors = 0
        if args.capture_output:
            capture_path = Path(args.capture_output)
            capture_path.parent.mkdir(parents=True, exist_ok=True)
            self.stream_capture = capture_path.open("wb")
            self.stream_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            # H.264 encoders emit an entire I-frame at once. Keep enough
            # kernel buffering that a short access-unit burst is not dropped
            # while Python writes the previous datagrams to disk.
            self.stream_sock.setsockopt(
                socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024
            )
            if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                self.stream_sock.setsockopt(
                    socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1
                )
            self.stream_sock.bind((args.bind, args.capture_port))
            self.stream_sock.setblocking(False)
        self.client: tuple[str, int] | None = None
        self.connected_at: float | None = None
        self.packet_number = 0
        self.last_frame_at = 0.0
        self.subscriptions: set[int] = set()
        self.schedule_sent: set[str] = set()
        self.crc_or_parse_errors = 0
        self.message_counts: dict[str, int] = {}
        self.frontend_states: list[dict[str, object]] = []
        self.stream_statuses: list[dict[str, object]] = []
        self.playlist_lists: list[dict[str, object]] = []
        self.playlist_contents: list[dict[str, object]] = []
        self.rumble: list[dict[str, int]] = []

    def log(self, message: str) -> None:
        stamp = time.strftime("%H:%M:%S")
        print(f"[{stamp}] {message}", flush=True)

    def send(self, message_type: int, payload: bytes = b"") -> None:
        if self.client is None:
            return
        self.sock.sendto(build_packet(message_type, payload, self.args.sender_id), self.client)

    def handle_packet(self, data: bytes, source: tuple[str, int]) -> None:
        parsed = parse_packet(data)
        if parsed is None:
            self.crc_or_parse_errors += 1
            self.log(f"invalid packet from {source[0]}:{source[1]} length={len(data)}")
            return
        magic, _sender_id, message_type, payload = parsed
        name = f"0x{message_type:06x}"
        self.message_counts[name] = self.message_counts.get(name, 0) + 1
        if self.client != source:
            self.client = source
            self.connected_at = time.monotonic()
            self.log(f"xemu DSU client connected from {source[0]}:{source[1]}")
        if magic != "DSUC":
            self.log(f"unexpected packet magic {magic} for {name}")

        if message_type == DSU_MSG_VERSION:
            self.send(DSU_MSG_VERSION, struct.pack("<H", DSU_PROTOCOL_VERSION))
        elif message_type == DSU_MSG_CONTROLLER:
            slots: list[int] = []
            if len(payload) >= 4:
                count = min(struct.unpack_from("<I", payload)[0], 4, max(0, len(payload) - 4))
                slots = [payload[4 + i] for i in range(count) if payload[4 + i] < 4]
            for slot in slots or sorted(self.subscriptions) or [0]:
                self.send(DSU_MSG_CONTROLLER, controller_info(slot))
        elif message_type == DSU_MSG_DATA and len(payload) >= 3:
            flags, requested_slot = payload[0], payload[2]
            if flags == 0:
                self.subscriptions.update(range(4))
            elif requested_slot < 4:
                was_new = requested_slot not in self.subscriptions
                self.subscriptions.add(requested_slot)
                if was_new:
                    self.log(f"slot {requested_slot} subscribed; slots={sorted(self.subscriptions)}")
                self.send(DSU_MSG_CONTROLLER, controller_info(requested_slot))
        elif message_type == DSU_MSG_STATE and len(payload) >= 193:
            state = {
                "flags": payload[0],
                "game": read_fixed_string(payload, 1, 64),
                "platform": read_fixed_string(payload, 65, 64),
                "core": read_fixed_string(payload, 129, 64),
            }
            if not self.frontend_states or self.frontend_states[-1] != state:
                self.frontend_states.append(state)
                self.log(f"frontend state: {state}")
        elif message_type == DSU_MSG_STREAM_STATUS and len(payload) >= 16:
            state, error, stream_type = struct.unpack_from("<III", payload)
            status: dict[str, object] = {
                "state": state,
                "error": error,
                "stream_type": stream_type,
                "url": read_fixed_string(payload, 16, min(256, max(0, len(payload) - 16))),
            }
            if len(payload) >= 280:
                width, height, aspect = struct.unpack_from("<HHf", payload, 272)
                status.update(width=width, height=height, aspect_ratio=aspect)
            if not self.stream_statuses or self.stream_statuses[-1] != status:
                self.stream_statuses.append(status)
                self.log(f"stream status: {status}")
        elif message_type == DSU_MSG_PLAYLIST_LIST and len(payload) >= 3:
            entries = []
            offset = 3
            for _ in range(payload[2]):
                if offset + 320 > len(payload):
                    break
                entries.append(
                    {
                        "name": read_fixed_string(payload, offset, 64),
                        "path": read_fixed_string(payload, offset + 64, 256),
                    }
                )
                offset += 320
            response = {"page": payload[0], "total_pages": payload[1], "entries": entries}
            self.playlist_lists.append(response)
            self.log(f"playlist list: {response}")
        elif message_type == DSU_MSG_PLAYLIST_CONTENTS and len(payload) >= 3:
            entries = []
            offset = 3
            for _ in range(payload[2]):
                if offset + 576 > len(payload):
                    break
                entries.append(
                    {
                        "label": read_fixed_string(payload, offset, 64),
                        "token": read_fixed_string(payload, offset + 64, 256),
                        "core": read_fixed_string(payload, offset + 320, 256),
                    }
                )
                offset += 576
            response = {"page": payload[0], "total_pages": payload[1], "entries": entries}
            self.playlist_contents.append(response)
            self.log(f"playlist contents: count={len(entries)} entries={entries[:3]}")
        elif message_type == DSU_MSG_RUMBLE and len(payload) >= 10:
            event = {
                "flags": payload[0],
                "slot": payload[1],
                "motor": payload[8],
                "intensity": payload[9],
            }
            self.rumble.append(event)
            self.log(f"rumble: {event}")

    def scheduled_send(self, name: str, delay: float, message_type: int, payload: bytes) -> None:
        if name in self.schedule_sent or self.connected_at is None:
            return
        if time.monotonic() - self.connected_at < delay:
            return
        self.schedule_sent.add(name)
        self.send(message_type, payload)
        self.log(f"sent {name}")

    def drive_schedule(self) -> None:
        if not self.args.stream_only:
            self.scheduled_send("playlist list request", 1.0, DSU_MSG_PLAYLIST_LIST_REQUEST, b"\0")
            self.scheduled_send(
                "playlist contents request",
                1.5,
                DSU_MSG_PLAYLIST_CONTENTS_REQUEST,
                b"\0" + XBOX_PLAYLIST.encode("utf-8"),
            )
            self.scheduled_send(
                "keyboard press",
                2.0,
                DSU_MSG_KEYBOARD,
                keyboard_payload(0, self.args.keyboard_key, True),
            )
            self.scheduled_send(
                "keyboard release",
                2.2,
                DSU_MSG_KEYBOARD,
                keyboard_payload(0, self.args.keyboard_key, False),
            )
            self.scheduled_send("mouse/lightgun sample", 2.4, DSU_MSG_MOUSE, mouse_payload(0))
            if self.args.launch_token:
                self.scheduled_send(
                    "frontend launch command",
                    3.0,
                    DSU_MSG_COMMAND,
                    launch_payload(self.args.launch_token),
                )

        stream_payload = (
            struct.pack("<I", 4)
            + fixed_string(self.args.stream_url, 256)
            + struct.pack(
                "<IHHB", self.args.bitrate_kbps, self.args.width, self.args.height, self.args.fps
            )
        )
        if not self.args.input_only:
            self.scheduled_send(
                "main stream start", self.args.start_delay, DSU_MSG_STREAM_START, stream_payload
            )

        if not self.args.input_only and not self.args.leave_stream_running:
            stop_delay = max(self.args.start_delay + 8.0, self.args.duration - 3.0)
            self.scheduled_send("main stream stop", stop_delay, DSU_MSG_STREAM_STOP, b"")

    def send_controller_frames(self) -> None:
        now = time.monotonic()
        if self.client is None or now - self.last_frame_at < 1.0 / 60.0:
            return
        self.last_frame_at = now
        self.packet_number = (self.packet_number + 1) & 0xFFFFFFFF
        for slot in sorted(self.subscriptions):
            touch_active = slot == 0 and (self.packet_number // 60) % 2 == 0
            self.send(
                DSU_MSG_DATA,
                controller_frame(
                    slot,
                    self.packet_number,
                    touch_active,
                    neutral=self.args.stream_only or self.args.neutral_controllers,
                ),
            )

    def receive_stream(self) -> None:
        if self.stream_sock is None or self.stream_capture is None:
            return
        while True:
            try:
                data, source = self.stream_sock.recvfrom(65535)
            except BlockingIOError:
                break
            self.stream_datagrams += 1
            self.stream_bytes += len(data)
            self.stream_sources.add(source)
            if not data or len(data) % 188 != 0 or any(
                data[offset] != 0x47 for offset in range(0, len(data), 188)
            ):
                self.stream_sync_errors += 1
            self.stream_capture.write(data)

    def summary(self) -> dict[str, object]:
        active_status = any(
            status.get("state") == 1 and status.get("error") == 0
            for status in self.stream_statuses
        )
        result: dict[str, object] = {
            "connected_client": list(self.client) if self.client else None,
            "subscribed_slots": sorted(self.subscriptions),
            "all_four_slots": self.subscriptions == {0, 1, 2, 3},
            "crc_or_parse_errors": self.crc_or_parse_errors,
            "message_counts": self.message_counts,
            "frontend_states": self.frontend_states,
            "stream_statuses": self.stream_statuses,
            "stream_active_observed": active_status,
            "playlist_lists": self.playlist_lists,
            "playlist_contents": self.playlist_contents,
            "rumble": self.rumble,
            "stream_capture": {
                "datagrams": self.stream_datagrams,
                "bytes": self.stream_bytes,
                "sources": [list(source) for source in sorted(self.stream_sources)],
                "ts_sync_errors": self.stream_sync_errors,
            },
        }
        result["passed"] = bool(
            result["all_four_slots"]
            and self.crc_or_parse_errors == 0
            and (
                self.args.input_only
                or (
                    active_status
                    and (
                        self.args.stream_only
                        or (self.playlist_lists and self.playlist_contents)
                    )
                )
            )
        )
        return result

    def run(self) -> int:
        self.log(f"DSU test server ready on {self.args.bind}:{self.args.port}")
        deadline = time.monotonic() + self.args.duration
        while time.monotonic() < deadline:
            while True:
                try:
                    data, source = self.sock.recvfrom(4096)
                except BlockingIOError:
                    break
                self.handle_packet(data, source)
            self.receive_stream()
            self.send_controller_frames()
            self.drive_schedule()
            time.sleep(0.002)

        result = self.summary()
        if self.stream_capture is not None:
            self.stream_capture.flush()
            self.stream_capture.close()
            self.stream_capture = None
        if self.stream_sock is not None:
            self.stream_sock.close()
            self.stream_sock = None
        rendered = json.dumps(result, indent=2, sort_keys=True)
        if self.args.output:
            output = Path(self.args.output)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(rendered + "\n", encoding="utf-8")
        print(rendered, flush=True)
        return 0 if result["passed"] else 2


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=26760)
    parser.add_argument("--duration", type=float, default=45.0)
    parser.add_argument("--start-delay", type=float, default=4.0)
    parser.add_argument("--stream-url", default="udp://192.168.1.121:26960")
    parser.add_argument("--bitrate-kbps", type=int, default=3000)
    # Zero requests server-selected geometry. xemu deliberately derives the
    # encoded size from its composed display output instead of these fields.
    parser.add_argument("--width", type=int, default=0)
    parser.add_argument("--height", type=int, default=0)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--sender-id", type=lambda value: int(value, 0), default=0x52445355)
    parser.add_argument("--output")
    parser.add_argument("--capture-output")
    parser.add_argument("--capture-port", type=int, default=26960)
    parser.add_argument(
        "--keyboard-key",
        type=lambda value: int(value, 0),
        default=4,
        help="SDL scancode sent by the keyboard probe (default: 4 / A)",
    )
    parser.add_argument(
        "--launch-token",
        help="launch an allowlisted xemu-library token through the DSU frontend launch command",
    )
    parser.add_argument("--leave-stream-running", action="store_true")
    parser.add_argument(
        "--neutral-controllers",
        action="store_true",
        help="keep DSU controller frames neutral while exercising frontend extensions",
    )
    parser.add_argument("--input-only", action="store_true")
    parser.add_argument(
        "--stream-only",
        action="store_true",
        help="start/stop and capture the stream without frontend, keyboard, or mouse probes",
    )
    return parser.parse_args()


if __name__ == "__main__":
    try:
        raise SystemExit(DsuHarness(parse_args()).run())
    except KeyboardInterrupt:
        raise SystemExit(130)

#!/usr/bin/env python3
"""Send a safe, neutral DSU sequence for reaching Xbox gameplay in xemu.

This script is a small DSU/Cemuhook server. It waits until xemu subscribes to
the selected controller slot, sends Start followed by A pulses, returns to a
fully neutral controller state between every press, and exits after a final
neutral interval. It does not read or modify xemu configuration or app data.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import select
import socket
import struct
import sys
import time
import zlib


DSU_PROTOCOL_VERSION = 1001
DSU_HEADER_SIZE = 20
DSU_MAX_SLOTS = 4

DSU_MSG_VERSION = 0x100000
DSU_MSG_CONTROLLER = 0x100001
DSU_MSG_DATA = 0x100002

DSU_SUB_ALL_CONTROLLERS = 0
DSU_SUB_SLOT_BASED = 1
DSU_SUB_MAC_BASED = 2

XEMU_DSU_DATA_GAMEPAD = 1 << 0

DSU_BUTTON_START = 0x0008  # DSU Options -> Xbox Start
DSU_BUTTON_B = 0x2000      # DSU Circle -> Xbox B
DSU_BUTTON_A = 0x4000      # DSU Cross -> Xbox A


@dataclass(frozen=True)
class ButtonEvent:
    start: float
    duration: float
    name: str
    mask: int

    @property
    def end(self) -> float:
        return self.start + self.duration


def build_packet(message_type: int, payload: bytes, sender_id: int) -> bytes:
    packet = bytearray(DSU_HEADER_SIZE + len(payload))
    packet[0:4] = b"DSUS"
    struct.pack_into(
        "<HHII",
        packet,
        4,
        DSU_PROTOCOL_VERSION,
        len(payload) + 4,
        0,
        sender_id,
    )
    struct.pack_into("<I", packet, 16, message_type)
    packet[DSU_HEADER_SIZE:] = payload
    struct.pack_into("<I", packet, 8, zlib.crc32(packet) & 0xFFFFFFFF)
    return bytes(packet)


def parse_client_packet(data: bytes) -> tuple[int, bytes] | None:
    if len(data) < DSU_HEADER_SIZE or data[0:4] != b"DSUC":
        return None

    version, length, expected_crc, _sender_id, message_type = struct.unpack_from(
        "<HHIII", data, 4
    )
    expected_size = length + 16
    if (
        version != DSU_PROTOCOL_VERSION
        or length < 4
        or expected_size != len(data)
    ):
        return None

    crc_data = bytearray(data)
    struct.pack_into("<I", crc_data, 8, 0)
    if (zlib.crc32(crc_data) & 0xFFFFFFFF) != expected_crc:
        return None
    return message_type, data[DSU_HEADER_SIZE:]


def controller_mac(slot: int) -> bytes:
    return bytes((0x12, 0x34, 0x56, 0x78, 0x90, slot & 0xFF))


def controller_info(slot: int) -> bytes:
    payload = bytearray(12)
    payload[0:4] = bytes((slot, 2, 2, 1))
    payload[4:10] = controller_mac(slot)
    payload[10] = 5
    return bytes(payload)


def controller_frame(slot: int, packet_number: int, buttons: int) -> bytes:
    """Build an 80-byte DSU frame with every non-button input neutral."""
    payload = bytearray(80)
    payload[0:4] = bytes((slot, 2, 2, 1))
    payload[4:10] = controller_mac(slot)
    payload[10] = 5
    payload[11] = 1
    struct.pack_into("<I", payload, 12, packet_number)
    struct.pack_into("<H", payload, 16, buttons & 0xFFFF)
    payload[20:24] = bytes((128, 128, 128, 128))
    struct.pack_into("<Q", payload, 48, int(time.time() * 1_000_000))
    return bytes(payload)


def build_schedule(args: argparse.Namespace) -> list[ButtonEvent]:
    pulse_seconds = args.pulse_ms / 1000.0
    if args.raw_mask is not None:
        return [
            ButtonEvent(
                args.start_delay + index * args.raw_interval,
                pulse_seconds,
                f"RAW {index + 1}/{args.raw_count} 0x{args.raw_mask:04x}",
                args.raw_mask,
            )
            for index in range(args.raw_count)
        ]
    events = []
    if not args.skip_start:
        events.append(
            ButtonEvent(args.start_delay, pulse_seconds, "START", DSU_BUTTON_START)
        )

    first_a = args.start_delay + (0 if args.skip_start else args.a_delay)
    for index in range(args.a_count):
        events.append(
            ButtonEvent(
                first_a + index * args.a_interval,
                pulse_seconds,
                f"A {index + 1}/{args.a_count}",
                DSU_BUTTON_A,
            )
        )

    if args.resume_with_b:
        previous_end = max((event.end for event in events),
                           default=args.start_delay)
        events.append(
            ButtonEvent(
                previous_end + args.resume_delay,
                pulse_seconds,
                "B resume",
                DSU_BUTTON_B,
            )
        )
    return events


def print_schedule(args: argparse.Namespace, events: list[ButtonEvent]) -> None:
    print(
        f"DSU navigation schedule for slot {args.player_slot}; "
        "time starts when that slot subscribes:"
    )
    for event in events:
        print(
            f"  +{event.start:7.3f}s  {event.name:<12} "
            f"mask=0x{event.mask:04x} for {event.duration * 1000:.0f} ms"
        )
    finish = max(event.end for event in events) + args.post_delay
    print(f"  +{finish:7.3f}s  complete after final neutral interval")


class DSUNavigator:
    def __init__(self, args: argparse.Namespace, events: list[ButtonEvent]) -> None:
        self.args = args
        self.events = events
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        # A DSU server must own its UDP port exclusively. On Windows,
        # SO_REUSEADDR permits two live UDP servers to bind the same endpoint;
        # subscriptions can then reach one process while controller replies
        # come from the other. That looks like a successful subscription but
        # produces no game input. Fail loudly instead when another harness is
        # still using the configured port.
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        self.sock.bind((args.host, args.port))
        # A closed synthetic UDP client can otherwise raise WinError 10054 on
        # the next recvfrom and terminate an otherwise healthy DSU server.
        if hasattr(socket, "SIO_UDP_CONNRESET"):
            self.sock.ioctl(socket.SIO_UDP_CONNRESET, False)
        self.sock.setblocking(False)

        self.client: tuple[str, int] | None = None
        self.subscribed_slots: set[int] = set()
        self.subscription_data_types: dict[int, int] = {}
        self.missing_gamepad_reported: set[int] = set()
        self.armed_at: float | None = None
        self.packet_number = 0
        self.last_label = "neutral"
        self.next_frame_at = time.monotonic()
        self.connection_deadline = time.monotonic() + args.connection_timeout
        self.completed = False

    def log(self, message: str) -> None:
        print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)

    def source_allowed(self, source: tuple[str, int]) -> bool:
        if self.args.client_host and source[0] != self.args.client_host:
            return False
        return self.client is None or source == self.client

    def send(self, message_type: int, payload: bytes = b"") -> None:
        if self.client is None:
            return
        packet = build_packet(message_type, payload, self.args.sender_id)
        try:
            self.sock.sendto(packet, self.client)
        except OSError as exc:
            self.log(f"warning: UDP send failed: {exc}")

    def subscribe(self, slot: int, data_types: int, now: float) -> None:
        if not 0 <= slot < DSU_MAX_SLOTS:
            return
        previous_data_types = self.subscription_data_types.get(slot)
        self.subscription_data_types[slot] = data_types
        if slot not in self.subscribed_slots:
            self.subscribed_slots.add(slot)
            self.log(
                f"slot {slot} subscribed data_types=0x{data_types:02x}; "
                f"slots={sorted(self.subscribed_slots)}"
            )
        elif previous_data_types != data_types:
            self.log(
                f"slot {slot} subscription data_types changed "
                f"to 0x{data_types:02x}"
            )
        self.send(DSU_MSG_CONTROLLER, controller_info(slot))
        if slot == self.args.player_slot and self.armed_at is None:
            if not (data_types & XEMU_DSU_DATA_GAMEPAD):
                if slot not in self.missing_gamepad_reported:
                    self.log(
                        f"target slot {slot} did not request gamepad data; "
                        "set input.dsu.player1.gamepad = true"
                    )
                    self.missing_gamepad_reported.add(slot)
                return
            self.armed_at = now
            self.next_frame_at = now
            self.log(f"navigation sequence armed for player slot {slot}")

    def handle_packet(self, data: bytes, source: tuple[str, int], now: float) -> None:
        parsed = parse_client_packet(data)
        if parsed is None or not self.source_allowed(source):
            return
        if self.client is None:
            self.client = source
            self.log(f"xemu DSU client locked to {source[0]}:{source[1]}")

        message_type, payload = parsed
        if message_type == DSU_MSG_VERSION:
            self.send(DSU_MSG_VERSION, struct.pack("<H", DSU_PROTOCOL_VERSION))
        elif message_type == DSU_MSG_CONTROLLER:
            slots: list[int] = []
            if len(payload) >= 4:
                count = min(
                    struct.unpack_from("<I", payload)[0],
                    DSU_MAX_SLOTS,
                    max(0, len(payload) - 4),
                )
                slots = [
                    payload[4 + index]
                    for index in range(count)
                    if payload[4 + index] < DSU_MAX_SLOTS
                ]
            for slot in slots or [self.args.player_slot]:
                self.send(DSU_MSG_CONTROLLER, controller_info(slot))
        elif message_type == DSU_MSG_DATA and len(payload) >= 3:
            flags = payload[0]
            data_types = payload[1]
            requested_slot = payload[2]
            if flags == DSU_SUB_ALL_CONTROLLERS:
                for slot in range(DSU_MAX_SLOTS):
                    self.subscribe(slot, data_types, now)
            elif flags == DSU_SUB_SLOT_BASED:
                self.subscribe(requested_slot, data_types, now)
            elif flags == DSU_SUB_MAC_BASED:
                self.subscribe(self.args.player_slot, data_types, now)

    def current_input(self, now: float) -> tuple[int, str]:
        if self.armed_at is None:
            return 0, "neutral"
        elapsed = now - self.armed_at
        for event in self.events:
            if event.start <= elapsed < event.end:
                return event.mask, event.name
        return 0, "neutral"

    def update_transition_log(self, label: str, mask: int) -> None:
        if label == self.last_label:
            return
        if label == "neutral":
            self.log("player slot returned to neutral")
        else:
            self.log(f"player slot {self.args.player_slot}: {label} down (0x{mask:04x})")
        self.last_label = label

    def send_frames(self, now: float) -> None:
        if self.client is None or not self.subscribed_slots:
            return
        self.packet_number = (self.packet_number + 1) & 0xFFFFFFFF
        mask, label = self.current_input(now)
        self.update_transition_log(label, mask)
        for slot in sorted(self.subscribed_slots):
            slot_mask = mask if slot == self.args.player_slot else 0
            self.send(DSU_MSG_DATA, controller_frame(slot, self.packet_number, slot_mask))

        if self.armed_at is not None:
            finish = max(event.end for event in self.events) + self.args.post_delay
            if now - self.armed_at >= finish:
                self.completed = True

    def send_final_neutral(self) -> None:
        if self.client is None or not self.subscribed_slots:
            return
        for _ in range(6):
            self.packet_number = (self.packet_number + 1) & 0xFFFFFFFF
            for slot in sorted(self.subscribed_slots):
                self.send(DSU_MSG_DATA, controller_frame(slot, self.packet_number, 0))
            time.sleep(1.0 / self.args.frame_rate)

    def run(self) -> int:
        self.log(
            f"safe DSU navigation server listening on {self.args.host}:{self.args.port}"
        )
        if self.args.client_host:
            self.log(f"accepting only xemu client host {self.args.client_host}")
        self.log("all controller inputs remain neutral until the target slot subscribes")

        frame_interval = 1.0 / self.args.frame_rate
        try:
            while not self.completed:
                now = time.monotonic()
                if self.armed_at is None and now >= self.connection_deadline:
                    self.log("timed out waiting for the target DSU slot subscription")
                    return 2

                wait_for = max(0.0, min(0.1, self.next_frame_at - now))
                ready, _, _ = select.select([self.sock], [], [], wait_for)
                if ready:
                    while True:
                        try:
                            data, source = self.sock.recvfrom(4096)
                        except BlockingIOError:
                            break
                        except ConnectionResetError:
                            continue
                        self.handle_packet(data, source, time.monotonic())

                now = time.monotonic()
                if now >= self.next_frame_at:
                    self.send_frames(now)
                    self.next_frame_at = now + frame_interval

            self.log("navigation sequence completed with controller neutral")
            return 0
        finally:
            self.send_final_neutral()
            self.sock.close()


def positive_float(value: str) -> float:
    parsed = float(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def nonnegative_float(value: str) -> float:
    parsed = float(value)
    if parsed < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return parsed


def slot_number(value: str) -> int:
    parsed = int(value)
    if not 0 <= parsed < DSU_MAX_SLOTS:
        raise argparse.ArgumentTypeError("must be between 0 and 3")
    return parsed


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Wait for xemu's DSU player subscription, then safely send Start "
            "followed by neutral-separated A pulses."
        ),
        epilog=(
            "Example: python scripts/xemu-dsu-navigate.py --client-host "
            "192.168.1.125 --resume-with-b"
        ),
    )
    parser.add_argument(
        "--host",
        "--bind-host",
        dest="host",
        default="0.0.0.0",
        help="local address on which the DSU server listens (default: %(default)s)",
    )
    parser.add_argument("--port", type=int, default=26760, help="DSU UDP port")
    parser.add_argument(
        "--client-host",
        help="accept DSU requests only from this xemu/Xbox IPv4 address",
    )
    parser.add_argument("--player-slot", type=slot_number, default=0)
    parser.add_argument("--start-delay", type=nonnegative_float, default=2.0)
    parser.add_argument(
        "--raw-mask",
        type=lambda value: int(value, 0),
        help="send a diagnostic 16-bit DSU button mask instead of the normal sequence",
    )
    parser.add_argument("--raw-count", type=int, default=1)
    parser.add_argument("--raw-interval", type=positive_float, default=4.0)
    parser.add_argument(
        "--skip-start",
        action="store_true",
        help="omit the Start pulse (useful when already at an in-game pause menu)",
    )
    parser.add_argument(
        "--a-delay",
        type=nonnegative_float,
        default=4.0,
        help="delay from Start to the first A pulse",
    )
    parser.add_argument("--a-count", type=int, default=6)
    parser.add_argument("--a-interval", type=positive_float, default=4.0)
    parser.add_argument("--pulse-ms", type=positive_float, default=200.0)
    parser.add_argument(
        "--resume-with-b",
        action="store_true",
        help="send one B pulse after the A sequence to resume an in-game pause menu",
    )
    parser.add_argument(
        "--resume-delay",
        type=nonnegative_float,
        default=4.0,
        help="neutral delay before the optional B resume pulse",
    )
    parser.add_argument("--post-delay", type=nonnegative_float, default=1.0)
    parser.add_argument(
        "--connection-timeout",
        type=positive_float,
        default=120.0,
        help="seconds to wait for the target slot subscription",
    )
    parser.add_argument("--frame-rate", type=positive_float, default=60.0)
    parser.add_argument(
        "--sender-id", type=lambda value: int(value, 0), default=0x52445355
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="print the sequence without opening a socket or sending input",
    )
    args = parser.parse_args()
    if args.a_count < 0:
        parser.error("--a-count must not be negative")
    if args.raw_count < 1:
        parser.error("--raw-count must be at least one")
    if args.raw_mask is not None and not 0 <= args.raw_mask <= 0xFFFF:
        parser.error("--raw-mask must be between 0 and 0xffff")
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    if args.client_host:
        try:
            args.client_host = socket.gethostbyname(args.client_host)
        except OSError as exc:
            parser.error(f"could not resolve --client-host: {exc}")
    return args


def main() -> int:
    args = parse_args()
    events = build_schedule(args)
    print_schedule(args, events)
    if args.dry_run:
        print("Dry run: no UDP socket opened and no controller input sent.")
        return 0

    try:
        navigator = DSUNavigator(args, events)
    except OSError as exc:
        print(f"error: could not start DSU server: {exc}", file=sys.stderr)
        return 1
    try:
        return navigator.run()
    except KeyboardInterrupt:
        print("\nInterrupted; final neutral controller frames were sent.")
        return 130


if __name__ == "__main__":
    raise SystemExit(main())

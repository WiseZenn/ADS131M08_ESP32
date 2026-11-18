"""BLE PGA control and batched ADC recording. Python 3.10+, Bleak 2.x."""

from __future__ import annotations

import argparse
import asyncio
import csv
import math
import struct
import time
from collections.abc import Callable
from contextlib import ExitStack
from dataclasses import dataclass
from pathlib import Path
from typing import Any

DATA_UUID = "abcdef01-1234-5678-1234-56789abcdef0"
CONFIG_UUID = "abcdef02-1234-5678-1234-56789abcdef0"
CAPTURE_UUID = "abcdef03-1234-5678-1234-56789abcdef0"
DATA_PACKET = struct.Struct("<I8i")
CAPTURE_RECORD = struct.Struct("<II8i")
MAX_CAPTURE_RECORDS = 6
CONFIG_PACKET = struct.Struct("<BBBBf")
GAIN_CHOICES = tuple(1 << code for code in range(8))


@dataclass(frozen=True)
class GainConfig:
    code: int
    verified: bool
    persisted: bool
    mv_per_lsb: float

    @classmethod
    def parse(cls, payload: bytes | bytearray) -> GainConfig:
        if len(payload) != CONFIG_PACKET.size:
            raise ValueError("Expected an 8-byte PGA configuration")
        code, verified, version, flags, scale = CONFIG_PACKET.unpack(payload)
        if version != 1:
            raise ValueError(
                "Flash BLEGainControl protocol version 1 before using this client"
            )
        if code > 7 or verified > 1 or flags & ~1:
            raise ValueError("Invalid PGA configuration fields")
        expected = 1200.0 / (8388608 * (1 << code))
        if not math.isfinite(scale) or not math.isclose(scale, expected, rel_tol=1e-5):
            raise ValueError("PGA gain and input-voltage scale do not agree")
        return cls(code, bool(verified), bool(flags & 1), scale)


class GainStream:
    """Serialize PGA changes and reject samples from expired subscriptions.

    Call start() once per connection. After a failed/cancelled transition,
    disconnect and create a new instance; a late reply has no request ID.
    on_gain_changed runs before data resumes, so callers can reset filters.
    """

    def __init__(
        self,
        client: Any,
        on_data: Callable[[int, list[float]], None],
        on_gain_changed: Callable[[GainConfig], None] | None = None,
        *,
        on_capture: Callable[[int, int, list[int], GainConfig], None] | None = None,
    ) -> None:
        self.client = client
        self.on_data = on_data
        self.on_gain_changed = on_gain_changed
        self.on_capture = on_capture
        self._data_uuid = CAPTURE_UUID if on_capture is not None else DATA_UUID
        self.config: GainConfig | None = None
        self.last_change_ms: float | None = None
        self.last_gap_ms: float | None = None
        self.transition_gap_ms: float | None = None
        self._measure_transition_gap = False
        self._last_sample_at: float | None = None
        self._lock = asyncio.Lock()
        self._pending: asyncio.Future[GainConfig] | None = None
        self._generation = 0
        self._accept_data = False
        self._subscribed = False
        self._started = False
        self._failed = False

    async def start(self) -> None:
        async with self._lock:
            if self._started or self._failed:
                raise RuntimeError("Create a new GainStream for each connection")
            self._started = True
            try:
                await self.client.start_notify(CONFIG_UUID, self._receive_config)
                config = GainConfig.parse(await self.client.read_gatt_char(CONFIG_UUID))
                if self._failed or not self.client.is_connected:
                    raise ConnectionError("BLE connection changed during startup")
                self._apply_config(config)
                await self._subscribe_data()
            except (Exception, asyncio.CancelledError):
                self._invalidate()
                raise

    def _invalidate(self) -> None:
        self._generation += 1
        self._accept_data = False
        self._failed = True

    def connection_lost(self) -> None:
        self._invalidate()
        if self._pending is not None and not self._pending.done():
            self._pending.set_exception(
                ConnectionError("BLE disconnected during PGA change")
            )

    def _apply_config(self, config: GainConfig) -> None:
        if not config.verified:
            raise RuntimeError("Firmware could not verify PGA and settled ADC data")
        self.config = config
        if self.on_gain_changed is not None:
            self.on_gain_changed(config)

    async def _subscribe_data(self) -> None:
        generation = self._generation

        def receive(_sender: Any, payload: bytearray) -> None:
            if (
                generation != self._generation
                or not self._accept_data
                or self._failed
                or not self.client.is_connected
            ):
                return
            if self.on_capture is None:
                if len(payload) != DATA_PACKET.size:
                    self._invalidate()
                    raise ValueError("Expected 36-byte ADC data; check ATT MTU >= 39")
            elif (
                not CAPTURE_RECORD.size <= len(payload) <= MAX_CAPTURE_RECORDS * CAPTURE_RECORD.size
                or len(payload) % CAPTURE_RECORD.size
            ):
                self._invalidate()
                raise ValueError("Expected one to six complete 40-byte capture records")

            assert self.config is not None
            now = time.monotonic()
            if self._last_sample_at is not None:
                self.last_gap_ms = (now - self._last_sample_at) * 1000.0
                if self._measure_transition_gap:
                    self.transition_gap_ms = self.last_gap_ms
            self._measure_transition_gap = False
            self._last_sample_at = now
            if self.on_capture is None:
                packet_id, *raw = DATA_PACKET.unpack(payload)
                self.on_data(packet_id, [value * self.config.mv_per_lsb for value in raw])
            else:
                for sequence, read_us, *raw in CAPTURE_RECORD.iter_unpack(payload):
                    self.on_capture(sequence, read_us, raw, self.config)

        self._accept_data = True
        await self.client.start_notify(self._data_uuid, receive)
        self._subscribed = True
        if self._failed or not self.client.is_connected:
            raise ConnectionError(
                "BLE session became invalid while subscribing to data"
            )

    def _receive_config(self, _sender: Any, payload: bytearray) -> None:
        pending = self._pending
        if pending is None or pending.done():
            # An unsolicited/late change has no matching data boundary.
            self._invalidate()
            return
        try:
            pending.set_result(GainConfig.parse(payload))
        except ValueError as exc:
            pending.set_exception(exc)

    async def set_gain(self, code: int, timeout: float = 2.0) -> GainConfig:
        if type(code) is not int or not 0 <= code <= 7:
            raise ValueError("PGA code must be an integer in 0..7")
        if not math.isfinite(timeout) or timeout <= 0:
            raise ValueError("Timeout must be finite and positive")
        async with self._lock:
            if not self._started or self._failed or not self.client.is_connected:
                raise RuntimeError("Start a healthy BLE connection before changing PGA")
            started = time.monotonic()
            self._generation += 1
            self._accept_data = False
            self.transition_gap_ms = None
            self._measure_transition_gap = True
            try:
                config = await asyncio.wait_for(self._change_gain(code), timeout)
            except (Exception, asyncio.CancelledError):
                self._invalidate()
                raise
            finally:
                if self._pending is not None:
                    if not self._pending.done():
                        self._pending.cancel()
                    elif not self._pending.cancelled():
                        self._pending.exception()  # Consume a disconnect during a failed write.
                self._pending = None
            self.last_change_ms = (time.monotonic() - started) * 1000.0
            return config

    async def _change_gain(self, code: int) -> GainConfig:
        if self._subscribed:
            # Await the CCCD write before changing hardware; the old callback
            # generation is already invalid, including OS-queued callbacks.
            await self.client.stop_notify(self._data_uuid)
            self._subscribed = False
        self._pending = asyncio.get_running_loop().create_future()
        await self.client.write_gatt_char(CONFIG_UUID, bytes([code]), response=True)
        config = await self._pending
        if config.code != code:
            raise RuntimeError("Firmware did not confirm the requested PGA gain")
        self._apply_config(config)
        if self._failed or not self.client.is_connected:
            raise ConnectionError("BLE connection lost before data could resume")
        await self._subscribe_data()
        return config


async def main(args: argparse.Namespace) -> None:
    # Keep protocol helpers importable for tests without Bluetooth dependencies.
    from bleak import BleakClient, BleakScanner

    device = args.address or await BleakScanner.find_device_by_name(
        args.name, timeout=10.0
    )
    if device is None:
        raise RuntimeError(f"BLE device {args.name!r} was not found")
    stream: GainStream | None = None
    link_lost = asyncio.Event()

    def disconnected(_client: Any) -> None:
        link_lost.set()
        if stream is not None:
            stream.connection_lost()

    async def wait_for_segment(client: Any) -> None:
        try:
            await asyncio.wait_for(link_lost.wait(), timeout=args.seconds)
        except asyncio.TimeoutError:
            if client.is_connected and not link_lost.is_set():
                return
        raise ConnectionError("BLE disconnected before the stream finished")

    def print_data(packet_id: int, channels: list[float]) -> None:
        if packet_id % 50 == 0:
            assert stream is not None
            gap = stream.last_gap_ms or 0.0
            print(f"packet={packet_id} CH0={channels[0]:.6f} mV gap={gap:.2f} ms")

    with ExitStack() as files:
        writer = None
        if args.capture_csv is not None:
            output = files.enter_context(args.capture_csv.open("x", newline="", encoding="utf-8"))
            writer = csv.writer(output)
            writer.writerow(["sequence", "read_us", "gain_code", *(f"ch{i}" for i in range(8))])

        frames = gaps = 0
        first_received = last_received = None
        previous_sequence: int | None = None

        def capture(sequence: int, read_us: int, raw: list[int], config: GainConfig) -> None:
            nonlocal frames, gaps, previous_sequence, first_received, last_received
            last_received = time.monotonic()
            if first_received is None:
                first_received = last_received
            if previous_sequence is not None:
                delta = (sequence - previous_sequence) & 0xFFFFFFFF
                if 1 < delta < 0x80000000:
                    gaps += delta - 1
            previous_sequence = sequence
            frames += 1
            assert writer is not None
            writer.writerow([sequence, read_us, config.code, *raw])

        def gain_changed(_config: GainConfig) -> None:
            nonlocal previous_sequence
            previous_sequence = None  # Do not count intentional gain pauses as loss.

        try:
            async with BleakClient(device, disconnected_callback=disconnected) as client:
                stream = GainStream(client, print_data, gain_changed,
                                    on_capture=capture if writer is not None else None)
                await stream.start()
                assert stream.config is not None
                print(f"Initial gain: {1 << stream.config.code}X")
                await wait_for_segment(client)
                for multiplier in args.gain:
                    config = await stream.set_gain(multiplier.bit_length() - 1)
                    print(
                        f"PGA={multiplier}X confirmed in {stream.last_change_ms:.2f} ms; "
                        f"saved={config.persisted}"
                    )
                    await wait_for_segment(client)
                    print(f"Sample gap across this change: {stream.transition_gap_ms} ms")
        except ConnectionError as exc:
            if writer is not None:
                raise ConnectionError(
                    f"{exc}. Incomplete CSV retained at {args.capture_csv} ({frames} frames)"
                ) from exc
            raise

        if writer is not None:
            if not frames:
                raise RuntimeError("No capture data received; verify the capture characteristic and ATT MTU >= 43")
            elapsed = last_received - first_received
            rate = (frames - 1) / elapsed if elapsed > 0 else 0.0
            print(f"Captured {frames} frames ({rate:.1f} frames/s observed)")
            print(f"Sequence gaps within gain segments: {gaps}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", help="BLE address or macOS device UUID")
    parser.add_argument("--name", default="ESP32S3")
    parser.add_argument("--capture-csv", type=Path, help="Record all received ADC frames into a new CSV file")
    parser.add_argument("--gain", nargs="*", type=int, choices=GAIN_CHOICES, default=[])
    parser.add_argument(
        "--seconds", type=float, default=5.0, help="Stream duration per gain"
    )
    arguments = parser.parse_args()
    if not math.isfinite(arguments.seconds) or arguments.seconds <= 0:
        parser.error("--seconds must be finite and positive")
    try:
        asyncio.run(main(arguments))
    except KeyboardInterrupt:
        pass
    except ConnectionError as exc:
        parser.exit(1, f"{exc}\n")

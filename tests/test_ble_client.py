"""Exercise real client transactions against a deterministic asynchronous GATT peer."""

import asyncio
from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "examples/BLEGainControl"))
from client import CONFIG_UUID, DATA_UUID, GainConfig, GainStream  # noqa: E402


def config_payload(code=5, verified=1, version=1, flags=1):
    return struct.pack(
        "<BBBBf", code, verified, version, flags, 1200 / (8388608 * (1 << code))
    )


def data_payload(code=5, packet_id=1):
    return bytearray(struct.pack("<I8i", packet_id, *([1024 * (1 << code)] * 8)))


class FakeClient:
    def __init__(self):
        self.is_connected = True
        self.callbacks = {}
        self.expired = []
        self.calls = []
        self.read_payload = config_payload()
        self.auto_reply = True
        self.write_error = False
        self.reply = None
        self.written = asyncio.Event()

    async def start_notify(self, uuid, callback):
        self.calls.append(("start", uuid))
        self.callbacks[uuid] = callback

    async def stop_notify(self, uuid):
        self.calls.append(("stop", uuid))
        callback = self.callbacks.pop(uuid)
        self.expired.append(callback)
        callback(uuid, data_payload())  # A previously queued OS callback.

    async def read_gatt_char(self, _uuid):
        return self.read_payload

    async def write_gatt_char(self, uuid, payload, *, response):
        assert response is True
        assert DATA_UUID not in self.callbacks
        self.calls.append(("write", payload[0]))
        self.written.set()
        if self.write_error:
            raise OSError("GATT write failed")
        if self.auto_reply:
            # Reply before write_gatt_char returns, exercising the early-ACK race.
            self.callbacks[uuid](
                uuid, bytearray(self.reply or config_payload(payload[0]))
            )


class ConfigTests(unittest.TestCase):
    def test_roundtrip_all_gains(self):
        for code in range(8):
            with self.subTest(code=code):
                config = GainConfig.parse(config_payload(code))
                self.assertEqual(config.code, code)
                self.assertTrue(config.verified and config.persisted)

    def test_invalid_config(self):
        bad = [
            b"",
            b"\x00" * 7,
            config_payload(version=0),
            config_payload(verified=2),
            config_payload(flags=2),
            struct.pack("<BBBBf", 8, 1, 1, 0, 1.0),
            struct.pack("<BBBBf", 5, 1, 1, 0, float("nan")),
            struct.pack("<BBBBf", 5, 1, 1, 0, -1.0),
            struct.pack("<BBBBf", 5, 1, 1, 0, 1.0),
        ]
        for payload in bad:
            with self.subTest(payload=payload), self.assertRaises(ValueError):
                GainConfig.parse(payload)


class StreamTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.peer = FakeClient()
        self.samples = []
        self.events = []
        self.stream = GainStream(
            self.peer,
            lambda packet_id, mv: self.samples.append((packet_id, mv)),
            lambda config: self.events.append(config.code),
        )
        await self.stream.start()
        self.peer.calls.clear()

    def emit(self, code=5, packet_id=1):
        self.peer.callbacks[DATA_UUID](DATA_UUID, data_payload(code, packet_id))

    async def test_change_preserves_physical_units_and_rejects_old_callbacks(self):
        self.emit()
        initial_mv = self.samples[-1][1][0]
        await self.stream.set_gain(3)
        for old in self.peer.expired:
            old(DATA_UUID, data_payload(5, 99))
        self.assertEqual(len(self.samples), 1)
        self.emit(3, 2)
        self.assertAlmostEqual(self.samples[-1][1][0], initial_mv)
        self.assertEqual(self.events, [5, 3])
        self.assertEqual(
            self.peer.calls, [("stop", DATA_UUID), ("write", 3), ("start", DATA_UUID)]
        )
        self.assertIsNotNone(self.stream.transition_gap_ms)

    async def test_no_samples_before_completion(self):
        self.peer.auto_reply = False
        change = asyncio.create_task(self.stream.set_gain(3))
        await self.peer.written.wait()
        self.assertNotIn(DATA_UUID, self.peer.callbacks)
        self.peer.expired[0](DATA_UUID, data_payload())
        self.assertEqual(self.samples, [])
        self.peer.callbacks[CONFIG_UUID](CONFIG_UUID, config_payload(3))
        await change
        self.emit(3)
        self.assertEqual(len(self.samples), 1)

    async def test_concurrent_requests_are_serialized(self):
        await asyncio.gather(self.stream.set_gain(3), self.stream.set_gain(6))
        self.assertEqual(
            [call for call in self.peer.calls if call[0] == "write"],
            [("write", 3), ("write", 6)],
        )
        for old in self.peer.expired:
            old(DATA_UUID, data_payload())
        self.assertEqual(self.samples, [])
        self.emit(6)
        self.assertEqual(len(self.samples), 1)

    async def test_start_and_gain_change_are_serialized(self):
        peer = FakeClient()
        stream = GainStream(peer, lambda *_: None)
        reading = asyncio.Event()
        release = asyncio.Event()

        async def delayed_read(_uuid):
            reading.set()
            await release.wait()
            return config_payload()

        peer.read_gatt_char = delayed_read
        startup = asyncio.create_task(stream.start())
        await reading.wait()
        change = asyncio.create_task(stream.set_gain(3))
        await asyncio.sleep(0)
        self.assertFalse(peer.written.is_set())
        release.set()
        await asyncio.gather(startup, change)
        self.assertEqual(stream.config.code, 3)

    async def test_duplicate_ack_during_resubscription_cannot_report_success(self):
        original_subscribe = self.peer.start_notify

        async def subscribe_with_late_ack(uuid, callback):
            await original_subscribe(uuid, callback)
            if uuid == DATA_UUID:
                self.peer.callbacks[CONFIG_UUID](CONFIG_UUID, config_payload(3))

        self.peer.start_notify = subscribe_with_late_ack
        with self.assertRaises(ConnectionError):
            await self.stream.set_gain(3)
        self.emit(3)
        self.assertEqual(self.samples, [])

    async def test_verification_failure_and_wrong_gain_stop_data(self):
        for payload in (config_payload(3, verified=0), config_payload(4)):
            with self.subTest(payload=payload):
                peer = FakeClient()
                stream = GainStream(peer, lambda *_: self.fail("Unexpected sample"))
                await stream.start()
                peer.reply = payload
                with self.assertRaises(RuntimeError):
                    await stream.set_gain(3)
                self.assertNotIn(DATA_UUID, peer.callbacks)
                with self.assertRaises(RuntimeError):
                    await stream.set_gain(5)

    async def test_timeout_and_late_reply_require_new_connection(self):
        self.peer.auto_reply = False
        with self.assertRaises(asyncio.TimeoutError):
            await self.stream.set_gain(3, timeout=0.01)
        self.peer.callbacks[CONFIG_UUID](CONFIG_UUID, config_payload(3))
        self.peer.expired[0](DATA_UUID, data_payload())
        self.assertEqual(self.samples, [])
        self.assertNotIn(DATA_UUID, self.peer.callbacks)
        with self.assertRaises(RuntimeError):
            await self.stream.set_gain(5)

    async def test_write_failure_stops_data(self):
        self.peer.write_error = True
        with self.assertRaises(OSError):
            await self.stream.set_gain(3)
        self.assertNotIn(DATA_UUID, self.peer.callbacks)

    async def test_cancelled_change_cannot_resume_data(self):
        self.peer.auto_reply = False
        change = asyncio.create_task(self.stream.set_gain(3))
        await self.peer.written.wait()
        change.cancel()
        with self.assertRaises(asyncio.CancelledError):
            await change
        self.peer.callbacks[CONFIG_UUID](CONFIG_UUID, config_payload(3))
        with self.assertRaises(RuntimeError):
            await self.stream.set_gain(5)

    async def test_disconnect_wakes_pending_change(self):
        self.peer.auto_reply = False
        change = asyncio.create_task(self.stream.set_gain(3))
        await self.peer.written.wait()
        self.peer.is_connected = False
        self.stream.connection_lost()
        with self.assertRaises(ConnectionError):
            await change
        self.assertNotIn(DATA_UUID, self.peer.callbacks)

    async def test_unsolicited_config_invalidates_data(self):
        self.peer.callbacks[CONFIG_UUID](CONFIG_UUID, config_payload(3))
        self.emit(3)
        self.assertEqual(self.samples, [])

    async def test_invalid_requests_do_not_touch_connection(self):
        for code in (-1, 8, True, 2.5):
            with self.subTest(code=code), self.assertRaises(ValueError):
                await self.stream.set_gain(code)
        for timeout in (0, -1, float("nan")):
            with self.subTest(timeout=timeout), self.assertRaises(ValueError):
                await self.stream.set_gain(3, timeout)
        self.assertEqual(self.peer.calls, [])
        await self.stream.set_gain(3)

    async def test_truncated_data_stops_acceptance(self):
        with self.assertRaises(ValueError):
            self.peer.callbacks[DATA_UUID](DATA_UUID, bytearray(20))
        self.emit()
        self.assertEqual(self.samples, [])

    async def test_legacy_firmware_cannot_claim_settled_acknowledgments(self):
        peer = FakeClient()
        peer.read_payload = config_payload(version=0)
        stream = GainStream(peer, lambda *_: self.fail("Unexpected sample"))
        with self.assertRaisesRegex(ValueError, "protocol version 1"):
            await stream.start()
        self.assertNotIn(DATA_UUID, peer.callbacks)


if __name__ == "__main__":
    unittest.main()

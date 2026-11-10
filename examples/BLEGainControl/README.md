# BLE gain control

An optional application example for the common ADS131M08 driver. Select PGA
gain from a host without editing and reflashing the sketch. The ADC driver
itself does not depend on BLE, Preferences, or Python.

## Build and run

Use Arduino-ESP32 3.x on a BLE-capable board. CI targets core **3.3.11**.
The default pins match the ESP32-S3 wiring in the root README. Edit `PIN_*`
for other boards; these pins are not a classic ESP32 wiring diagram.

From the library root, with the ESP32 core installed:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 --library . examples/BLEGainControl
python -m pip install -r examples/BLEGainControl/requirements.txt
```

Upload the sketch using your normal board/port configuration. With Python 3.10+
and Bluetooth enabled, stream five seconds at each requested gain:

```sh
python examples/BLEGainControl/client.py --name ESP32S3 --gain 32 8 64 --seconds 5
```

Use `--address` instead of name discovery to select a particular board. Omitting
`--gain` only streams at the saved/default gain. The central must negotiate
**ATT MTU >= 39** for a 36-byte notification; advertising MTU 256 on the ESP32
alone does not guarantee the central accepted it. Truncated data is rejected.

## Switching without mixing gain scales

Use one central and one outstanding gain request at a time. A GATT write
response acknowledges receipt of a command, not completion of ADC setup.

1. The host invalidates its old data callback and awaits `stop_notify(Data)`.
   This writes the data CCCD before hardware changes. Already queued callbacks
   retain their old subscription generation and are rejected even after resume.
2. The host installs its completion future before writing a one-byte gain code.
   The firmware BLE callback only enqueues this code in a FreeRTOS queue.
3. `loop()` writes and reads back both GAIN registers. Data output is suspended.
   On failure it attempts a verified restoration, reports failure, and keeps
   output suspended until an explicit successful request.
4. The firmware drains ADC data throughout the filter guard, observes DRDY
   deasserted after the guard, then waits for a new DRDY sample. Only then does
   it save the gain, publish `gain_ok=1`, and send the config completion event.
5. The host checks that the event confirms the requested gain, updates its
   input-voltage scale, calls `on_gain_changed` to reset application filters,
   and starts a new data subscription.

There is **no 50 ms polling interval**. A missing notification is a transaction
failure, not permission to guess the scale. The client bounds the entire change
at two seconds by default; a timeout, cancellation, write error, unsolicited
completion, or disconnect invalidates the session. Disconnect and create a new
`GainStream` before retrying. The compact protocol has no request ID, so a late
completion must not be associated with a later request on the same session.

The new client requires `protocol_version=1`. Older firmware that reports
completion immediately after register verification has not promised settled
samples and is deliberately rejected.

## Timing and missing data

The ADC keeps converting; transient samples are discarded and the host pauses
its data subscription. This creates an intentional gap only when gain changes.
Do not synthesize samples or feed a repeated stale sample into a control loop.
Applications should hold/suspend their output according to their own control
policy, then resume processing after configuration and filter reset.

With the example's **8 MHz MCLK and OSR=8192**, the digital filter guard is
`ceil(16984 / 8 MHz) = 2123 us`, followed by a new sample boundary. The nominal
sample period is 2048 us. `SETTLE_CLOCKS` covers all eight OSRs, and unsigned
elapsed-time arithmetic handles `micros()` wraparound. The firmware reports
failure if no settled sample arrives within 100 ms. This timeout is not a
normal switching delay.

These values come from [TI's ADS131M08 datasheet](https://www.ti.com/lit/ds/symlink/ads131m08.pdf),
section 8.3.7.1.2/Table 8-3. Section 8.5.1.9.1 also describes the two-entry ADC
FIFO and the need to drain buffered data after a pause. The device outputs
unsettled data rather than suppressing it automatically.

BLE writes, notifications, CCCD changes, OS scheduling, and NVS add latency.
The advertised preferred connection interval is 10–15 ms, but the central chooses the
actual interval. There is no unmeasured end-to-end latency guarantee.

The client prints two separate measurements:

- `last_change_ms`: request start through confirmed config and data resubscription.
- `transition_gap_ms`: time from the last accepted old-gain sample to the first
  accepted new-gain sample. `None` means the necessary samples have not arrived.

Neither metric is overwritten by later ordinary sample intervals. The data
counter can have gaps while unsubscribed; this is expected during a transition.

## Wire protocol

All multibyte fields are little-endian. UUIDs match the RH56 sensor service:

| Attribute | UUID | Properties |
|---|---|---|
| Service | `12345678-1234-5678-1234-56789abcdef0` | — |
| Data | `abcdef01-1234-5678-1234-56789abcdef0` | Read / Notify |
| Config | `abcdef02-1234-5678-1234-56789abcdef0` | Read / Write / Notify |

Data remains **36 bytes**, Python format `<I8i`: a `uint32` packet counter and
eight sign-extended raw ADC channels. The counter resets at a new BLE connection;
it does not reset on a PGA change. Use notifications for streaming, not repeated
GATT reads. The BLE sketch does not call software `calibrate()`.

Config remains **8 bytes**, Python format `<BBBBf`:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `uint8 gain_code` | 0..7 → 1/2/4/8/16/32/64/128X |
| 1 | `uint8 gain_ok` | 1 only after register verification and settled fresh data |
| 2 | `uint8 protocol_version` | 1 for this handshake; formerly reserved |
| 3 | `uint8 flags` | Bit 0: current gain saved in NVS; other bits zero |
| 4 | `float32 mv_per_lsb` | `1200 / (2^23 × gain)` |

Write exactly **one byte**, the gain code (not the multiplier). Malformed
commands are ignored and the readable config is restored by `loop()`. A value
with `gain_ok=0` must never authorize voltage conversion or data consumption.
`gain_code` on failure is the driver's last verified cached value, not proof
that a failed rollback restored hardware state.

NVS uses namespace `ads131m08`, key `gain`, and defaults to 32X when no valid
saved gain exists. Repeated requests for the saved gain avoid a flash write.
Failed hardware verification and ADC timeouts never save a new value. If NVS
is unavailable or saving fails, the verified gain can still be used for this
boot, but `flags & 1` is zero and Serial reports the persistence failure.

## Reuse in an existing host

`GainStream` is independent of Qt and only requires a connected Bleak client
and synchronous `on_data(packet_id, input_mv)` / `on_gain_changed(config)`
callbacks. Attach `connection_lost()` to the client's disconnect callback.
Use `on_gain_changed` to clear filters before any new samples are delivered.

Legacy RH56 hosts can still decode the unchanged 36-byte data and 8-byte config;
their `<BB2xf` config parser ignores the two formerly reserved bytes. Their
existing 50 ms polling path does **not** gain the subscription boundary merely
by updating this library or sketch. Port the handshake above before using live
gain changes in a control session. Firmware with reserved bytes set to zero
continues to work with a legacy host, but not with this stricter new client.

This example is for a single trusted lab central and does not configure pairing
or authorization. Add access control when adapting writable characteristics
for a deployed product.

## Verification

From the repository root:

```sh
python -m unittest discover -s tests -v
python tests/run_native.py
```

The native tests execute the real sketch and driver with simulated SPI, DRDY,
GATT, NVS, and time. They cover settling/fresh-sample boundaries, readback and
rollback failure, ADC timeout, NVS failure and duplicate-write avoidance,
malformed requests, disconnect queue cleanup, and timer wraparound. Host tests
cover old queued callbacks, early completion, concurrent requests, input
validation, timeout/cancellation, and disconnect behavior.

These simulations do not measure real BLE/analog timing. Before relying on a
board, check a known non-clipping input across gain changes, record both timing
metrics, and check gain persistence over a power cycle.

# BLE gain control

This example offers a low-latency RH56 control stream and a separate stream
for recording ADC samples. A DRDY interrupt wakes the acquisition task.
BLE notifications run in another task, so transmitting a batch does not
pause SPI acquisition. The ADC driver itself is independent of BLE and Python.

## Build and run

Use Arduino-ESP32 **3.3.11** on a BLE-capable board.
The default pins match the ESP32-S3 wiring in the root README. Edit `PIN_*`
for other boards; these pins are not a classic ESP32 wiring diagram.

From the library root, with the ESP32 core installed:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s3 --library . examples/BLEGainControl
arduino-cli compile --fqbn esp32:esp32:esp32s3 --library . \
  --build-property compiler.cpp.extra_flags=-DADS131_BLE_USE_DMA=1 examples/BLEGainControl
python -m pip install -r examples/BLEGainControl/requirements.txt
```

Both builds use DRDY-driven acquisition. The second selects the driver's
ESP-IDF DMA transport. Use the same board configuration and test input for
both builds when comparing timing. GPIO, SPI mode 1, and the 30-byte ADC frame
are common to both. The DMA backend needs exclusive ownership of the chosen
SPI host, SPI2 by default.

Upload using your normal board and port configuration. With Python 3.10+
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

1. The host invalidates its old data callback and stops the Data or Capture subscription.
   This writes the data CCCD before hardware changes. Already queued callbacks
   retain their old subscription generation and are rejected even after resume.
2. The host installs its completion future before writing a one-byte gain code.
   The firmware BLE callback only enqueues this code in a FreeRTOS queue.
3. The acquisition task pauses both streams, empties their buffers, and writes and reads back both GAIN registers.
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

The ADC keeps converting during PGA changes. The host pauses its subscription
and the acquisition task discards samples collected while the filter settles.
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
The peripheral requests a 7.5–10 ms connection interval and a 251-byte link
data length. The negotiated values depend on the central and adapter.

The client prints two separate measurements:

- `last_change_ms`: request start through confirmed config and data resubscription.
- `transition_gap_ms`: time from the last accepted old-gain sample to the first
  accepted new-gain sample. `None` means the necessary samples have not arrived.

Neither metric is overwritten by later ordinary sample intervals. A gain change intentionally starts a new recording segment.

## Record at the ADC rate

Start with a fixed gain so a deliberate gain-change pause does not affect
the throughput measurement:

```sh
python examples/BLEGainControl/client.py --name ESP32S3 --seconds 20 --capture-csv adc.csv
```

This subscribes to Capture instead of Data. The firmware gives Capture
priority while subscribed. Use Data with the RH56 controls and Capture
with the recorder on separate connections or at separate times.

At 8 MHz CLKIN and OSR 8192 the ADC generates about **488 eight-channel
samples/s**. The legacy Data stream sends the latest sample at up to 100 Hz by
default. It is suitable for low-latency control. Its packet number counts
notifications attempted, independently of the ADC sample sequence. Define
`ADS131_BLE_LIVE_INTERVAL_US=5000` at build time to test a nominal 200 Hz control
limit with the same wire format. Check host processing and the measured rate
before using the higher control limit.

Capture sends up to **six 40-byte records** per notification, in order. At
full batches this transports about 488 samples/s in about 81 notifications/s
and about 19.5 kB/s of application payload. At ATT MTU 243 or larger, six
records fit. A smaller negotiated MTU reduces the number of records per
notification. ATT MTU 43 is the minimum for one record. If the central accepts 10 ms
and carries one full batch per event, the nominal payload capacity is 600
samples/s. Multiple notifications can share an event. Measure the actual
connection and host receive rates. The 128-record
firmware FIFO holds about 262 ms of samples at the nominal rate, plus the
batch being submitted. When the FIFO is full, new arrivals are counted and
discarded. A rejected BLE submission retains its batch for a later retry.
Gain changes or the end of a subscription clear pending data for that stream.

The CSV records sample sequence, the device's 32-bit SPI read timestamp
`read_us`, verified gain code, and eight signed raw codes. The timestamp uses
`micros()` and wraps after about 71.6 minutes. It marks read completion. The
client prints the observed receive rate and sequence gaps within each gain
segment after a completed run. A disconnect during recording stops the run
with an error and leaves the already recorded rows in the incomplete CSV.
Watch the device's serial counters at 115200 baud:

| Counter | Interpretation |
|---|---|
| `adc_read`, `adc_read_error` | Successfully read samples and failed sample-read or FIFO recovery attempts |
| `fifo_resync` | Successful two-frame ADC FIFO recoveries after late reads |
| `capture_queue_drop` | Samples dropped because the ESP32 capture FIFO was full |
| `ble_retry` | Rejected submissions retried by the firmware |
| `capture_accepted` | Capture frames accepted by the local BLE stack |
| `queued` | Frames waiting in the software FIFO or the pending batch |
| `mtu` | Observed per-peer ATT MTU |

The ADS131M08 has a two-sample hardware FIFO. The GPIO/ADC path can still
miss conversions. Those missing conversions occur before a sample sequence
is assigned, so also compare ADC read rate, read timestamps, and FIFO
recoveries. Stack acceptance does not confirm PC receipt. Compare the
serial counters with the CSV, including over long recordings and weak links.
For records that must survive a sustained link outage, use local storage
or a wired path.

## Wire protocol

All multibyte fields are little-endian. UUIDs match the RH56 sensor service:

| Attribute | UUID | Properties |
|---|---|---|
| Service | `12345678-1234-5678-1234-56789abcdef0` | — |
| Data | `abcdef01-1234-5678-1234-56789abcdef0` | Read / Notify |
| Config | `abcdef02-1234-5678-1234-56789abcdef0` | Read / Write / Notify |
| Capture | `abcdef03-1234-5678-1234-56789abcdef0` | Notify |

Data remains **36 bytes**, Python format `<I8i`: a `uint32` packet counter and
eight sign-extended raw ADC channels. The counter resets at a new BLE connection;
it does not reset on a PGA change. Use notifications for streaming, not repeated
GATT reads. The BLE sketch does not call software `calibrate()`.

Capture contains one to six concatenated `<II8i` records. Each record
contains `uint32 sequence`, `uint32 read_us`, and eight sign-extended
raw ADC channels. Accept payload lengths of 40 to 240 bytes in multiples of
40. The sequence advances on each successful normal ADC read, including
samples the recording queue drops. It starts at boot and spans BLE
connections and PGA changes.

Config remains **8 bytes**, Python format `<BBBBf`:

| Offset | Field | Meaning |
|---|---|---|
| 0 | `uint8 gain_code` | 0..7 → 1/2/4/8/16/32/64/128X |
| 1 | `uint8 gain_ok` | 1 only after register verification and settled fresh data |
| 2 | `uint8 protocol_version` | 1 for this handshake; formerly reserved |
| 3 | `uint8 flags` | Bit 0: current gain saved in NVS; other bits zero |
| 4 | `float32 mv_per_lsb` | `1200 / (2^23 × gain)` |

Write exactly **one byte**, the gain code (not the multiplier). Malformed
commands are ignored and the acquisition task restores the readable config. A value
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
malformed requests, disconnect queue cleanup, timer wraparound, ADC sample
ordering, MTU sizing, BLE retry, and FIFO overflow. Host tests
cover old queued callbacks, early completion, concurrent requests, input
validation, timeout/cancellation, and disconnect behavior.

These simulations do not measure real BLE/analog timing. Before relying on a
board, check a known non-clipping input across gain changes, record both timing
metrics, and check gain persistence over a power cycle.

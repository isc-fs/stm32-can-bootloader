# Architecture

This document describes the canonical memory map, boot flow and firmware contract
for the STM32 CAN bootloader. It is the source of truth that application firmware
must honour to be flashed and launched by this bootloader.

---

## Target hardware

- **MCU**: STM32H733ZGT6
- **Flash**: 1 MB, 8 × 128 KB sectors, bank 1 only on this variant
- **CAN peripheral**: FDCAN2 (classic CAN only for the bootloader — CAN FD is
  not used by the protocol at this stage)
- **Status LEDs**: `OK_STATUS` on PD14, `ERR_STATUS` on PD15
- **FDCAN pins**: PB12 (RX) / PB13 (TX), AF9

---

## Flash memory map

All addresses are expressed in the STM32 flash alias `0x0800_0000`.

| Sector | Start       | End         | Size   | Purpose                              |
|:------:|-------------|-------------|:------:|--------------------------------------|
| 0      | `0x08000000`| `0x0801FFFF`| 128 KB | **Bootloader** — WRP-protected       |
| 1–7    | `0x08020000`| `0x080FFFFF`| 896 KB | **Application**                      |

The last 32 bytes of the application region, `0x080FFFE0`–`0x080FFFFF`, hold a single
FLASHWORD with the application metadata record (see below).

Regions for NVM parameters, DTC storage and option-byte staging are intentionally
**not** carved out yet — they will be defined when Phases 3–4 of the roadmap are
reached. Until then, the whole span `0x08020000`–`0x080FFFE0` is available to the
application image.

All flash constants used by the bootloader live in
[`Core/Inc/bl_memmap.h`](Core/Inc/bl_memmap.h); C code must never re-declare them.

---

## Application metadata record

The bootloader refuses to launch any application without a valid metadata record.
The record is a single FLASHWORD (32 bytes, eight `uint32_t` words) at
`BL_APP_METADATA_ADDR` = `0x080FFFE0`, little-endian:

| Word | Field            | Notes                                              |
|:----:|------------------|----------------------------------------------------|
| 0    | `magic`          | Must equal `BL_APP_META_MAGIC` = `0xB007C0DE`      |
| 1    | `image_size`     | Application size in bytes, starting at `BL_APP_BASE` |
| 2    | `image_crc32`    | IEEE-802.3 CRC32 (poly `0xEDB88320`, init/xor `0xFFFFFFFF`) over the first `image_size` bytes of the application |
| 3    | `app_base`       | Must equal `BL_APP_BASE` = `0x08020000`            |
| 4    | `version`        | Host-defined firmware version word                 |
| 5–7  | reserved         | Must be `0` today; reserved for future fields      |

The record is written by the bootloader itself during `BL_CMD_WRITE_FINISH` after
a CRC match. Application images do not contain the metadata word — the host
flasher provides size, CRC and version, and the bootloader stamps the record.

---

## Boot flow

1. **Reset** — MCU starts the bootloader (vector table at `0x08000000`).
2. **Peripheral init** — clocks, GPIO, FDCAN2, status LEDs.
3. **Boot-request check** — the bootloader reads `RTC->BKP0R`:
   - If it equals `BL_BOOT_REQ_MAGIC` = `0xB00710AD`, the register is cleared and
     the bootloader stays in CAN-listen mode. This is the handshake an application
     uses to drop back into the bootloader without JTAG.
   - Otherwise the bootloader calls `Bootloader_CheckApplication()`:
     - Every field of the metadata record is validated: magic, size bounds, CRC
       (recomputed over flash), app base, and finally the application's vector
       table (SP pointing to a valid RAM region, entry inside the app region).
     - On success, a 2-second auto-jump window is armed; any CAN frame received
       in that window cancels the jump and keeps the bootloader active.
4. **Main loop** — polls FDCAN2 RX FIFO0 and dispatches frames. The auto-jump
   fires when its deadline elapses with no inbound traffic.
5. **Jump to application** — deinitialises FDCAN and HAL, stops SysTick,
   disables IRQs, relocates `SCB->VTOR` to `BL_APP_BASE`, restores MSP from the
   application's stack word, re-enables IRQs and branches to the application's
   reset vector.

A corrupt or missing application never causes a blind jump — the bootloader
lights the error LED and stays in CAN-listen mode.

---

## Bootloader-request handshake

The application can re-enter the bootloader cleanly by:

1. Writing `0xB00710AD` to `RTC->BKP0R` (backup domain access must be enabled).
2. Triggering a system reset.

The bootloader detects the magic, clears `RTC->BKP0R` so the request is
one-shot, and disables the auto-jump for that session.

---

## CAN protocol

The bootloader exposes a classic-CAN protocol on FDCAN2 — 11-bit standard IDs,
up to 8 bytes of payload per frame. CAN FD is not used by this bootloader.
Protocol constants live in [`Core/Inc/bl_proto.h`](Core/Inc/bl_proto.h).

### Frame ID layout (11-bit)

```
 bit 10 9 8 | 7 6 5 4 | 3 2 1 0
 +---------+---------+---------+
 | message |  source | dest    |
 |  type   | node ID | node ID |
 +---------+---------+---------+
```

| Field        | Bits | Notes                                                  |
|--------------|------|--------------------------------------------------------|
| message type | 10:8 | `bl_proto_type_t`; see table below                     |
| source       | 7:4  | 4-bit node ID of the transmitter; host is `0x0`        |
| destination  | 3:0  | 4-bit node ID of the receiver; `0xF` = broadcast       |

### Message types

| Value | Name       | Direction              | Description                       |
|-------|------------|------------------------|-----------------------------------|
| `0x0` | `CMD`      | host → device           | Command frame                     |
| `0x1` | `ACK`      | device → host           | Positive acknowledgement          |
| `0x2` | `NACK`     | device → host           | Negative acknowledgement + code   |
| `0x3` | `DATA`     | bidirectional           | Multi-frame payload continuation  |
| `0x4` | `NOTIFY`   | device → host           | Unsolicited event (log, DTC, …)   |
| `0x7` | `DISCOVER` | broadcast               | Discovery ping / response         |

### Node ID provisioning

Each board's 4-bit node ID is a compile-time constant, `BL_NODE_ID`, defined
in [`Core/Inc/bl_config.h`](Core/Inc/bl_config.h) and overridden per board via
the toolchain's `-DBL_NODE_ID=0x…` flag. Values `0x0` (host) and `0xF`
(broadcast) are reserved; valid bootloader IDs are `0x1..0xE`.

The compile-time approach is intentionally simple for Phase 2. An NVM-backed
override is planned for Phase 4 so the same firmware image can be provisioned
to different nodes without rebuilding.

### FDCAN hardware filtering

Two classic-mask filters are installed on FIFO0 during `Bootloader_Init`:

1. `dst == BL_NODE_ID` — unicast to this board
2. `dst == 0xF`       — broadcast to every bootloader on the bus

Non-matching standard IDs, non-matching extended IDs and all remote frames
are rejected at the peripheral and never reach software. The software
dispatcher keeps a defence-in-depth `bl_proto_addressed_to_us` check so any
future software-routed path stays honest.

### Transport layer (ISO-TP)

Every protocol frame — single- and multi-frame alike — carries a PCI byte
at byte 0 of its 8-byte payload. The high nibble names the frame kind:

```
 bit 7 6 5 4 | 3 2 1 0
 +-----------+---------+
 |   kind    |  info   |
 +-----------+---------+

 kind = 0x0  Single Frame    info = payload length (1..7)
 kind = 0x1  First Frame     info = high nibble of 12-bit total length
                              byte 1 = low byte of total length
 kind = 0x2  Consecutive     info = sequence number 0..15 (wraps)
 kind = 0x3  Flow Control    info = 0 CTS / 1 Wait / 2 Overflow
                              byte 1 = BlockSize
                              byte 2 = SeparationTime
```

TYPE ↔ PCI binding on this bus (matches the spec):

- A message's **First Frame** keeps the original TYPE (e.g. `TYPE=CMD`
  for a long command).
- **Consecutive Frames** and the receiver's **Flow Control** reply use
  `TYPE=DATA`. The bootloader's RX path is state-based and doesn't
  enforce the TYPE on CFs, but that's the convention the host is
  expected to follow.
- **Single Frames** use their own TYPE (`TYPE=CMD` for a short command,
  `TYPE=NACK` for a negative ack, etc.).

Max reassembled message size: **1024 bytes** (`BL_ISOTP_MAX_MSG` in
[`Core/Inc/bl_isotp.h`](Core/Inc/bl_isotp.h)). The ISO 15765 32-bit
escape First Frame is explicitly not supported — any attempt elicits a
`NACK(BL_NACK_TRANSPORT_ERROR)`.

On receiving a First Frame the bootloader immediately replies with
**`FC(CTS, BS=0, STmin=0)`**: block size zero tells the host it may send
every remaining Consecutive Frame without waiting for another FC, and
STmin zero imposes no inter-frame gap. No real backpressure yet — FC
throttling during flash erase is `feat/8-flash-opcodes` work.

Reassembly is bounded by a **1 s** total-elapsed timeout between the
First Frame and completion. If the deadline expires the peer gets
`NACK(BL_NACK_TRANSPORT_TIMEOUT)` and the reassembler returns to IDLE.
ISO 15765's separate N_Bs / N_Cr timers are collapsed into this single
timeout — sufficient for every bootloader use case. Any malformed PCI,
out-of-order Consecutive Frame, unexpected CF with no FF active, or
declared length over 1024 bytes produces `NACK(BL_NACK_TRANSPORT_ERROR)`.

Only one reassembly is in flight at a time. If a new First Frame or
Single Frame arrives mid-reassembly the in-flight buffer is discarded
and processing starts fresh with the new frame — consistent with the
standard's handling of an unexpected SF/FF.

### Opcodes

Every message a host sends is an ISO-TP-wrapped payload whose first
byte is the opcode. The Phase 2 set:

| Opcode | Name             | Session | Request args                                   | Response (ACK payload)                          |
|--------|------------------|:-------:|------------------------------------------------|-------------------------------------------------|
| `0x01` | `CONNECT`        |    –    | `[major, minor]`                                | `[opcode, major, minor]`                        |
| `0x02` | `DISCONNECT`     |    –    | –                                              | `[opcode]`                                      |
| `0x03` | `DISCOVER`       |    –    | – (sent to `dst=0xF`)                           | `[opcode, node_id, major, minor]` — TYPE = `DISCOVER` |
| `0x04` | `GET_FW_INFO`    |    –    | –                                              | `[opcode, <64-byte __firmware_info record>]` — multi-frame |
| `0x05` | `GET_HEALTH`     |    –    | –                                              | `[opcode, <32-byte health record>]` — multi-frame |
| `0x10` | `FLASH_ERASE`    |   ✔    | `[start_le32, length_le32]`                     | `[opcode]`                                      |
| `0x11` | `FLASH_WRITE`    |   ✔    | `[addr_le32, data…]` (≤ 256 B data)             | `[opcode]`                                      |
| `0x12` | `FLASH_READ_CRC` |   ✔    | `[addr_le32, length_le32]`                      | `[opcode, crc32_le32]`                          |
| `0x13` | `FLASH_VERIFY`   |   ✔    | `[expected_crc_le32, expected_size_le32, expected_version_le32]` | `[opcode]` — also writes the metadata word  |
| `0x30` | `LOG_STREAM_START` | ✔  | `[min_severity]`                                 | `[opcode]`; `NOTIFY_LOG` frames start flowing  |
| `0x31` | `LOG_STREAM_STOP`  | ✔  | –                                              | `[opcode]`; ring contents preserved             |
| `0x32` | `LIVE_DATA_START`  | ✔  | `[rate_hz]` (1..50)                              | `[opcode]`; `NOTIFY_LIVE_DATA` frames start flowing |
| `0x33` | `LIVE_DATA_STOP`   | ✔  | –                                              | `[opcode]`; emitter paused                      |
| `0x40` | `DTC_READ`       |    –    | –                                              | `[opcode, count_le16, entry_0, entry_1, …]` — multi-frame |
| `0x41` | `DTC_CLEAR`      |   ✔    | –                                              | `[opcode]` — table is empty after this         |
| `0x60` | `RESET`          |    –    | `[mode]` (0..3)                                 | `[opcode]` emitted **before** reset             |
| `0x61` | `JUMP`           |    –    | `[addr_le32]`                                   | `[opcode]` emitted **before** jump              |

The "Session" column marks opcodes that refuse to run unless the host
has issued a successful `CONNECT`. Flash programming is gated; identity
and hand-off opcodes are not.

#### CONNECT / DISCONNECT / DISCOVER

**CONNECT**: host offers its protocol `(major, minor)`. Bootloader
rejects a major mismatch with `NACK(BL_NACK_PROTOCOL_VERSION)`. On
success the bootloader's own `(major, minor)` is echoed back, the
internal **session latch** goes active, and the session watchdog (see
below) is armed.

**DISCONNECT** clears the session latch explicitly. An MCU reset or a
watchdog timeout also clears it.

**DISCOVER**: sent as `TYPE=DISCOVER` with `dst=0xF`. Each bootloader
on the bus replies — `TYPE=DISCOVER`, `dst=0x0` (host) — with its node
ID and protocol version. The reply is kept short (SF, 5 bytes
payload) so a bus scan stays cheap; hosts that want full identity
follow up with `GET_FW_INFO` on the specific node they care about.

**GET_FW_INFO**: returns the 64-byte `__firmware_info` record the
**application** (not the bootloader) publishes at a fixed offset. The
ACK payload is 65 bytes (opcode + record), so it travels as an
ISO-TP First Frame + nine Consecutive Frames — the first real
exercise of multi-frame TX from the bootloader. If no valid
application is installed the host gets `NACK(BL_NACK_NO_VALID_APP)`;
if the app is valid but lacks a firmware-info record,
`NACK(BL_NACK_UNSUPPORTED)`. Not session-gated — identity is
readable anytime.

##### Firmware-info record layout (64 bytes)

Stored at **`BL_APP_BASE + 0x400` = `0x08020400`**. The offset clears
an H7-sized vector table with margin; app code typically starts at
`0x500` or later.

| Offset | Size | Field                  | Notes                                               |
|:------:|:----:|------------------------|-----------------------------------------------------|
|    0   |   4  | `magic`                | `BL_FWINFO_MAGIC = 0xF14F1B00`                       |
|    4   |   4  | `record_version`       | `0xMMMMmmmm` — major in high 16 bits, minor in low  |
|    8   |   4  | `fw_version_major`     | Host-defined                                        |
|   12   |   4  | `fw_version_minor`     | Host-defined                                        |
|   16   |   4  | `fw_version_patch`     | Host-defined                                        |
|   20   |   4  | `mcu_id`               | `DBGMCU->IDCODE` value baked into the image         |
|   24   |   8  | `git_hash[8]`          | First 8 bytes of the source SHA-1                   |
|   32   |   8  | `build_timestamp`      | Unix seconds, little-endian 64-bit                  |
|   40   |  16  | `product_name[16]`     | ASCII, NUL-padded                                   |
|   56   |   8  | `reserved[2]`          | Zero until future revisions claim them              |

The bootloader accepts any record whose magic matches **and** whose
major version in `record_version` is ≥ 1 — minor revisions keep the
layout compatible. Apps express the record in C like this:

```c
#include "bl_fwinfo.h"   // header from this repo

const bl_fwinfo_t __firmware_info
    __attribute__((section(".firmware_info"), used)) = {
    .magic            = BL_FWINFO_MAGIC,
    .record_version   = BL_FWINFO_RECORD_VERSION,   // 0x00010000 (1.0)
    .fw_version_major = 1,
    .fw_version_minor = 2,
    .fw_version_patch = 3,
    .mcu_id           = 0x00000450U,                 // STM32H7x3
    .git_hash         = { 0xAA, 0xBB, 0xCC, 0xDD,
                          0x11, 0x22, 0x33, 0x44 },   // truncated SHA-1
    .build_timestamp  = 1734567890ULL,
    .product_name     = "IFS08-CE-ECU",
    .reserved         = { 0, 0 },
};
```

And their linker script pins the section at the right offset:

```
.firmware_info 0x08020400 :
{
    KEEP(*(.firmware_info))
} > FLASH
```

The bootloader never writes to this region — it's owned end-to-end by
the application. `__firmware_info` lands in the app image alongside
`.text`, gets flashed by `FLASH_WRITE`, and starts answering
`GET_FW_INFO` queries the moment the image is in place.

#### GET_HEALTH + NOTIFY_HEARTBEAT

The bootloader reports health in two shapes:

- **Unsolicited** `NOTIFY_HEARTBEAT` at **1 Hz while a session is
  active**. Idle bootloaders stay silent so a multi-node bus isn't
  flooded with heartbeats going nowhere. The heartbeat is a compact
  7-byte SF payload:

  | Byte | Field                          |
  |:---:|--------------------------------|
  | 0   | opcode = `0xF0` (`NOTIFY_HEARTBEAT`) |
  | 1   | `node_id`                       |
  | 2   | `reset_cause` (latched at boot) |
  | 3   | `flags` low byte                 |
  | 4–6 | `uptime_le24` (wraps at ~194 days) |

  Emitted with TYPE = `NOTIFY`, dst = `0x0` (host). Host-side CAN
  traces can filter for this one frame to passively monitor every
  bootloader on the bus.

- **On demand** via `CMD_GET_HEALTH` (`0x05`). Returns a 32-byte
  record with the fuller picture:

  | Offset | Size | Field                | Notes                                                  |
  |:---:|:---:|----------------------|--------------------------------------------------------|
  |  0  |  4  | `uptime_seconds`     | Full 32-bit, seconds since boot                        |
  |  4  |  4  | `reset_cause`        | One of the `BL_RESET_*` values below                   |
  |  8  |  4  | `flags`              | `BL_HEALTH_FLAG_*` bitmask (full 32-bit)               |
  | 12  |  4  | `flash_write_count`  | Reserved for Phase 4 NVM tracking; `0` today           |
  | 16  |  4  | `dtc_count`          | Populated by `feat/12-dtc`; `0` today                  |
  | 20  |  4  | `last_dtc_code`      | Populated by `feat/12-dtc`; `0` today                  |
  | 24  |  8  | `reserved`           | Zero until future revisions claim bytes                |

  ACK payload is `opcode + record` = 33 bytes, so the reply travels
  as an ISO-TP FF + 4 CFs. Not session-gated — hosts can poll health
  any time, even before `CONNECT`, to decide which node to talk to.

##### Reset cause values

Latched once from `RCC->RSR` at the start of `Bootloader_Init`;
`RCC->RSR` is cleared immediately afterwards so the next reset starts
clean. Conflict resolution favours fault causes over normal causes so
rare dual-condition scenarios still surface the more actionable code.

| Value | Name                   | Source                                    |
|:---:|------------------------|-------------------------------------------|
| `0x00` | `BL_RESET_UNKNOWN`     | No recognised flag set                    |
| `0x01` | `BL_RESET_POWER_ON`    | `RCC_RSR_PORRSTF` — fresh power           |
| `0x02` | `BL_RESET_PIN`         | `RCC_RSR_PINRSTF` — NRST line             |
| `0x03` | `BL_RESET_SOFTWARE`    | `RCC_RSR_SFTRSTF` — `NVIC_SystemReset()`  |
| `0x04` | `BL_RESET_IWDG`        | `RCC_RSR_IWDG1RSTF` — independent watchdog |
| `0x05` | `BL_RESET_WWDG`        | `RCC_RSR_WWDG1RSTF` — window watchdog     |
| `0x06` | `BL_RESET_LOW_POWER`   | `RCC_RSR_LPWRRSTF`                        |
| `0x07` | `BL_RESET_BROWNOUT`    | `RCC_RSR_BORRSTF`                         |

##### Flags bitmask

Bits 0–1 are live today. Bits 2–31 are reserved; later phases flip
them on as WRP status (`feat/15`), pending DTCs (`feat/12`), encrypted
session (`feat/19`), etc. become real.

| Bit | Name                              | When it's on                                  |
|:---:|-----------------------------------|-----------------------------------------------|
|  0  | `BL_HEALTH_FLAG_SESSION_ACTIVE`    | A session is currently established            |
|  1  | `BL_HEALTH_FLAG_VALID_APP_PRESENT` | `Bootloader_CheckApplication()` passes today |

#### DTC table (DTC_READ, DTC_CLEAR, NOTIFY_DTC)

Diagnostic trouble codes live in a dedicated table in **Backup SRAM**
at `0x38800000`. Backup SRAM survives soft resets and watchdog fires —
the exact situations where the DTC that caused a crash is most
valuable — but is lost on power loss unless the board has a coin
cell on `V_BAT`. Capacity: 32 entries × 20 bytes + 16-byte header =
656 bytes, leaving ~3.4 KB of BKPSRAM for later phases to claim.

**Why BKPSRAM** (not flash): DTCs are "faults seen since last clear",
not a permanent audit log. No flash wear, no rotating-sector
bookkeeping. A permanent audit log is a separate Phase 5+ concern.

##### Entry layout (20 bytes)

| Offset | Size | Field                          |
|:---:|:---:|---------------------------------|
|  0  |  2  | `code` — vendor-defined fault code |
|  2  |  1  | `severity` (0 info / 1 warn / 2 error / 3 fatal) |
|  3  |  1  | `occurrence_count` (saturates at 255) |
|  4  |  4  | `first_seen_uptime_seconds`    |
|  8  |  4  | `last_seen_uptime_seconds`     |
| 12  |  4  | `context_data` — e.g. failing flash address |
| 16  |  4  | `reserved` — zero                |

##### Dedup + eviction

Logging an already-present `code` bumps its `occurrence_count` (saturating
at 255) and updates `last_seen_uptime_seconds`. No `NOTIFY_DTC` is emitted
on dedupes — a chatty persistent fault would otherwise flood the bus.
When a 33rd distinct code arrives the oldest entry (slot 0) is evicted
and the rest shift down.

##### Well-known codes

Codes the bootloader emits itself. Apps / future phases are free to
allocate their own codes above `0x0100`.

| Code    | Name                        | Severity | `context_data`                |
|---------|-----------------------------|:--------:|-------------------------------|
| `0x0010` | `BL_DTC_FLASH_HW`           | error   | Failing flash address          |
| `0x0020` | `BL_DTC_SESSION_TIMEOUT`    | warn    | 0                              |

##### Wire surfaces

- **`CMD_DTC_READ` (`0x40`)** — not session-gated. Returns the whole
  table in one ACK: `[opcode, count_le16, entry_0, entry_1, …]`. Worst
  case 643 bytes, comfortably inside the 1024 B reassembly limit. Host
  gets all fault history regardless of session state.
- **`CMD_DTC_CLEAR` (`0x41`)** — session-gated (destructive).
  `NACK(BL_NACK_BAD_SESSION)` without a prior `CONNECT`. ACK
  `[opcode]` on success; the table is empty after this.
- **`NOTIFY_DTC` (`0xF1`)** — unsolicited 5-byte SF emitted when a
  *new* code appears: `[opcode, code_le16, severity, occurrence_count=1]`.
  Silent on dedupes.

##### Persistence across the BL → app jump

BKPSRAM is not cleared when the bootloader jumps to the application.
The app doesn't touch the DTC region today (it's bootloader-owned),
so on the next entry into the bootloader the table is recovered as-
is and `DTC_READ` returns the full history. The only things that
clear the table are `DTC_CLEAR` and a power cycle (without backup
battery).

#### Log streaming (LOG_STREAM_START, LOG_STREAM_STOP, NOTIFY_LOG)

The bootloader keeps a 1 KB printf-style log ring in Backup SRAM at
`0x38800400` (right after the DTC table). Ring contents survive soft
resets and watchdog fires: a host that reconnects after a crash can
`LOG_STREAM_START` and replay the last ~1 KB of bootloader log
leading up to the reboot.

##### Entry format

Same shape in the ring and on the wire inside `NOTIFY_LOG` payloads:

| Offset | Size | Field                                              |
|:---:|:---:|-----------------------------------------------------|
|  0  |  1  | `len_u8`          — length of `text`                |
|  1  |  1  | `severity_u8`     — `BL_LOG_SEV_*` (info/warn/error/fatal) |
| 2–5 |  4  | `uptime_le32`     — seconds since boot              |
| 6…  | `len` | `text`          — UTF-8, not NUL-terminated on the wire |

6-byte header + up to 120 bytes of text per entry. Entries are packed
back-to-back in the ring; a single entry can straddle the end-of-buffer
boundary.

##### Overflow policy

When a new entry doesn't fit, the **oldest entries are evicted** one at
a time until there's room. A running counter of evicted bytes is kept
and, on the next drain, a **synthetic marker entry** is prepended to
the output:

```
<LOG_OVERFLOW N bytes dropped>   severity=warn
```

The marker never lives in the ring itself — it's formatted on the fly
when draining — so the host always sees a breadcrumb for lost data.

##### Drain / emit

`bl_log_tick` runs every main-loop iteration. If streaming is active,
the ring has data, and at least `BL_LOG_MIN_EMIT_INTERVAL_MS`
(default 50 ms) has elapsed since the last emission, up to
`BL_LOG_DRAIN_BUDGET` bytes (default 256) are pulled from the ring
and shipped as one `NOTIFY_LOG` message — a single ISO-TP FF + CFs.
The rate limit caps log traffic at ~20 frames/s ≈ 5 KB/s even when
the ring is full; everything else stays buffered.

##### Severity filter

`LOG_STREAM_START` carries a `min_severity` byte. Entries below that
threshold are **consumed from the ring but not emitted** — once you
pick a verbosity, anything below it is gone for good. The intent is
that log verbosity is a host-side choice, applied as the ring is
drained. A noisy INFO stream can be filtered to WARN without changing
any bootloader behaviour.

##### Wire surfaces

- **`CMD_LOG_STREAM_START` (`0x30`)** — session-gated. 1-byte arg
  `min_severity`. Bootloader starts emitting `NOTIFY_LOG` on the next
  tick; current ring contents are replayed first, then new entries as
  they land. ACK `[opcode]`.
- **`CMD_LOG_STREAM_STOP` (`0x31`)** — session-gated. No args.
  Streaming halts immediately; ring contents are left intact for a
  future START. ACK `[opcode]`.
- **`NOTIFY_LOG` (`0xF2`)** — unsolicited. `[opcode, <entries…>]`.
  One or more entries packed back-to-back; host parses entry-by-entry
  using the length prefix in each header.

##### Persistence across the BL → app jump

Same as `bl_dtc`: BKPSRAM survives the jump. The app doesn't touch
this region today, so the ring is preserved and the host can stream
it out on the next visit to the bootloader.

##### Bootloader-side call sites wired in this branch

- `bl_log_info` on boot-up (with the latched reset cause)
- `bl_log_info` on `CONNECT` (records the host protocol version)
- `bl_log_info` / `bl_log_warn` / `bl_log_error` bracketing
  `FLASH_ERASE` (scope, success, HW failure, rejection)
- `bl_log_warn` on session watchdog timeout

More call sites accrue in later branches as more code paths become
interesting to observe.

#### Live-data streaming (LIVE_DATA_START, LIVE_DATA_STOP, NOTIFY_LIVE_DATA)

Where `GET_HEALTH` is an on-demand snapshot of coarse state,
`NOTIFY_LIVE_DATA` is a push stream at 1..50 Hz carrying a 32-byte
snapshot of bootloader-internal counters and current-state pointers.
Intended for host-side live-table / plot views during a flash
session.

##### Snapshot layout (32 bytes, natural alignment, LE)

| Offset | Size | Field                 | Notes                                                      |
|:---:|:---:|------------------------|------------------------------------------------------------|
|  0  |  4  | `uptime_ms`            | `HAL_GetTick()`; wraps at ~49 days                         |
|  4  |  2  | `frames_rx`            | Saturating uint16                                          |
|  6  |  2  | `frames_tx`            | Saturating uint16                                          |
|  8  |  2  | `nacks_sent`           | Subset of `frames_tx`                                      |
| 10  |  2  | `dtc_count`            | Mirrors `GET_HEALTH`                                       |
| 12  |  2  | `last_dtc_code`        | Mirrors `GET_HEALTH`                                       |
| 14  |  1  | `flags`                | Bits 0..3 (see below)                                      |
| 15  |  1  | `last_opcode`          | Most recent CMD opcode received by the dispatcher          |
| 16  |  4  | `last_flash_addr`      | Most recent `FLASH_ERASE` / `FLASH_WRITE` target address   |
| 20  |  4  | `isotp_rx_progress`    | Bytes reassembled in the current in-flight message, 0 = idle |
| 24  |  4  | `session_age_ms`       | ms since last RX activity; 0 when no session               |
| 28  |  4  | `reserved`             | Zero until future revisions claim                           |

Flags bits (byte 14):

| Bit | Name                               |
|:---:|------------------------------------|
|  0  | `BL_LIVE_FLAG_SESSION_ACTIVE`       |
|  1  | `BL_LIVE_FLAG_VALID_APP_PRESENT`    |
|  2  | `BL_LIVE_FLAG_LOG_STREAMING`        |
|  3  | `BL_LIVE_FLAG_LIVEDATA_STREAMING`   |

##### Rate

`LIVE_DATA_START` carries a 1-byte `rate_hz`. Values outside
`[BL_LIVE_MIN_RATE_HZ = 1, BL_LIVE_MAX_RATE_HZ = 50]` are rejected
with `NACK(BL_NACK_UNSUPPORTED)`. 50 Hz over a 500 kbps bus runs
~6 % bus utilisation — a safe ceiling that leaves room for the rest
of the protocol.

##### Wire surfaces

- **`CMD_LIVE_DATA_START` (`0x32`)** — session-gated. 1-byte arg
  `rate_hz`. ACK `[opcode]`.
- **`CMD_LIVE_DATA_STOP` (`0x33`)** — session-gated. No args. ACK
  `[opcode]`.
- **`NOTIFY_LIVE_DATA` (`0xF3`)** — unsolicited. `[opcode, <32-byte
  snapshot>]` = 33 bytes → one ISO-TP FF + 4 CFs per emission.

##### Signal-definition file

The host-side `can-flasher` tool maintains a signal-definition file
(JSON or TOML) that maps the offsets above to named fields with
types and scaling. That file is out of scope for the bootloader; the
struct layout above is the single source of truth on the device
side. Bumping the layout is a breaking change and should come with
a minor-version bump on the protocol.

##### App-side live data

This phase streams **bootloader** state only. A second mechanism for
apps to publish their own live-data fields is future work — most
likely a fixed RAM region the app writes to and the bootloader
proxies on a separate opcode, so the two streams don't interleave.

#### FLASH_ERASE / FLASH_WRITE / FLASH_READ_CRC / FLASH_VERIFY

All four operate on the writable range `[BL_APP_BASE, BL_APP_METADATA_ADDR)`.
Any address outside that range earns `NACK(BL_NACK_OUT_OF_BOUNDS)` or
`NACK(BL_NACK_PROTECTED_ADDR)` depending on whether the target is merely
out-of-bounds or overlaps the bootloader / metadata. Flash hardware
errors surface as `NACK(BL_NACK_FLASH_HW)`.

**FLASH_ERASE** requires the range to be sector-aligned — both
`start` and `length` must be multiples of `BL_FLASH_SECTOR_SIZE`
(128 KB). It's the first real exercise of ISO-TP First Frame + CF
segmentation (9 bytes of args don't fit in an SF). The handler is
**synchronous**: it blocks in `HAL_FLASHEx_Erase` for the full erase
duration (typically 1–4 s per 128 KB sector on H7) and only ACKs when
done. CAN frames that arrive during the erase queue in FDCAN FIFO0
(depth 16) and are drained once the handler returns. A real
`FC(Wait)` backpressure mechanism is deliberately **not** implemented
in this phase — the request/response pattern keeps the host from
needing to send anything during an erase.

**FLASH_WRITE** requires `addr` to be FLASHWORD-aligned (32 bytes).
Data length can be anything up to the ISO-TP reassembly limit (1024 B)
minus the 5-byte header, but the host-side spec caps per-message data
at 256 B to keep error recovery granular. A trailing partial FLASHWORD
is padded with `0xFF` internally. Each FLASHWORD can only be programmed
once between erases (H7 ECC constraint) — the host must erase the
target sectors before writing.

**FLASH_READ_CRC** computes CRC32 over `[addr, addr + length)` and
returns it in the ACK payload. Read-only but still session-gated: any
`FLASH_*` opcode requires `CONNECT` first.

**FLASH_VERIFY** recomputes CRC32 over `[BL_APP_BASE, BL_APP_BASE +
expected_size)`. On match it writes the metadata FLASHWORD at
`BL_APP_METADATA_ADDR` with `{magic, size, crc, base, version, 0, 0, 0}`
(see [Application metadata record](#application-metadata-record)).
On mismatch: `NACK(BL_NACK_CRC_MISMATCH)`; the metadata FLASHWORD
remains erased so `Bootloader_CheckApplication` will reject the image
on the next boot. The metadata sector must have been erased previously
(typically by a `FLASH_ERASE` that covered sector 7); otherwise the
program call fails with `NACK(BL_NACK_FLASH_HW)`.

#### RESET

`mode` byte picks what "reset" means.

| `mode` | Effect                                                      |
|--------|-------------------------------------------------------------|
| `0x00` | Hard reset via `NVIC_SystemReset()`                          |
| `0x01` | Soft reset — identical to 0x00 on this family                |
| `0x02` | Write `BL_BOOT_REQ_MAGIC` to `RTC->BKP0R`, then reset. The   |
|        | bootloader handshake at next boot holds in listen mode.      |
| `0x03` | Direct jump to the installed application (no reset).        |

Mode 3 is validated **before** the ACK — if `Bootloader_CheckApplication`
rejects the installed image the host gets `NACK(BL_NACK_NO_VALID_APP)`
and the bootloader stays in listen mode. Modes 0–2 always ACK first,
then trigger the reset.

#### JUMP

Jumps directly to `addr_le32`. Phase-2 policy: the address must equal
`BL_APP_BASE` and the installed app must pass integrity checks.
Out-of-range addresses earn `NACK(BL_NACK_OUT_OF_BOUNDS)`; a corrupt
or missing app earns `NACK(BL_NACK_NO_VALID_APP)`.

### Session watchdog

Once `CONNECT` succeeds the bootloader arms a watchdog timer that
fires after `BL_SESSION_TIMEOUT_MS` (default **30 000 ms**, override
with `-DBL_SESSION_TIMEOUT_MS=…`) of silence. Idle bootloaders that
have never seen a `CONNECT` are not watchdogged.

**What counts as activity.** The last-activity timestamp is refreshed
on two events:

1. Any addressed frame that passes the FDCAN filter and the TYPE / PCI
   gate in `bl_proto_dispatch`. Frames that end up NACKed count too —
   a host that's still transmitting, even unsuccessfully, is still
   alive.
2. Every ACK the bootloader transmits. This keeps long synchronous
   handlers (notably `FLASH_ERASE`, which can block for several
   seconds inside `HAL_FLASHEx_Erase`) from appearing dead on the
   first main-loop tick after they finally reply.

**What a timeout does.** On expiry the bootloader clears the session
latch, resets the ISO-TP reassembler, and then tries to auto-jump to
the installed application via `Bootloader_CheckApplication` +
`Bootloader_JumpToApplication`:

- If a valid app is installed → jump to it. The host-abandoned window
  is treated like a stretched post-boot auto-jump.
- If the app is corrupt or missing (e.g. host died mid-flash) → stay
  in listen mode. A fresh `CONNECT` from any host re-arms the session
  and lets the operation resume.

No NACK or NOTIFY is sent on timeout — the session is silent from the
bootloader's side too. The host either times out independently or
reconnects later.

**Keepalive.** The host-side protocol sends a `CMD_CONNECT` every 5 s
during long operations to keep the watchdog armed. `CMD_CONNECT` was
chosen over a dedicated keepalive opcode so there's only one session-
management surface to reason about; the ~10 bytes every 5 s is
negligible on a 500 kbps bus.

### NACK codes

| Code   | Name                          | When                                                    |
|--------|-------------------------------|---------------------------------------------------------|
| `0x01` | `BL_NACK_PROTECTED_ADDR`       | write/erase range touches sector 0 or metadata FLASHWORD |
| `0x02` | `BL_NACK_OUT_OF_BOUNDS`        | address outside the writable region                      |
| `0x03` | `BL_NACK_CRC_MISMATCH`         | `FLASH_VERIFY`: computed CRC != expected                 |
| `0x06` | `BL_NACK_BAD_SESSION`          | `FLASH_*` issued without an active session               |
| `0x07` | `BL_NACK_FLASH_HW`             | HAL flash erase / program returned non-OK                 |
| `0x08` | `BL_NACK_BUSY`                 | previous op not complete (reserved)                     |
| `0x09` | `BL_NACK_TRANSPORT_TIMEOUT`    | ISO-TP reassembly ran past `BL_ISOTP_TIMEOUT_MS`         |
| `0x0A` | `BL_NACK_TRANSPORT_ERROR`      | ISO-TP PCI / seq / overflow                              |
| `0x0B` | `BL_NACK_PROTOCOL_VERSION`     | host/device major version disagree                       |
| `0x0C` | `BL_NACK_NO_VALID_APP`         | jump / reset-to-app with no valid image                 |
| `0xFE` | `BL_NACK_UNSUPPORTED`          | unknown opcode, bad arg length, or unaligned address    |

### Protocol branch coverage

**Phase 2** — `v0.2.0-protocol` tagged:

| Branch                     | Adds                                                                                 |
|----------------------------|--------------------------------------------------------------------------------------|
| `feat/5-frame-layout`      | Frame layout, node ID, FDCAN filters, dispatch skeleton                              |
| `feat/6-isotp`             | ISO-TP-style multi-frame segmentation / reassembly                                    |
| `feat/7-core-opcodes`      | `CONNECT` / `DISCONNECT` / `DISCOVER` / `RESET` / `JUMP`                              |
| `feat/8-flash-opcodes`     | `FLASH_ERASE` / `FLASH_WRITE` / `FLASH_READ_CRC` / `FLASH_VERIFY`; session gating     |
| `feat/9-session-timeout`   | 30 s session watchdog + keepalive + retry budget                                     |

**Phase 3** — firmware contract & diagnostics, closes at `v0.3.0-diagnostics`:

| Branch                     | Adds                                                                                 |
|----------------------------|--------------------------------------------------------------------------------------|
| `feat/10-firmware-info`    | `__firmware_info` record + `GET_FW_INFO` opcode                                      |
| `feat/11-heartbeat-health` | 1 Hz `NOTIFY_HEARTBEAT` + `GET_HEALTH` 32-byte record; reset-cause latch             |
| `feat/12-dtc`              | BKPSRAM DTC table + `DTC_READ` / `DTC_CLEAR` / `NOTIFY_DTC`; auto-logged fault codes |
| `feat/13-log-stream`       | BKPSRAM log ring + `LOG_STREAM_START/STOP` + `NOTIFY_LOG`; printf-style `bl_log_*`   |
| `feat/14-live-data`        | 32-byte live snapshot + `LIVE_DATA_START/STOP` + `NOTIFY_LIVE_DATA`                  |

By the end of Phase 3 the bootloader is a fully observable node: a
host can identify the board (`GET_FW_INFO`), watch its heartbeat,
inspect fault history (`DTC_READ`), tail its log (`LOG_STREAM_START`)
and plot its counters in real time (`LIVE_DATA_START`). The
`v0.3.0-diagnostics` dev→main tag closes the phase.

---

## RAM usage (bootloader)

| Region   | Range                     | Used for                          |
|----------|---------------------------|-----------------------------------|
| DTCMRAM  | `0x20000000`–`0x2001FFFF` | Bootloader stack (top = `0x20020000`) |
| RAM_D1   | `0x24000000`–`0x2404FFFF` | `.data` / `.bss` / heap           |

The application is free to place its own stack/data in any of the H7 RAMs; the
bootloader's SP sanity check accepts a DTCM or RAM_D1 stack pointer at jump
time and will be extended as new SRAM regions come into use.

---

## Future work (not implemented)

- Option-byte read and `WRP` self-apply over the CAN protocol
- NVM parameter region
- DTC / audit log
- Rollback slots (A/B)
- Ed25519 signed firmware and replay counter
- Session authentication and optional AES-CTR transport

Each of these will carve its own region from the current app space and will be
documented here as it lands.

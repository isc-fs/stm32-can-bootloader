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

### Current opcode coverage

Phase 2 lands the protocol across five branches:

| Branch                      | Adds                                                                 |
|----------------------------|----------------------------------------------------------------------|
| `feat/5-frame-layout`      | This document; frame layout, node ID, FDCAN filters, dispatch skeleton |
| `feat/6-isotp`             | ISO-TP-style multi-frame segmentation / reassembly                    |
| `feat/7-core-opcodes`      | `CONNECT` / `DISCONNECT` / `DISCOVER` / `RESET` / `JUMP` + NACK codes |
| `feat/8-flash-opcodes`     | `FLASH_ERASE` / `FLASH_WRITE` / `FLASH_READ_CRC` / `FLASH_VERIFY`     |
| `feat/9-session-timeout`   | 30 s session watchdog + keepalive + retry budget                     |

Until `feat/7-core-opcodes` and `feat/8-flash-opcodes` merge, every
received frame is answered with a generic `NACK(0xFE)` — the bootloader
on `dev` is intentionally non-flashable during this Phase 2 window. That
window closes at the `v0.2.0-protocol` dev→main tag.

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

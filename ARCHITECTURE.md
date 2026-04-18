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

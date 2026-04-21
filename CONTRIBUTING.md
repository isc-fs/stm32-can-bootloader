# Contributing to the STM32 CAN bootloader

Conventions for working on the bootloader firmware itself — branch
naming, review process, CI hooks. If you just want to flash boards
or diagnose a brick in the pit, you want
[PROVISIONING.md](PROVISIONING.md) instead.

---

## Setup

1. GitHub account + clone this repo:
   - SSH: `git@github.com:isc-fs/stm32-can-bootloader.git`
   - HTTPS: `https://github.com/isc-fs/stm32-can-bootloader.git`
2. Toolchain: ARM GCC + CMake + Ninja (STM32CubeIDE ships all three;
   a standalone install works too). Verify with `cmake --preset
   Debug && cmake --build build/Debug` — should produce
   `build/Debug/CAN_BL.elf` with no warnings.
3. First-time Git users: [git-scm.com's official tutorial](https://git-scm.com/docs/gittutorial)
   covers everything needed below.

---

## Branches

```
main  ──●──────────────●──▶  validated releases only
        ↑              ↑
dev   ──●───●───●───●──●──▶  continuous integration
        ↑   ↑       ↑
       feat/1  fix/1  feat/2
```

- **`main`** — production. Only merges from `dev` after full bench
  validation. Don't work here.
- **`dev`** — integration. Don't work here either.
- **Feature / fix branches** are cut from `dev`, PR'd back to `dev`,
  reviewed, merged, deleted. Two name formats, both with a short
  kebab-case title so the branch purpose is visible at a glance:
  ```
  feat/<n>-<short-title>   → new functionality
  fix/<n>-<short-title>    → bug fix
  docs/<n>-<short-title>   → documentation changes only
  ```
  Counters are independent (`feat/5-…` and `fix/5-…` can coexist).
  Pick the next number by filtering issues by label and looking at
  the highest closed one.

## The tracking issue

Every branch gets an auto-created GitHub issue carrying:
- The full branch name in the title: `[feat/3-xyz] ...`
- The matching label (`feat` / `fix` / `docs`)
- A template description

A GitHub Actions workflow creates the issue on first push. If the
number is off-by-one (too high or too low), the workflow comments a
warning so you can rename before anyone else sees. The first commit
message you push auto-fills the issue description; edit by hand to
fix-up.

**Closing**: the issue closes automatically when its PR merges.
Issues are the permanent record of what each branch did — to see
past work, filter by label + state=closed.

---

## Workflow

```bash
# 1. Up-to-date dev
git checkout dev && git pull origin dev

# 2. Cut the branch (replace number + title)
git checkout -b feat/5-frame-layout

# 3. Push — triggers issue creation
git push origin feat/5-frame-layout

# 4. Work, commit, push as usual
git add <files>
git commit -m "short, imperative description"
git push

# 5. PR toward dev via gh or the web UI
gh pr create --base dev
```

In the PR description, include `Closes #<issue-number>` so merge
closes the tracking issue automatically.

**Before requesting review**, check:
- `cmake --build build/Release` is clean (no warnings, no link errors).
- You ran the change on bench if it touches anything wire-format,
  flash-programming, or session-state. If hardware was involved,
  say what you tested in the PR body.
- The PR targets `dev`, not `main`.

## Merging to main (release cut)

Only when `dev` holds a set of validated changes and someone with
bench access has signed off on a full flash-cycle test. The PR
from `dev` → `main` is the release cut — tag it and bump the
version in `bl_proto.h` so host tools see the change.

After the release is published, `sync-dev-after-release.yml`
fast-forwards `dev` to match `main` automatically. No need to
run `git checkout dev && git merge main && git push` by hand;
the workflow handles it on the `release: published` event.

---

## Where things live

- `Core/Src/bl_proto.c` — protocol dispatch, ISO-TP glue, session
  latch, opcode handlers. The hot path.
- `Core/Src/bl_flash.c` — HAL wrapper for erase / program / CRC
  with the range-checks that enforce the bootloader's self-protection.
- `Core/Src/bl_isotp.c` — ISO-TP reassembler, HAL-free, unit-testable.
- `Core/Src/bl_*.c` — health, DTC, log, live-data, NVM, option-byte
  modules. Each with a matching `.h` that's the public surface.
- `Core/Src/main.c` — CubeMX-managed init + the main dispatch loop.
  Hand-edited code lives in `USER CODE BEGIN/END` blocks per CubeMX
  convention.

See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full memory map,
boot flow, and opcode-by-opcode design rationale.

## Wire-format changes

If your change touches the CAN protocol (opcodes, ID layout, ISO-TP
framing, msg-type byte) you need matching changes in the host tool
(`isc-fs/can-flasher`) because they share the protocol. Coordinate
via linked PRs — the flasher side holds the spec
(`can-flasher/REQUIREMENTS.md`), this side implements it. Bump
`BL_PROTO_VERSION_MINOR` on backward-compatible changes,
`BL_PROTO_VERSION_MAJOR` on breaking ones.

---

*ISC Racing Team — IFS08 Driverless*

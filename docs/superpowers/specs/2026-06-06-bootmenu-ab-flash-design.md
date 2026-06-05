# Bootmenu A/B Flash Design

**Date:** 2026-06-06
**Target:** i.MX6ULL 14x14 DOF (NAND boot, `mx6ull_14x14_dof_nand_defconfig`)
**Status:** Approved (awaiting implementation plan)

## Goal

Modify the U-Boot bootmenu so that flashing `boot`, `kfd`, or `rootfs` targets the
**opposite slot** of the currently active one. After flashing, automatically switch
the next-boot slot to the newly flashed side, save the environment, and reset.

This implements a developer-friendly A/B update flow entirely from the U-Boot
console, without requiring the running kernel to coordinate.

## Background

The NAND already has a fixed A/B layout (`include/configs/mx6ulldof.h`):

```
8m(uboot_a), 8m(uboot_b), 32m(kfd_a), 32m(kfd_b),
16m(tee_a), 16m(tee_b), 512k(env_a), 512k(env_b),
175m(rootfs_a), 175m(rootfs_b), -(date)
```

- `kfd_a` / `kfd_b` are UBI partitions that hold two static volumes each: `kernel`
  (8 MiB) and `dtb` (128 KiB).
- `rootfs_a` / `rootfs_b` are raw NAND partitions written as a single `.ubi` image
  via `nand write.trimffs`.
- `uboot_a` / `uboot_b` are raw NAND partitions written via `nandbcb init`.

Today's bootmenu exposes per-slot flash commands but no automatic A/B switching.
Developers must remember which slot is currently running and pick the right entry,
and they must manually update `bootcmd` to boot the new slot.

## Non-Goals

- Production-grade OTA (no signing, no rollback counters, no atomic dual-write).
- Modifying SPL or the i.MX6ULL ROM behavior.
- Replacing `fw_setenv` / host-side tooling.
- A/B switching for `tee_*` (out of scope; tee flashing uses the symmetric pattern
  if ever needed).

## Design

### 1. Active-slot tracking

Two new environment variables:

| Variable | Meaning | Default | Persisted |
|----------|---------|---------|-----------|
| `active_slot` | The slot we are currently running on (`a` or `b`) | `a` | yes (in `env_a` / `env_b`) |
| `target_slot` | The opposite of `active_slot` (the flash target) | recomputed | no |

A small helper script computes `target_slot` from `active_slot`:

```
set_target_slot = if test ${active_slot}=a ; then setenv target_slot b ;
                  else setenv target_slot a ; fi
```

`set_target_slot` is invoked at the start of:

- `bootcmd_primary`
- `bootcmd_backup`
- `update_boot`
- `update_kfd`
- `update_rootfs`
- Both "set active slot to a/b" menu entries

This keeps `target_slot` always consistent with `active_slot` without depending on
a saveenv between the two.

### 2. Bootcmd wiring

```
bootcmd_primary = run set_target_slot ;
                  setenv active_slot a ;
                  <existing bootcmd_primary body>

bootcmd_backup  = run set_target_slot ;
                  setenv active_slot b ;
                  <existing bootcmd_backup body>
```

The first time the user enters the bootmenu without having set `active_slot` (e.g.
right after `env` is reset), `active_slot` defaults to `a` because the U-Boot
default environment defines it. Once `bootcmd_primary` or `bootcmd_backup` runs to
completion, `active_slot` is also written to `env_a` / `env_b` by the next
`saveenv` (e.g. issued by the "switch next boot" entry).

### 3. Flash commands

#### `update_boot` — U-Boot firmware

```
update_boot = run set_target_slot ;
              tftp 0x80800000 u-boot-dtb.imx ;
              nand erase.part uboot_${target_slot} ;
              nandbcb init 0x80800000 uboot_${target_slot} ${filesize} ;
              <switch bootcmd + saveenv + reset>   # see section 4
```

Note: writing U-Boot to the opposite slot does not affect the currently running
U-Boot. After the post-flash reset, the i.MX6ULL ROM reads from offset `0x0` (the
BCB-controlled area); whether the new U-Boot runs depends on the existing
`nandbcb` mechanism, which is left untouched.

#### `update_kfd` — kernel + device tree

```
update_kfd = run set_target_slot ;
              tftp 0x80800000 zImage ;
              tftp 0x83000000 imx6ull-14x14-evk-dof-nand.dtb ;
              setenv dtb_filesize ${filesize} ;
              nand erase.part kfd_${target_slot} ;
              ubi part kfd_${target_slot} ;
              ubi create kernel 0x800000 s || true ;
              ubi create dtb 0x20000 s || true ;
              ubi write kernel 0x80800000 ${filesize} ;
              ubi write dtb 0x83000000 ${dtb_filesize} ;
              <switch bootcmd + saveenv + reset>
```

The `|| true` guards make the `ubi create` lines idempotent: a second flash to
the same slot reuses the existing static volumes.

#### `update_rootfs` — rootfs UBI image

```
update_rootfs = run set_target_slot ;
                tftp 0x80800000 rootfs_dof.ubi ;
                nand erase.part rootfs_${target_slot} ;
                nand write.trimffs 0x80800000 rootfs_${target_slot} ${filesize} ;
                <switch bootcmd + saveenv + reset>
```

### 4. Post-flash bootcmd switch

A shared suffix appended to every `update_*` command:

```
switch_and_reboot = if test ${active_slot}=a ; then setenv bootcmd ${bootcmd_backup} ;
                     else setenv bootcmd ${bootcmd_primary} ; fi ;
                     saveenv ;
                     echo Next boot will use ${target_slot}, resetting... ;
                     reset
```

Each `update_*` ends with `run switch_and_reboot` so the user does not have to
navigate the menu again. This is the only path that performs the auto-switch;
manual menu entries (section 5) also use `switch_and_reboot` for consistency.

### 5. Bootmenu entries (NAND BOOT only)

The existing entries for `tftp upgrade image and dtb`, `via nfs rootfs`, and the
non-A/B SD-card menu items stay where they are. New entries are appended:

| Index | Label | Command |
|-------|-------|---------|
| N+0   | `update boot → ${target_slot}` | `run update_boot` |
| N+1   | `update kfd → ${target_slot}` | `run update_kfd` |
| N+2   | `update rootfs → ${target_slot}` | `run update_rootfs` |
| N+3   | `set active slot to a` | `setenv active_slot a ; run set_target_slot ; saveenv` |
| N+4   | `set active slot to b` | `setenv active_slot b ; run set_target_slot ; saveenv` |
| N+5   | `switch next boot to ${target_slot}` | `run switch_and_reboot` |

The exact `N` value depends on the existing menu layout; the implementation plan
will pin the indices after re-reading the current `include/configs/mx6ulldof.h`.

### 6. C code change in `cmd/bootmenu.c`

The current `bootmenu_create()` reads each `bootmenu_N` env var and splits on the
first `=`, taking the left half verbatim as the menu label. We add a small helper
`expand_title()` that performs `${var}` expansion on the label string and replace
the `entry->title` allocation with the expanded result.

#### `expand_title` behavior

- Scans the input for the substring `${name}` where `name` is up to 63 characters
  of `[A-Za-z0-9_]`.
- Looks up `name` via `env_get()` and copies the value into the output buffer.
- Unknown or malformed references (no closing brace) are copied verbatim.
- Output buffer is `strlen(src) + 256`; this is generous for the small set of
  labels we use and avoids needing a two-pass sizing pass.
- On allocation failure, fall back to `strdup(src)` so the menu still renders.

#### Why this approach

- Single-file change in `cmd/bootmenu.c`; no new Kconfig options.
- Backward compatible: labels without `${...}` work exactly as before.
- No dependency on the hush shell at menu-render time, which keeps the
  "labels are pure env state" mental model intact.

### 7. Error handling

- Each `tftp` step propagates its own error; if `tftp` fails, the `nand erase` /
  `nand write` chain is not reached because U-Boot's shell aborts on the first
  failed command in a multi-statement env (semicolon-separated).
- `ubi create ... || true` makes the kfd flash idempotent on the same slot.
- A failed `saveenv` will print a warning from `common/env_nand.c`; the user can
  retry from the menu.

### 8. Testing strategy

The implementation plan must include the following manual checks, executed in
order against the real i.MX6ULL DOF board:

1. **Build + boot** from NAND A (current default).
2. Enter bootmenu; confirm labels read `update boot → b`, `update kfd → b`,
   `update rootfs → b`, `switch next boot to b`.
3. Select `set active slot to b`; reset. Confirm labels flip to `→ a`.
4. Reboot from A, select `update kfd → b`. After TFTP, confirm:
   - `ubi part kfd_b ; ubi info` shows new `kernel` and `dtb` volumes.
   - `printenv bootcmd` shows `bootcmd_backup`.
   - After auto-reset, kernel log shows `ubi.mtd=9` (rootfs_b bootargs).
5. Repeat for `update_boot` (verify `nandbcb` chain succeeds) and
   `update_rootfs` (verify `ubi.mtd=8` is reachable on next boot).
6. Negative test: select `update kfd` with an empty TFTP server. The flash
   should abort at the `tftp` step; `bootcmd` must remain unchanged.

## Files touched

- `include/configs/mx6ulldof.h` — replace existing `update*` and `bootmenu_*`
  env strings under `#if defined(CONFIG_NAND_BOOT)`.
- `cmd/bootmenu.c` — add `expand_title()` and call it from `bootmenu_create()`.

## Out of scope for this design

- The `fw_env.config` host-side tool, `tools/env/fw_env_example.c`, and
  `tools/env/README.md` are not affected by this change.
- SD-card bootmenu entries (the `#else` branch of the config) are not modified.
- The `bootcmd_primary` / `bootcmd_backup` body (kernel + dtb `ubi read` calls)
  is left intact; only the leading `setenv active_slot` lines are prepended.

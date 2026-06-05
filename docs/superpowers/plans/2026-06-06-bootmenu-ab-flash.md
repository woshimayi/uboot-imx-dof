# Bootmenu A/B Flash Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a U-Boot bootmenu to the NAND BOOT branch of `mx6ulldof.h` that flashes `boot`, `kfd`, and `rootfs` to the opposite slot of the currently active one, with auto-switch of the next-boot slot.

**Architecture:** Two-part change. (1) Modify `cmd/bootmenu.c` to expand `${var}` references in menu labels at display time. (2) Restructure the NAND BOOT `CONFIG_EXTRA_ENV_SETTINGS` to introduce `active_slot` / `target_slot` state, three A/B-aware `update_*` commands, and six new bootmenu entries that use the dynamic labels.

**Tech Stack:** U-Boot 2024.x (NXP downstream), C (bootmenu.c), hush shell (env-driven commands), GPMI NAND on i.MX6ULL.

**Spec:** `docs/superpowers/specs/2026-06-06-bootmenu-ab-flash-design.md`

---

## Current State Discovery

The latest working tree has `include/configs/mx6ulldof.h` in a state where the
NAND BOOT branch (`#if defined(CONFIG_NAND_BOOT)`) does **not** contain any
`bootmenu_*` entries. The SD BOOT branch (the `#else` branch) keeps
`bootmenu_0` … `bootmenu_7` for SD card operations. The NAND branch instead
defines:

- `updateboot_a`, `updateboot_b`, `updateboot` — using the legacy `boot_a` /
  `boot_b` partition names that **no longer exist** in the current
  `MFG_NAND_MTDPARTS` layout.
- `bootcmd_primary`, `bootcmd_backup` — already correct, using `kfd_a` /
  `kfd_b` UBI partitions.
- `bootcmd=run bootcmd_primary` — no fallback to backup.

This plan therefore **adds** the bootmenu (it does not modify an existing
one) and replaces the stale `updateboot_*` commands with the new
`update_boot` / `update_kfd` / `update_rootfs` A/B-aware commands.

---

## File Structure

Files modified by this plan:

- `cmd/bootmenu.c` — add `expand_title()` helper; use it in
  `bootmenu_create()` for `entry->title`.
- `include/configs/mx6ulldof.h` — restructure the
  `#if defined(CONFIG_NAND_BOOT)` branch of `CONFIG_EXTRA_ENV_SETTINGS`.

No new files are created. No header / Kconfig / defconfig changes are needed.

---

## Task 1: Add `expand_title()` to `cmd/bootmenu.c`

**Files:**
- Modify: `cmd/bootmenu.c:248-318` (`bootmenu_create()` function)

- [ ] **Step 1: Add the `expand_title()` static function**

Open `cmd/bootmenu.c` and add this function directly above
`static struct bootmenu_data *bootmenu_create(int delay)` (currently at
line 248). Keep the existing comment style (doxygen not required for a
file-local helper; one short line is enough).

```c
/*
 * Expand ${name} references in a menu-label string by looking each
 * `name` up in the U-Boot environment. Unknown / malformed references
 * are copied verbatim. The output buffer is sized at strlen(src) + 256
 * which is generous for the small set of labels used in this project.
 */
static char *expand_title(const char *src)
{
	int inlen = strlen(src);
	int outlen = inlen + 256;
	char *out = malloc(outlen);
	char *p;
	const char *s = src;

	if (!out)
		return strdup(src);

	p = out;
	while (*s) {
		if (s[0] == '$' && s[1] == '{') {
			const char *end = strchr(s, '}');
			if (end) {
				int varlen = end - s - 2;
				char varname[64];
				const char *val;
				int avail = outlen - (p - out) - 1;

				if (varlen > 0 && varlen < 64) {
					memcpy(varname, s + 2, varlen);
					varname[varlen] = '\0';
					val = env_get(varname);
					if (val) {
						int vallen = strlen(val);
						if (vallen > avail)
							vallen = avail;
						memcpy(p, val, vallen);
						p += vallen;
					}
				}
				s = end + 1;
				continue;
			}
		}
		if (p - out < outlen - 1)
			*p++ = *s;
		s++;
	}
	*p = '\0';
	return out;
}
```

- [ ] **Step 2: Use `expand_title()` in `bootmenu_create()`**

In `cmd/bootmenu.c`, find the block in `bootmenu_create()` that begins with
`len = sep-option;` (around line 283) and currently does:

```c
		len = sep-option;
		entry->title = malloc(len + 1);
		if (!entry->title) {
			free(entry);
			goto cleanup;
		}
		memcpy(entry->title, option, len);
		entry->title[len] = 0;
```

Replace it with:

```c
		/* Title is everything before the first '=' in the env value. */
		len = sep - option;
		/*
		 * Terminate the source slice with a NUL so expand_title can
		 * treat it as a C string without copying first.
		 */
		*sep = '\0';
		entry->title = expand_title(option);
		*sep = '=';
		if (!entry->title) {
			free(entry);
			goto cleanup;
		}
```

The `*sep = '\0'` / `*sep = '='` dance keeps the original env buffer
intact; `expand_title` then reads `option` (the start of the env value)
as a null-terminated string.

- [ ] **Step 3: Verify the file still compiles**

Run from the repo root:

```bash
make mx6ull_14x14_dof_nand_defconfig
make -j$(nproc) 2>&1 | tee /tmp/ubuild.log
```

Expected: build completes without warnings about
`expand_title` being defined but unused, and without any new errors
related to `bootmenu.c`. (Existing warnings are fine; only flag **new**
ones introduced by this change.)

- [ ] **Step 4: Commit**

```bash
git add cmd/bootmenu.c
git commit -m "cmd/bootmenu: expand \${var} in menu labels

Allows menu labels to reference environment variables, e.g.
  bootmenu_1=update kfd -> \${target_slot}=run update_kfd
displays as 'update kfd -> b' when target_slot=b. Unknown /
malformed references are copied verbatim so existing labels
are unaffected.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: Wire `active_slot` / `target_slot` into the NAND env

**Files:**
- Modify: `include/configs/mx6ulldof.h:114-147` (the NAND BOOT branch of
  `CONFIG_EXTRA_ENV_SETTINGS`)

This task introduces the slot-state variables and the two helper scripts
(`set_target_slot`, `switch_and_reboot`). It does **not** change
`bootcmd_*` or add new menu entries yet; those land in Tasks 3 and 4.

- [ ] **Step 1: Add `active_slot` to the env block**

In `include/configs/mx6ulldof.h`, find the NAND BOOT branch
(`#if defined(CONFIG_NAND_BOOT)`) of `CONFIG_EXTRA_ENV_SETTINGS`. Insert
a new line **immediately after** the `console=ttymxc0\0"` line (currently
line 123):

```c
	"active_slot=a\0" \
	"set_target_slot=if test ${active_slot}=a ; then setenv target_slot b ; else setenv target_slot a ; fi\0" \
	"switch_and_reboot=if test ${active_slot}=a ; then setenv bootcmd ${bootcmd_backup} ; else setenv bootcmd ${bootcmd_primary} ; fi ; saveenv ; echo Next boot will use ${target_slot}, resetting... ; reset\0" \
```

The backslash-newline after each `\0` matches the existing concatenation
style of the surrounding block.

- [ ] **Step 2: Verify the concatenation is syntactically valid**

The full `CONFIG_EXTRA_ENV_SETTINGS` macro is a string-literal
concatenation. Each new line must end with `\` and the next line must
begin with `"`. Re-read the file from line 114 to line 147 and confirm:

- Every line that should be part of the macro ends with `\` followed by
  newline, with the next line starting with `"`.
- The block ends with `"\0" \` (the existing `bootargs_backup=...` line)
  followed by a closing `#else` later.

If the existing `updateboot_a=` / `updateboot_b=` / `updateboot=` lines
(124–126) confuse the layout, leave them for now — Task 3 deletes them.

- [ ] **Step 3: Build**

```bash
make -j$(nproc) 2>&1 | tee /tmp/ubuild.log
```

Expected: build succeeds. If the compiler complains about unterminated
string literals, you have a missing `\` or stray newline in the
concatenation.

- [ ] **Step 4: Commit**

```bash
git add include/configs/mx6ulldof.h
git commit -m "mx6ulldof: add active_slot / target_slot env plumbing

Introduces the active_slot default ('a'), the set_target_slot helper
script, and the switch_and_reboot shared suffix used by the new
A/B-aware update commands.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: Replace stale `updateboot_*` with A/B-aware commands

**Files:**
- Modify: `include/configs/mx6ulldof.h:124-126` (delete the old
  `updateboot_a=`, `updateboot_b=`, `updateboot=` lines)
- Modify: `include/configs/mx6ulldof.h` (add the three new
  `update_*` commands, near the slot helpers from Task 2)

- [ ] **Step 1: Delete the three legacy `updateboot*` lines**

In `include/configs/mx6ulldof.h`, remove these three lines entirely:

```c
	"updateboot_a=tftp 0x80800000 boot_a.img; nand erase.part boot_a; nand write.trimffs 0x80800000 boot_a ${filesize}\0" \
	"updateboot_b=tftp 0x80800000 boot_b.img; nand erase.part boot_b; nand write.trimffs 0x80800000 boot_b ${filesize}\0" \
	"updateboot=run updateboot_a\0" \
```

The `boot_a` / `boot_b` partition names no longer exist (the current
layout uses `uboot_a` / `uboot_b` / `kfd_a` / `kfd_b` /
`rootfs_a` / `rootfs_b`), so these are dead code.

- [ ] **Step 2: Add the three new `update_*` commands**

Insert the following block **immediately after** the new
`switch_and_reboot=...` line added in Task 2:

```c
	"update_boot=run set_target_slot ; tftp 0x80800000 u-boot-dtb.imx ; nand erase.part uboot_${target_slot} ; nandbcb init 0x80800000 uboot_${target_slot} ${filesize} ; run switch_and_reboot\0" \
	"update_kfd=run set_target_slot ; nand erase.part kfd_${target_slot} ; ubi part kfd_${target_slot} ; ubi create kernel 0x800000 s || true ; ubi create dtb 0x20000 s || true ; tftp 0x80800000 zImage ; ubi write kernel 0x80800000 ${filesize} ; tftp 0x83000000 imx6ull-14x14-evk-dof-nand.dtb ; ubi write dtb 0x83000000 ${filesize} ; run switch_and_reboot\0" \
	"update_rootfs=run set_target_slot ; tftp 0x80800000 rootfs_dof.ubi ; nand erase.part rootfs_${target_slot} ; nand write.trimffs 0x80800000 rootfs_${target_slot} ${filesize} ; run switch_and_reboot\0" \
```

Notes on the `update_kfd` order (critical): the tftp/ubi write pairs are
**interleaved** rather than batched, because U-Boot's `${filesize}` env
is overwritten by every `tftp` call. If both images were tftp'd up front,
the kernel `ubi write` would use the dtb size.

- [ ] **Step 3: Build**

```bash
make -j$(nproc) 2>&1 | tee /tmp/ubuild.log
```

Expected: build succeeds, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add include/configs/mx6ulldof.h
git commit -m "mx6ulldof: replace stale updateboot_* with A/B update_* commands

update_boot / update_kfd / update_rootfs each flash to the opposite
slot (target_slot = complement of active_slot), then call
switch_and_reboot to point bootcmd at the freshly flashed slot and
reset.

The legacy updateboot_a/b/updateboot referenced the now-nonexistent
boot_a/boot_b partitions and are removed.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: Prepend `setenv active_slot` to `bootcmd_primary` / `bootcmd_backup`

**Files:**
- Modify: `include/configs/mx6ulldof.h:132-141` (`bootcmd_primary=` and
  `bootcmd_backup=` env strings)

- [ ] **Step 1: Update `bootcmd_primary` to set `active_slot=a`**

Find the line:

```c
	"bootcmd_primary=setenv bootargs 'console=ttymxc0,115200 ubi.mtd=8 ...
```

Change it to begin with the slot bookkeeping:

```c
	"bootcmd_primary=run set_target_slot ; setenv active_slot a ; setenv bootargs 'console=ttymxc0,115200 ubi.mtd=8 root=ubi0:rootfs rootfstype=ubifs mtdparts=gpmi-nand:8m(uboot_a),8m(uboot_b),32m(kfd_a),32m(kfd_b),16m(tee_a),16m(tee_b),512k(env_a),512k(env_b),175m(rootfs_a),175m(rootfs_b),-(date)' && "\
		"ubi part kfd_a && "\
		"ubi read ${loadaddr} kernel && "\
		"ubi read ${fdt_addr} dtb && "\
		"bootz ${loadaddr} - ${fdt_addr}\0" \
```

(The only change is the `run set_target_slot ; setenv active_slot a ; `
prepended at the start; the rest of the body is identical to the current
line.)

- [ ] **Step 2: Update `bootcmd_backup` the same way**

Find the `bootcmd_backup=` line (currently 137) and prepend
`run set_target_slot ; setenv active_slot b ; ` so it begins:

```c
	"bootcmd_backup=run set_target_slot ; setenv active_slot b ; setenv bootargs 'console=ttymxc0,115200 ubi.mtd=9 ...
```

The rest of the body is unchanged.

- [ ] **Step 3: Build**

```bash
make -j$(nproc) 2>&1 | tee /tmp/ubuild.log
```

Expected: build succeeds, no new warnings.

- [ ] **Step 4: Commit**

```bash
git add include/configs/mx6ulldof.h
git commit -m "mx6ulldof: stamp active_slot in bootcmd_primary/backup

When the kernel is actually loaded from a given slot, the bootcmd
itself records which slot that was, so the bootmenu and any later
update_* command know the right target_slot. set_target_slot is
re-evaluated at the start of each bootcmd in case the env was loaded
from a default.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: Add six new bootmenu entries to the NAND branch

**Files:**
- Modify: `include/configs/mx6ulldof.h` (insert new lines **before** the
  closing `\0"` of the NAND BOOT `CONFIG_EXTRA_ENV_SETTINGS` block)

The current NAND BOOT `CONFIG_EXTRA_ENV_SETTINGS` block has no
`bootmenu_*` entries at all. We add six, using the dynamic label feature
from Task 1.

- [ ] **Step 1: Insert the six new menu entries**

Locate the line:

```c
	"bootargs_backup=console=ttymxc0,115200 ubi.mtd=rootfs_a,rootfs_b " \
```

Insert the following block **immediately before** it (so the menu entries
sit with the other boot-related env, and `bootargs_backup` remains at
the end of the NAND block):

```c
	"bootmenu_0=boot from nfs=setenv bootargs 'noinitrd console=ttymxc0,115200 root=/dev/nfs nfsroot=10.8.8.4:/home/zsD/linux/nfs/rootfs,v3, rw ip=10.8.8.10:10.8.8.4:10.8.8.1:255.255.255.0::eth0:off' ; tftp 0x80800000 zImage ; tftp 0x83000000 imx6ull-14x14-evk-dof-nand.dtb ; bootz 0x80800000 - 0x83000000\0" \
	"bootmenu_1=update boot -> ${target_slot}=run update_boot\0" \
	"bootmenu_2=update kfd -> ${target_slot}=run update_kfd\0" \
	"bootmenu_3=update rootfs -> ${target_slot}=run update_rootfs\0" \
	"bootmenu_4=set active slot to a=setenv active_slot a ; run set_target_slot ; saveenv\0" \
	"bootmenu_5=set active slot to b=setenv active_slot b ; run set_target_slot ; saveenv\0" \
	"bootmenu_6=switch next boot to ${target_slot}=run switch_and_reboot\0" \
```

The arrow `->` is rendered as a literal ASCII glyph (the same arrow
already used in the spec doc and the existing code in this file uses
plain ASCII in labels). If the user wants a Unicode arrow (`→`) instead,
swap the two characters in each label; U-Boot's ANSI terminal code path
(`bootmenu_print_entry`) calls `puts()` on the title and does not filter
non-ASCII bytes.

The two `set active slot to a/b` entries intentionally do not call
`switch_and_reboot` — they only retarget the **next** flash, not the next
boot.

- [ ] **Step 2: Build**

```bash
make -j$(nproc) 2>&1 | tee /tmp/ubuild.log
```

Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add include/configs/mx6ulldof.h
git commit -m "mx6ulldof: add NAND bootmenu with A/B flash entries

Six new bootmenu_* entries for the NAND BOOT branch:

  - update boot / kfd / rootfs: flash to \${target_slot} (opposite
    of active_slot), then auto-switch and reset.
  - set active slot to a / b: manually override which slot is treated
    as active. Useful when the board was just reflashed out of band.
  - switch next boot to \${target_slot}: only retarget bootcmd, no
    flash.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: On-board smoke test

This task runs against the real i.MX6ULL DOF board. It is the gate that
decides whether the feature is shippable.

- [ ] **Step 1: Confirm clean boot from NAND A**

Flash the freshly built U-Boot via the existing `mx6ull_run.sh` (or
`uuu` directly), then power-cycle. Stop autoboot and:

```bash
=> printenv active_slot
active_slot=a
=> printenv target_slot
target_slot=b
```

Expected: both variables resolve. If `active_slot` is empty, the env
default in Task 2 did not take effect — re-check the
`CONFIG_EXTRA_ENV_SETTINGS` concatenation.

- [ ] **Step 2: Verify menu labels are dynamic**

```bash
=> menu
```

Expected: the menu shows, among others:

```
     0: boot from nfs
     1: update boot -> b
     2: update kfd -> b
     3: update rootfs -> b
     4: set active slot to a
     5: set active slot to b
     6: switch next boot to b
     7: U-Boot console
```

(The `U-Boot console` row is appended automatically by `bootmenu_create()`.)

- [ ] **Step 3: Flip active_slot and re-check labels**

Select entry 5 (`set active slot to b`), then re-enter the menu:

```bash
=> run set_target_slot
=> menu
```

Expected: the four dynamic labels now read `-> a`.

- [ ] **Step 4: Flash kfd to the inactive slot (round-trip)**

Reset back to the A side (entry 4: `set active slot to a`). Then select
entry 2 (`update kfd -> b`). After TFTP completes:

```bash
=> printenv bootcmd
bootcmd=run bootcmd_backup
```

Expected: `bootcmd` has been switched to `bootcmd_backup` and saved.

- [ ] **Step 5: Verify the new kfd_b UBI volumes**

After the auto-reset lands you back in U-Boot (because the new kfd_b is
now what `bootcmd_backup` loads, but the menu stopped autoboot first):

```bash
=> ubi part kfd_b
=> ubi info
```

Expected: two static volumes named `kernel` (8 MiB) and `dtb` (128 KiB).

- [ ] **Step 6: Negative test — TFTP failure must not switch bootcmd**

Stop autoboot. With active_slot=a, manually run the kfd flash against a
nonexistent TFTP server:

```bash
=> setenv serverip 127.0.0.1
=> run update_kfd
```

Expected: `TFTP error: ...` from the tftp step, and the rest of the
chain is **not** executed. `bootcmd` must still be
`run bootcmd_primary` afterwards. (U-Boot's hush shell aborts on the
first failing command in a semicolon-separated env, so `run
switch_and_reboot` is unreachable.)

Restore `serverip=10.8.8.4` afterwards.

- [ ] **Step 7: Commit the test results**

If everything passed, create a one-line note in the existing
`NAND_FLASH_SUCCESS_LOG.md` (or add a new dated section) recording
that the A/B flash flow was validated on `2026-06-06`. Commit:

```bash
git add NAND_FLASH_SUCCESS_LOG.md
git commit -m "docs: log successful A/B bootmenu flash validation

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

If any step failed, do **not** commit. Open a new plan task describing
the failure and revisit the previous tasks.

---

## Definition of Done

All five code tasks plus the on-board smoke test must succeed before this
plan is considered complete. The final state of the NAND BOOT branch in
`include/configs/mx6ulldof.h` should contain:

- `active_slot=a` default.
- `set_target_slot` and `switch_and_reboot` helpers.
- `update_boot`, `update_kfd`, `update_rootfs` A/B-aware commands.
- `bootcmd_primary` / `bootcmd_backup` stamping `active_slot`.
- Seven `bootmenu_*` entries (`boot from nfs` plus six A/B ones).

The C file `cmd/bootmenu.c` should contain a static `expand_title()`
function and use it in `bootmenu_create()`.

The legacy `updateboot_a` / `updateboot_b` / `updateboot` env strings
must be gone.

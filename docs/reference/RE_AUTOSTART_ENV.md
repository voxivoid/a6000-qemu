# Clean-boot LD_PRELOAD autostart (stock `env.txt` / `dmode` mechanism)

A way to load a `.so` into the camera's `im.elf` at boot **without ptrace**, so
the menu and key input stay fully operable (a ptrace inject leaves im.elf's
event0 key path dead — `tools/ui_so.c`: "event0 is empty in-process" — so the
menu can't be navigated after an inject). This is the camera's own debug
mechanism, not a QEMU change; it is also how OpenMemories autostart works on the
real device.

## The init path (`re/ida/init.c`, `/sbin/init` in the stock initrd)

```
main → mount /setting → check_oreore2() → check_mode_file()
check_mode_file (@0x9088):
    open("/setting/mode/dmode"); if it exists → is_oreore_env = 1 (always),
    then the file's chars tune sub-modes: '0'=kernel_only, k=kmc, g=gdb,
    d=duma, e=efence (an EMPTY dmode just sets is_oreore_env, nothing else).
main: if (is_oreore_env) set_oreore_env()
set_oreore_env (@0x9568): mmap /setting/env.txt → parse_and_set_env():
    putenv() every whitespace-separated NAME=VALUE token.
main then: run_bg_process("/usr/bin/bootin.elf")  ← inherits the env (LD_PRELOAD)
main LABEL_53 (AFTER bootin is launched): putenv("LD_PRELOAD=")  ← clears it late
```

So `bootin.elf` (and the `im.elf` it starts) inherit the `LD_PRELOAD` from
`env.txt` — **but only if `/setting/mode/dmode` exists** (that is the one gate
that was missing in earlier attempts).

## Recipe

1. Put the `.so` under **`/setting`** (nflasha2, rw). `/usr` (nflasha15) is a
   read-only FTL that rejects writes larger than ~2 KiB, so the `.so` cannot go
   there.
2. `/setting/env.txt` = `LD_PRELOAD=/setting/a5k/<loader>.so` (plus any other
   `NAME=VALUE` the loader reads, e.g. `MODLOADER_TAB=1`).
3. `touch /setting/mode/dmode` (empty).
4. Cold-reboot the **same** nand (keep `/setting`). The loader dlopens into
   im.elf at boot; its `comm=="im.elf"` self-gate keeps it inert in other
   processes.

Helper: `tools/install_autostart.py --port <A5K_SHELL> --so <loader.so>
[--env MODLOADER_TAB=1]`, then reboot. `--remove` returns to pure stock.

Verified on the offline `sony-a5000` machine: the `.so` appears in im.elf's maps
(4 segments) at clean boot, its constructor/worker runs, and the menu is
navigable by `sendkey` (unlike after a ptrace inject). Brick-safe: `/setting`
only, no `/usr`/flash writes, fully reversible (`--remove` + reboot).

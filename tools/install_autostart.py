#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Install a clean-boot LD_PRELOAD autostart into a running sony-a5000 guest.

This is the camera's OWN stock mechanism, not a QEMU change: /sbin/init's
`check_mode_file()` sets `is_oreore_env=1` whenever `/setting/mode/dmode` exists;
`set_oreore_env()` then `putenv`s every `NAME=VALUE` token from `/setting/env.txt`
(`parse_and_set_env`), and that environment is inherited by `/usr/bin/bootin.elf`
and thus by `im.elf` (LD_PRELOAD is only cleared AFTER bootin is launched, at the
init's LABEL_53). So a `.so` named in env.txt's LD_PRELOAD is dlopened into
im.elf at boot -- with NO ptrace, so the menu/input stays fully operable (a
ptrace inject leaves im.elf's event0 key path dead, ui_so.c: "event0 is empty
in-process"). This is also exactly how OpenMemories autostart works on-device.

Usage (guest already booted with A5K_SHELL=<port>):
    tools/install_autostart.py --port 47820 --so /path/to/loader.so \
        [--dst /setting/a5k/loader.so] [--env MODLOADER_TAB=1 ...]
then reboot the SAME nand (keep /setting): the .so loads into im.elf at boot.
Remove: tools/install_autostart.py --port 47820 --remove
Note: /setting (nflasha2) accepts the .so write; /usr (nflasha15) is a read-only
FTL that rejects writes >~2KB, so the .so must live under /setting.
"""
import argparse, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def sh(port, cmd, wait=12):
    return subprocess.run(
        [sys.executable, "-I", os.path.join(HERE, "a5kshell.py"),
         "--port", str(port), "--wait", str(wait), "run", cmd],
        capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True, help="A5K_SHELL guest shell port")
    ap.add_argument("--so", help="host path to the .so to preload")
    ap.add_argument("--dst", default="/setting/a5k/loader.so",
                    help="guest path under /setting for the .so")
    ap.add_argument("--env", action="append", default=[],
                    help="extra NAME=VALUE for env.txt (repeatable)")
    ap.add_argument("--remove", action="store_true",
                    help="disable autostart (rm env.txt/dmode)")
    a = ap.parse_args()

    if a.remove:
        sh(a.port, "rm -f /setting/env.txt /setting/mode/dmode; sync; echo removed")
        print("autostart removed; reboot to return to pure stock")
        return

    if not a.so:
        ap.error("--so is required unless --remove")
    base = os.path.basename(a.dst)
    subprocess.run([sys.executable, "-I", os.path.join(HERE, "a5kshell.py"),
                    "--port", str(a.port), "--wait", "12", "push", a.so, base],
                   check=False)
    env_lines = " ".join(["LD_PRELOAD=" + a.dst] + a.env)
    # one NAME=VALUE per line (parse_and_set_env splits on whitespace anyway)
    body = "\n".join(["LD_PRELOAD=" + a.dst] + a.env)
    sh(a.port,
       "mkdir -p /setting/a5k /setting/mode; cp /android/a5k/%s %s; chmod 755 %s; "
       "printf '%%s\\n' %s > /setting/env.txt 2>/dev/null || echo '%s' > /setting/env.txt; "
       ": > /setting/mode/dmode; sync; cat /setting/env.txt"
       % (base, a.dst, a.dst, _shq(body), body.replace("\n", " ")))
    print("installed: LD_PRELOAD=%s  env=[%s]" % (a.dst, env_lines))
    print("reboot the SAME nand (keep /setting) to load it into im.elf at boot")


def _shq(s):
    return "'" + s.replace("'", "'\\''") + "'"


if __name__ == "__main__":
    main()

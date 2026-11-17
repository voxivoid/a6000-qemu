#!/usr/bin/env python3
"""a6kshell.py - host side of the guest root shell (docs/reference/RE_GUEST_SHELL.md).

The emulator is started with the stock init shell on UART2 (ttyAM1), served
as a TCP socket:

    A6K_SHELL=46310 ./run.sh -display none ...
    (= -M sony-a6000,shell-socket=127.0.0.1:46310 -serial null)

Modes:
    a6kshell.py [--port N] [--job-control]       interactive (Ctrl-] quits)
    a6kshell.py [--port N] push LOCAL [REMOTE]   upload + md5 check
    a6kshell.py [--port N] pull REMOTE LOCAL     download + md5 check
    a6kshell.py [--port N] run CMD...            run one command, print output

push keeps the executable bit of LOCAL (chmod 755).  REMOTE defaults to /android/a6k/<basename of LOCAL>; a relative REMOTE is
taken relative to /android/a6k.  /android is a tmpfs mounted by the stock
s_start_android.sh: files there are executable and are lost at power-off.
Writing outside /android needs --force (never /tmp, /setting or another
NAND partition by accident).

Transfers use stock busybox only (section 5.1): push = `stty raw` + `cat`
with VTIME (the host paces 1 KiB per 10 ms), pull = `cat` with -opost.
Both are checked with `busybox md5sum`.

The guest's idle power save cycles the machine after about 5 minutes
without key input (section 9).  The tool notices a reboot (new kernel
banner, or /proc/uptime going back) and exits with status 3.

Exit status: 0 ok (run: the command's own status), 1 usage/connection
error, 2 md5 mismatch or timeout, 3 guest rebooted.  Stdlib only, Python 3.9+.
"""

import argparse
import hashlib
import os
import posixpath
import re
import select
import socket
import sys
import time

DEFAULT_DIR = "/android/a6k"
REBOOT_PATTERNS = (b"Linux version ", b"Suspending console", b"Restarting system")


class ShellError(Exception):
    status = 2


class RebootError(ShellError):
    status = 3


class Session:
    """Line-oriented driver of the guest ash on the serial socket."""

    def __init__(self, host, port, verbose=False):
        self.verbose = verbose
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.settimeout(0.1)
        self.seq = 0
        self.uptime = None

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def log(self, msg):
        if self.verbose:
            sys.stderr.write("a6kshell: %s\n" % msg)

    def recv(self, timeout, until=None, buf=b""):
        """Read for up to `timeout` s, stop early once `until` (bytes or
        compiled regex) matches the accumulated buffer."""
        end = time.time() + timeout
        while time.time() < end:
            try:
                d = self.sock.recv(65536)
                if not d:
                    raise ShellError("connection closed by QEMU")
                buf += d
            except socket.timeout:
                pass
            if until is not None:
                if isinstance(until, bytes):
                    if until in buf:
                        break
                elif until.search(buf):
                    break
        return buf

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        self.sock.sendall(data)

    @staticmethod
    def check_reboot(buf):
        for p in REBOOT_PATTERNS:
            if p in buf:
                raise RebootError("guest rebooted / powered off (saw %r); "
                                  "the shell session and /android are gone "
                                  "(idle power save? docs/reference/RE_GUEST_SHELL.md "
                                  "section 9)" % p.decode())

    def cmd(self, command, timeout=15.0):
        """Run `command`, return (output bytes, exit status).

        The output is framed by markers whose echoed spelling differs from
        the printed one (`@@B""G` echoes as typed, prints as `@@BG`)."""
        self.seq += 1
        n = self.seq
        line = 'echo @@B""G%d; %s; echo "@@E""ND%d=$?"\r' % (n, command, n)
        self.send(line)
        end_re = re.compile(rb"@@END%d=(\d+)\r?\n" % n)
        buf = self.recv(timeout, end_re)
        m = end_re.search(buf)
        if not m:
            self.check_reboot(buf)
            raise ShellError("no reply to %r within %.0f s (shell busy or "
                             "guest not up?)" % (command, timeout))
        beg = re.search(rb"@@BG%d\r?\n" % n, buf)
        start = beg.end() if beg else 0
        out = buf[start:m.start()]
        return out, int(m.group(1))

    def sync(self, wait=0.0):
        """Get to a responsive prompt; with `wait` > 0 keep trying (boot)."""
        deadline = time.time() + wait
        self.recv(0.3)
        while True:
            # stray raw mode from an interrupted transfer: restore the tty
            self.send("\r")
            self.recv(0.3)
            try:
                out, _ = self.cmd("busybox stty sane; cat /proc/uptime",
                                  timeout=5)
                self.uptime = parse_uptime(out)
                self.log("guest uptime %.1f s" % (self.uptime or -1))
                return
            except RebootError:
                if time.time() >= deadline:
                    raise
            except ShellError:
                if time.time() >= deadline:
                    raise ShellError("no shell on the socket (is the guest "
                                     "booted with A6K_SHELL / console=ttyAM1?"
                                     " use --wait to wait for boot)")
            time.sleep(2)

    def check_uptime(self):
        out, _ = self.cmd("cat /proc/uptime", timeout=5)
        up = parse_uptime(out)
        if up is not None and self.uptime is not None and up < self.uptime:
            raise RebootError("guest rebooted: /proc/uptime went from %.1f "
                              "to %.1f s" % (self.uptime, up))
        self.uptime = up

    def job_control(self):
        """Replace the init shell by an ash with a controlling tty, so that
        Ctrl-C works (section 4: getty does setsid + TIOCSCTTY)."""
        self.send("exec busybox getty -n -l /bin/ash 115200 ttyAM1\r")
        self.recv(2.0, b"built-in shell")
        self.recv(0.5)
        self.cmd("true", timeout=5)


def parse_uptime(out):
    m = re.search(rb"(\d+\.\d+)\s+\d+\.\d+", out)
    return float(m.group(1)) if m else None


def sh_quote(s):
    return "'" + s.replace("'", "'\\''") + "'"


def remote_path(remote, local, force):
    if remote is None:
        remote = posixpath.join(DEFAULT_DIR, os.path.basename(local))
    elif remote.endswith("/"):
        remote = posixpath.join(remote, os.path.basename(local))
    if not remote.startswith("/"):
        remote = posixpath.join(DEFAULT_DIR, remote)
    remote = posixpath.normpath(remote)
    if not force and not (remote + "/").startswith("/android/"):
        raise ShellError("refusing to write %s: outside the /android tmpfs "
                         "(NAND partition?); use --force" % remote)
    return remote


def remote_md5(sess, path):
    out, rc = sess.cmd("busybox md5sum %s" % sh_quote(path), timeout=60)
    m = re.search(rb"\b([0-9a-f]{32})\s", out)
    if rc or not m:
        raise ShellError("md5sum %s failed: %s"
                         % (path, out.decode("latin1").strip()))
    return m.group(1).decode()


def do_push(sess, local, remote, chunk, delay, force):
    with open(local, "rb") as f:
        data = f.read()
    remote = remote_path(remote, local, force)
    q = sh_quote(remote)
    _, rc = sess.cmd("mkdir -p %s" % sh_quote(posixpath.dirname(remote)))
    if rc:
        raise ShellError("mkdir -p %s failed" % posixpath.dirname(remote))
    t0 = time.time()
    # VMIN=0/VTIME=30: cat sees EOF 3 s after the last byte
    sess.send('busybox stty raw -echo min 0 time 30; echo @@R""DY; '
              'cat > %s; busybox stty sane; echo @@P""D\r' % q)
    buf = sess.recv(10, b"@@RDY")
    if b"@@RDY" not in buf:
        sess.check_reboot(buf)
        raise ShellError("guest did not enter raw mode")
    for i in range(0, len(data), chunk):
        sess.send(data[i:i + chunk])
        time.sleep(delay)
    buf = sess.recv(30, b"@@PD")
    if b"@@PD" not in buf:
        sess.check_reboot(buf)
        raise ShellError("push did not finish")
    dt = time.time() - t0
    lmd5 = hashlib.md5(data).hexdigest()
    rmd5 = remote_md5(sess, remote)
    sess.check_uptime()
    if rmd5 != lmd5:
        raise ShellError("PUSH MD5 MISMATCH %s: local %s remote %s"
                         % (remote, lmd5, rmd5))
    if os.access(local, os.X_OK):
        sess.cmd("chmod 755 %s" % q)
    print("push %s -> %s: %d B in %.1f s (incl. 3 s idle), md5 %s ok"
          % (local, remote, len(data), dt, lmd5))


def do_pull(sess, remote, local):
    if not remote.startswith("/"):
        remote = posixpath.join(DEFAULT_DIR, remote)
    q = sh_quote(remote)
    # keep kernel console lines out of the byte stream
    sess.cmd("busybox dmesg -n 1")
    out, rc = sess.cmd("ls -l %s" % q)
    fields = out.split()
    if rc or len(fields) < 5 or not fields[4].isdigit():
        raise ShellError("cannot stat %s: %s"
                         % (remote, out.decode("latin1").strip()))
    size = int(fields[4])
    rmd5 = remote_md5(sess, remote)
    sess.send('busybox stty raw -echo -opost; echo @@B""G; cat %s; '
              'busybox stty sane\r' % q)
    buf = sess.recv(10, b"@@BG\n")
    i = buf.find(b"@@BG\n")
    if i < 0:
        sess.check_reboot(buf)
        raise ShellError("pull did not start")
    data = buf[i + 5:]
    last = time.time()
    while len(data) < size and time.time() - last < 10:
        n = len(data)
        data = sess.recv(0.5, buf=data)
        if len(data) != n:
            last = time.time()
    data = data[:size]
    if len(data) < size:
        sess.check_reboot(data)
        raise ShellError("pull %s: got %d of %d B" % (remote, len(data), size))
    sess.recv(0.5)
    sess.check_uptime()
    lmd5 = hashlib.md5(data).hexdigest()
    if lmd5 != rmd5:
        raise ShellError("PULL MD5 MISMATCH %s: remote %s local %s"
                         % (remote, rmd5, lmd5))
    with open(local, "wb") as f:
        f.write(data)
    print("pull %s -> %s: %d B, md5 %s ok" % (remote, local, size, lmd5))


def do_run(sess, command, timeout):
    out, rc = sess.cmd(command, timeout=timeout)
    sys.stdout.write(out.replace(b"\r\n", b"\n").decode("utf-8", "replace"))
    sys.stdout.flush()
    sess.check_uptime()
    return rc


def do_interactive(sess):
    """Raw passthrough; Ctrl-] quits.  The guest's tty does the echo."""
    stdin = sys.stdin.fileno()
    tty = os.isatty(stdin)
    old = None
    if tty:
        import termios
        import tty as ttymod
        old = termios.tcgetattr(stdin)
        ttymod.setraw(stdin)
        sys.stderr.write("a6kshell: connected, Ctrl-] quits\r\n")
    sess.sock.setblocking(False)
    try:
        sess.send("\r")
        while True:
            r, _, _ = select.select([sess.sock, stdin], [], [])
            if sess.sock in r:
                try:
                    d = sess.sock.recv(65536)
                except BlockingIOError:
                    d = None
                if d == b"":
                    sys.stderr.write("\r\na6kshell: connection closed\r\n")
                    return 1
                if d:
                    if b"Linux version " in d:
                        sys.stderr.write("\r\na6kshell: guest rebooted\r\n")
                    os.write(sys.stdout.fileno(), d)
            if stdin in r:
                d = os.read(stdin, 1024)
                if not d:
                    # piped input ended: show the replies (1 s quiet), quit
                    sess.sock.setblocking(True)
                    sess.sock.settimeout(0.1)
                    out = sess.recv(1.0)
                    while out:
                        os.write(sys.stdout.fileno(), out)
                        out = sess.recv(1.0)
                    return 0
                if tty and b"\x1d" in d:
                    return 0
                if not tty:
                    d = d.replace(b"\n", b"\r")
                sess.sock.setblocking(True)
                sess.send(d)
                sess.sock.setblocking(False)
    finally:
        if old is not None:
            import termios
            termios.tcsetattr(stdin, termios.TCSADRAIN, old)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Guest root shell / file transfer over the A6000 "
                    "emulator's UART2 socket (docs/reference/RE_GUEST_SHELL.md)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("A6K_SHELL", "0") or 0),
                    help="TCP port of the UART2 socket (default $A6K_SHELL)")
    ap.add_argument("--wait", type=float, default=0.0, metavar="SEC",
                    help="wait up to SEC seconds for the shell (boot)")
    ap.add_argument("--job-control", action="store_true",
                    help="first exec busybox getty so that Ctrl-C works")
    ap.add_argument("--force", action="store_true",
                    help="allow push targets outside /android")
    ap.add_argument("--chunk", type=int, default=1024,
                    help="push chunk size in bytes (default 1024)")
    ap.add_argument("--delay", type=float, default=0.01,
                    help="push delay per chunk in s (default 0.01)")
    ap.add_argument("--timeout", type=float, default=60.0,
                    help="run: seconds to wait for the command")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("mode", nargs="?", default="shell",
                    choices=("shell", "push", "pull", "run"))
    ap.add_argument("args", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    if not a.port:
        ap.error("no port: use --port N or set A6K_SHELL")
    if a.mode == "push" and len(a.args) not in (1, 2):
        ap.error("push LOCAL [REMOTE]")
    if a.mode == "pull" and len(a.args) != 2:
        ap.error("pull REMOTE LOCAL")
    if a.mode == "run" and not a.args:
        ap.error("run CMD...")

    try:
        sess = Session(a.host, a.port, a.verbose)
    except OSError as e:
        sys.stderr.write("a6kshell: cannot connect to %s:%d: %s\n"
                         % (a.host, a.port, e))
        return 1
    try:
        if a.mode == "shell" and not a.job_control:
            return do_interactive(sess)
        sess.sync(a.wait)
        if a.job_control:
            sess.job_control()
        if a.mode == "shell":
            return do_interactive(sess)
        if a.mode == "push":
            do_push(sess, a.args[0], a.args[1] if len(a.args) > 1 else None,
                    a.chunk, a.delay, a.force)
            return 0
        if a.mode == "pull":
            do_pull(sess, a.args[0], a.args[1])
            return 0
        return do_run(sess, " ".join(a.args), a.timeout)
    except ShellError as e:
        sys.stderr.write("a6kshell: %s\n" % e)
        return e.status
    finally:
        sess.close()


if __name__ == "__main__":
    sys.exit(main())

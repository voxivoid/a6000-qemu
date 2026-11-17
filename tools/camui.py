#!/usr/bin/env python3
"""Camera body UI for the emulator: live LCD + every physical control.

Serves a local web page (tools/camui.html) that shows the emulated panel and
a drawn camera body; pressing a control sends real key down/up events to the
running emulator over QMP (input-send-event), so holds (shutter half press,
movie, wheel) behave like the hardware keys in the README table.

  # let camui start the emulator (headless, QMP socket in state/):
  python3 tools/camui.py --launch [--model a6000] [--port 8642]
  # or attach to an emulator started with -qmp unix:<sock>,server,nowait:
  python3 tools/camui.py --qmp state/qmp.sock

Then open http://localhost:8642 (also from Windows when running under WSL2).
Only the stdlib is used.  Nothing here touches the firmware or the NAND.
"""
import argparse
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

E = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HTML = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'camui.html')


class QMP:
    """Minimal QMP client: one socket, serialised commands, events dropped."""

    def __init__(self, path):
        self.path = path
        self.lock = threading.Lock()
        self.sock = None
        self.buf = b''

    def _connect(self):
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(10)
        s.connect(self.path)
        self.sock, self.buf = s, b''
        self._read()                                    # greeting
        self._send({'execute': 'qmp_capabilities'})

    def _read(self):
        while b'\n' not in self.buf:
            d = self.sock.recv(65536)
            if not d:
                raise ConnectionError('QMP closed')
            self.buf += d
        line, self.buf = self.buf.split(b'\n', 1)
        return json.loads(line)

    def _send(self, msg):
        self.sock.sendall(json.dumps(msg).encode() + b'\n')
        while True:
            r = self._read()
            if 'return' in r or 'error' in r:
                return r

    def cmd(self, execute, **args):
        with self.lock:
            for attempt in (0, 1):
                try:
                    if self.sock is None:
                        self._connect()
                    msg = {'execute': execute}
                    if args:
                        msg['arguments'] = args
                    return self._send(msg)
                except (OSError, ConnectionError, ValueError):
                    self.sock = None
                    if attempt:
                        raise


def keys_event(qmp, qcodes, down):
    events = [{'type': 'key', 'data': {'down': down,
               'key': {'type': 'qcode', 'data': q}}} for q in qcodes]
    return qmp.cmd('input-send-event', events=events)


class Handler(BaseHTTPRequestHandler):
    qmp = None
    shot = None
    frame_lock = threading.Lock()

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype='application/json', extra=()):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store')
        for k, v in extra:
            self.send_header(k, v)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj).encode())

    def do_GET(self):
        if self.path in ('/', '/index.html'):
            with open(HTML, 'rb') as f:
                return self._send(200, f.read(), 'text/html; charset=utf-8')
        if self.path.startswith('/frame'):
            return self.frame()
        if self.path == '/status':
            try:
                r = self.qmp.cmd('query-status')
                return self._json({'ok': True, 'status': r.get('return', {}).get('status')})
            except Exception as e:
                return self._json({'ok': False, 'error': str(e)})
        self._send(404, b'{}')

    def frame(self):
        """Panel as raw RGB (QMP screendump -> PPM, header stripped)."""
        try:
            with self.frame_lock:
                r = self.qmp.cmd('screendump', filename=self.shot)
                if 'error' in r:
                    return self._json({'error': r['error']}, 503)
                with open(self.shot, 'rb') as f:
                    d = f.read()
        except Exception as e:
            return self._json({'error': str(e)}, 503)
        magic, dims, maxv, px = d.split(b'\n', 3)
        w, h = dims.split()
        self._send(200, px, 'application/octet-stream',
                   (('X-Width', w.decode()), ('X-Height', h.decode())))

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        try:
            req = json.loads(self.rfile.read(n) or b'{}')
        except ValueError:
            return self._json({'error': 'bad json'}, 400)
        try:
            if self.path == '/key':
                # {"keys": ["s"], "down": true|false}
                r = keys_event(self.qmp, req['keys'], bool(req['down']))
            elif self.path == '/tap':
                # {"keys": [...], "ms": 300}: press, hold, release
                keys_event(self.qmp, req['keys'], True)
                time.sleep(min(int(req.get('ms', 300)), 5000) / 1000)
                r = keys_event(self.qmp, req['keys'], False)
            elif self.path == '/dial':
                # {"value": 1..11}: A6000 mode dial position (machine property)
                r = self.qmp.cmd('qom-set', path='/machine', property='mode-dial',
                                 value=str(int(req['value'])))
            elif self.path == '/vm':
                op = req.get('op')
                if op not in ('stop', 'cont', 'system_reset'):
                    return self._json({'error': 'bad op'}, 400)
                r = self.qmp.cmd(op)
            else:
                return self._json({'error': 'not found'}, 404)
        except Exception as e:
            return self._json({'error': str(e)}, 503)
        self._json({'ok': 'error' not in r, 'qmp': r})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--qmp', default=os.path.join(E, 'state', 'qmp.sock'))
    ap.add_argument('--launch', action='store_true',
                    help='start ./run.sh headless with the QMP socket')
    ap.add_argument('--model', default='a6000', choices=('a5000', 'a6000'))
    ap.add_argument('--port', type=int, default=8642)
    ap.add_argument('--bind', default='127.0.0.1')
    a, extra = ap.parse_known_args()

    proc = None
    if a.launch:
        if os.path.exists(a.qmp):
            os.unlink(a.qmp)
        log = open(os.path.join(E, 'state', 'camui-%s.log' % a.model), 'wb')
        env = dict(os.environ, A5K_MODEL=a.model)
        proc = subprocess.Popen(
            [os.path.join(E, 'run.sh'), '-display', 'none',
             '-qmp', 'unix:%s,server=on,wait=off' % a.qmp] + extra,
            env=env, stdout=log, stderr=subprocess.STDOUT)
        for _ in range(100):
            if os.path.exists(a.qmp) or proc.poll() is not None:
                break
            time.sleep(0.1)
        if proc.poll() is not None:
            sys.exit('emulator exited, see %s' % log.name)
        print('emulator pid %d, log %s' % (proc.pid, log.name), flush=True)

    Handler.qmp = QMP(a.qmp)
    shm = '/dev/shm' if os.path.isdir('/dev/shm') else '/tmp'
    Handler.shot = os.path.join(shm, 'camui-%d.ppm' % os.getpid())
    srv = ThreadingHTTPServer((a.bind, a.port), Handler)
    # SIGTERM/SIGHUP -> same cleanup as Ctrl-C (stop the emulator we launched)
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt))
    print('camera UI: http://localhost:%d' % a.port, flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        if proc:
            proc.terminate()
        if os.path.exists(Handler.shot):
            os.unlink(Handler.shot)


if __name__ == '__main__':
    main()

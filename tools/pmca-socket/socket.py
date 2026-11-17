"""USB over TCP ("A6KU") to the sony-a6000 QEMU emulator.

Backend for Sony-PMCA-RE, file pmca/usb/driver/generic/socket.py, selected
with `pmca-console.py <command> -d socket`.  The emulator's USB device model
(cxd90014-usbhdc) is the server; see docs/reference/RE_USB_HOST_PATH.md section 5 of
the emulator for the protocol.  Connecting plugs the virtual cable,
closing the connection (process exit) pulls it.

Environment:
  A6K_USB=host:port        server address (default 127.0.0.1:7643)
  A6K_USB_TIMEOUT=ms       transfer timeout (default 30000)
  A6K_USB_ATTACH_WAIT=s    how long the first connection waits for the
                           camera to pull up its gadget (default 180)
  A6K_USB_REATTACH_WAIT=s  after the camera dropped its gadget (switch to
                           app-install or updater mode, reboot), how long
                           the next device lookup waits for it to come
                           back (default 300; PMCA's own loops wait 5-30 s,
                           too short for an emulated re-enumeration)
  A6K_USB_DEBUG=1          print every A6KU message

Copyright (c) 2026 the a5000 emulator authors.  MIT license, like
Sony-PMCA-RE (see LICENSE next to this file in the emulator repository).
"""

import atexit
import collections
import os
import socket
import struct
import sys
import threading
import time

from . import *

MAGIC = 0x5541
HDR = struct.Struct('<HBBIII')    # magic, type, ep, id, length, arg

T_HELLO, T_RESET, T_CONTROL, T_OUT, T_IN, T_CANCEL = 1, 2, 3, 4, 5, 6
T_HELLO_ACK, T_EVENT = 0x81, 0x90
EV_ATTACHED, EV_DETACHED = 1, 2
ST_OK, ST_STALL, ST_NODEV, ST_CANCELLED, ST_OVERFLOW, ST_PROTO = range(6)
STATUS_NAMES = ['OK', 'STALL', 'NODEV', 'CANCELLED', 'OVERFLOW', 'PROTO']

Endpoint = collections.namedtuple('Endpoint', 'bEndpointAddress, bmAttributes, wMaxPacketSize')
Interface = collections.namedtuple('Interface', 'bInterfaceNumber, bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, endpoints')


def _env_addr():
 host, _, port = os.environ.get('A6K_USB', '127.0.0.1:7643').rpartition(':')
 return host or '127.0.0.1', int(port)

TIMEOUT = int(os.environ.get('A6K_USB_TIMEOUT', '30000')) / 1000.
ATTACH_WAIT = float(os.environ.get('A6K_USB_ATTACH_WAIT', '180'))
REATTACH_WAIT = float(os.environ.get('A6K_USB_REATTACH_WAIT', '300'))
DEBUG = os.environ.get('A6K_USB_DEBUG', '') not in ('', '0')


class _Device(object):
 """What one enumeration (one generation) found"""
 def __init__(self, gen, desc, config):
  self.gen = gen
  self.idVendor, self.idProduct = struct.unpack_from('<HH', desc, 8)
  self.desc = desc
  self.config = config
  self.bConfigurationValue = config[5]
  self.interfaces = []
  off = 0
  iface = None
  while off + 2 <= len(config):
   l, t = config[off], config[off + 1]
   if l < 2:
    break
   if t == 4 and l >= 9:
    if config[off + 3] == 0:    # alternate setting 0 only
     iface = Interface(config[off + 2], config[off + 5], config[off + 6], config[off + 7], [])
     self.interfaces.append(iface)
    else:
     iface = None
   elif t == 5 and l >= 7 and iface is not None:
    iface.endpoints.append(Endpoint(config[off + 2], config[off + 3], struct.unpack_from('<H', config, off + 4)[0]))
   off += l


class _Connection(object):
 def __init__(self, addr):
  self.sock = socket.create_connection(addr, timeout=2)
  self.sock.settimeout(None)
  self.lock = threading.Lock()
  self.cond = threading.Condition()
  self.pending = {}
  self.nextId = 1
  self.attached = False
  self.gen = 0
  self.speed = 0
  self.closed = False
  self.device = None
  self.connectTime = time.time()
  self.waitedAttach = False
  self.waitReattach = False
  self.thread = threading.Thread(target=self._reader, daemon=True)
  self.thread.start()
  status, data = self._request(T_HELLO, 0, struct.pack('<I', 1), 0, 10)
  version, attached, gen, speed = struct.unpack('<IIII', data[:16])
  with self.cond:
   self.attached = bool(attached)
   self.gen = gen
   self.speed = speed

 def close(self):
  try:
   self.sock.shutdown(socket.SHUT_RDWR)
  except OSError:
   pass
  self.sock.close()

 def _recvall(self, n):
  b = b''
  while len(b) < n:
   c = self.sock.recv(n - len(b))
   if not c:
    raise EOFError()
   b += c
  return b

 def _reader(self):
  try:
   while True:
    magic, type, ep, id, length, arg = HDR.unpack(self._recvall(HDR.size))
    data = self._recvall(length) if length else b''
    if magic != MAGIC:
     raise EOFError()
    if DEBUG:
     print('[a6ku] <- type 0x%x ep 0x%x id %d arg %d len %d' % (type, ep, id, arg, length), file=sys.stderr)
    with self.cond:
     if type == T_EVENT:
      gen, speed = struct.unpack('<II', data[:8])
      self.gen = gen
      self.speed = speed
      self.attached = arg == EV_ATTACHED
      if not self.attached:
       self.device = None
       self.waitReattach = True
     elif id in self.pending:
      self.pending[id] = (arg, data)
     self.cond.notify_all()
  except (EOFError, OSError):
   pass
  with self.cond:
   self.closed = True
   self.attached = False
   self.device = None
   self.cond.notify_all()

 def _send(self, type, ep, id, payload, arg):
  if DEBUG:
   print('[a6ku] -> type 0x%x ep 0x%x id %d arg %d len %d' % (type, ep, id, arg, len(payload)), file=sys.stderr)
  with self.lock:
   self.sock.sendall(HDR.pack(MAGIC, type, ep, id, len(payload), arg) + payload)

 def _request(self, type, ep, payload=b'', arg=0, timeout=None):
  """Sends a request and waits for its completion: (status, data)"""
  if timeout is None:
   timeout = TIMEOUT
  with self.cond:
   if self.closed:
    raise GenericUsbException('A6KU connection closed')
   id = self.nextId
   self.nextId = self.nextId % 0x7fffffff + 1
   self.pending[id] = None
  try:
   self._send(type, ep, id, payload, arg)
   end = time.time() + timeout
   with self.cond:
    while self.pending[id] is None and not self.closed:
     left = end - time.time()
     if left <= 0:
      break
     self.cond.wait(left)
    if self.pending[id] is not None:
     return self.pending[id]
    if self.closed:
     raise GenericUsbException('A6KU connection closed')
   # timeout: cancel, then wait for the CANCELLED (or a late) completion
   self._send(T_CANCEL, 0, 0, b'', id)
   with self.cond:
    end = time.time() + 2
    while self.pending[id] is None and not self.closed and time.time() < end:
     self.cond.wait(end - time.time())
   raise GenericUsbException('USB timeout')
  finally:
   with self.cond:
    self.pending.pop(id, None)

 def control(self, bmRequestType, bRequest, wValue, wIndex, data=b'', wLength=None, timeout=None):
  if wLength is None:
   wLength = len(data)
  setup = struct.pack('<BBHHH', bmRequestType, bRequest, wValue, wIndex, wLength)
  status, ret = self._request(T_CONTROL, 0, setup + (b'' if bmRequestType & 0x80 else data), 0, timeout)
  if status != ST_OK:
   raise GenericUsbException('Control transfer %02x %02x: %s' % (bmRequestType, bRequest, STATUS_NAMES[status] if status < len(STATUS_NAMES) else status))
  return ret

 def waitAttached(self, timeout):
  end = time.time() + timeout
  with self.cond:
   while not self.attached and not self.closed and time.time() < end:
    self.cond.wait(min(1, end - time.time()))
   return self.attached

 def ensureEnumerated(self):
  """Enumerates the current generation once (replaces the host OS's work)"""
  if not self.waitedAttach:
   # the first connection plugs the cable: wait for the gadget's pull-up
   self.waitedAttach = True
   if not self.attached:
    print('Waiting for the emulated camera to start its USB gadget...')
    self.waitAttached(ATTACH_WAIT)
  if self.waitReattach:
   # the gadget went away while we were connected: it is expected back
   self.waitReattach = False
   if not self.attached:
    print('Waiting for the emulated camera to reconnect...')
    self.waitAttached(REATTACH_WAIT)
  with self.cond:
   if not self.attached:
    return None
   if self.device and self.device.gen == self.gen:
    return self.device
   gen = self.gen
  status, data = self._request(T_RESET, 0, b'', 0)
  if status != ST_OK:
   raise GenericUsbException('Bus reset failed: %s' % STATUS_NAMES[status])
  desc = self.control(0x80, 6, 0x0100, 0, wLength=18)
  self.control(0x00, 5, 1, 0)
  head = self.control(0x80, 6, 0x0200, 0, wLength=9)
  total = struct.unpack_from('<H', head, 2)[0]
  config = self.control(0x80, 6, 0x0200, 0, wLength=total)
  dev = _Device(gen, desc, config)
  self.control(0x00, 9, dev.bConfigurationValue, 0)
  if DEBUG:
   print('[a6ku] enumerated %04x:%04x gen %d, interface 0 class %d' % (dev.idVendor, dev.idProduct, gen, dev.interfaces[0].bInterfaceClass if dev.interfaces else -1), file=sys.stderr)
  with self.cond:
   if self.gen != gen:
    return None
   self.device = dev
  return dev


_conn = None

def _getConnection():
 global _conn
 if _conn is not None and _conn.closed:
  _conn.close()
  _conn = None
 if _conn is None:
  try:
   _conn = _Connection(_env_addr())
  except (OSError, GenericUsbException):
   _conn = None
 return _conn

@atexit.register
def _closeConnection():
 if _conn is not None:
  _conn.close()


class _UsbContext(BaseUsbContext):
 def __init__(self, name, classType, driverClass):
  super(_UsbContext, self).__init__('socket-%s' % name, classType)
  self._driverClass = driverClass

 def __enter__(self):
  _getConnection()
  return self

 def listDevices(self, vendor):
  conn = _getConnection()
  if not conn:
   return
  try:
   dev = conn.ensureEnumerated()
  except GenericUsbException as e:
   print('USB enumeration failed: %s' % e)
   return
  if dev and dev.idVendor == vendor and dev.interfaces and dev.interfaces[0].bInterfaceClass == self.classType:
   yield UsbDeviceHandle(UsbBackend(conn, dev), dev.idVendor, dev.idProduct)

 def openDevice(self, device):
  return self._driverClass(device.handle)


class MscContext(_UsbContext):
 def __init__(self):
  super(MscContext, self).__init__('MSC', USB_CLASS_MSC, None)

 def openDevice(self, device):
  protocol = device.handle.device.interfaces[0].bInterfaceProtocol
  if protocol in (0, 1):
   return MscCbiDriver(device.handle)
  elif protocol == 0x50:
   return MscBbbDriver(device.handle)
  else:
   raise Exception('Invalid protocol')

class MtpContext(_UsbContext):
 def __init__(self):
  super(MtpContext, self).__init__('MTP', USB_CLASS_PTP, MtpDriver)

class VendorSpecificContext(_UsbContext):
 def __init__(self):
  super(VendorSpecificContext, self).__init__('vendor-specific', USB_CLASS_VENDOR_SPECIFIC, GenericUsbDriver)


class UsbBackend(BaseUsbBackend):
 """Transfers to the enumerated emulated device of one generation"""

 def __init__(self, conn, device):
  self.conn = conn
  self.device = device

 def _check(self):
  if self.conn.closed or self.conn.gen != self.device.gen or not self.conn.attached:
   raise GenericUsbException('Device detached')

 def reset(self):
  # No bus reset: that would reset the camera's function state for nothing
  self._check()

 def clearHalt(self, ep):
  self._check()
  self.conn.control(0x02, 1, 0, ep)

 def getId(self):
  return self.device.idVendor, self.device.idProduct

 def getEndpoints(self):
  return self.device.interfaces[0].endpoints

 def read(self, ep, length, timeout=None):
  self._check()
  status, data = self.conn._request(T_IN, ep, b'', length, timeout / 1000. if timeout else None)
  if status != ST_OK:
   raise GenericUsbException('Read from 0x%x: %s' % (ep, STATUS_NAMES[status] if status < len(STATUS_NAMES) else status))
  return data

 def write(self, ep, data):
  self._check()
  status, ret = self.conn._request(T_OUT, ep, bytes(data), 0)
  if status != ST_OK:
   raise GenericUsbException('Write to 0x%x: %s' % (ep, STATUS_NAMES[status] if status < len(STATUS_NAMES) else status))

 def classInterfaceRequestOut(self, request, value, index, data=b''):
  self._check()
  self.conn.control(0x21, request, value, index, data)

 def vendorRequestOut(self, request, value, index, data=b''):
  self._check()
  self.conn.control(0x43, request, value, index, data)

#!/usr/bin/env python3
"""Print the guest kernel log from emulator RAM.
Reads the Sony persistent klog ring (cmdline klog.addr=0x834E0000,
klog.size=0x20000), which receives every printk as plain text.
usage: klog.py <monitor-port> [tmpdir]"""
import sys, os, re, subprocess, tempfile
port = sys.argv[1]
tmp = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp()
out = os.path.join(tmp, 'klog_ram.bin')
here = os.path.dirname(os.path.abspath(__file__))
subprocess.run([sys.executable, os.path.join(here, 'mon.py'), port,
                'pmemsave 0x834E0000 0x20000 "%s"' % out], capture_output=True)
d = open(out, 'rb').read()
i = d.find(b'[    0.000000]')
d = d[i:] if i >= 0 else d
print(re.sub(rb'\0+', b'', d).decode('latin1').rstrip())

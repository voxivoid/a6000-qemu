# Poll the emulator monitor for guest T_osd: jiffies(0x80360240,HZ=100) when
# AIF 0xF3440144 bit1 (panel OSD on) first sets.  Uniform for stock & hook.
import socket,sys,re,time
port=int(sys.argv[1]); label=sys.argv[2] if len(sys.argv)>2 else "?"
deadline=time.time()+ (int(sys.argv[3]) if len(sys.argv)>3 else 200)
def mon(s,cmd):
    s.sendall(cmd.encode()+b'\n'); b=b''
    try:
        while not b.endswith(b'(qemu) '):
            c=s.recv(65536)
            if not c: break
            b+=c
    except socket.timeout: pass
    return b.decode('latin1',errors='replace')
def word(s,addr):
    o=mon(s,'xp /1wx 0x%x'%addr)
    m=re.search(r':\s*0x([0-9a-fA-F]+)',o); return int(m.group(1),16) if m else None
first=None
while time.time()<deadline:
    try:
        s=socket.create_connection(("127.0.0.1",port),timeout=3); s.settimeout(3); mon(s,'')
    except Exception as e:
        time.sleep(0.5); continue
    j=word(s,0x80360240); reg=word(s,0xF3440144); s.close()
    if j is None or reg is None: time.sleep(0.3); continue
    gs=j/100.0
    on=(reg>>1)&1
    if on and first is None:
        first=gs; print("%s OSD_ON guest_s=%.2f jiffies=%d reg=0x%08x"%(label,gs,j,reg),flush=True); break
    time.sleep(0.3)
if first is None: print("%s OSD_ON NOT_SEEN"%label,flush=True)

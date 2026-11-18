import capstone,struct,sys,re,json
d=open('/home/voxivoid/code/a6000-qemu/firmware/unpacked/firmware.tar_unpacked/0700_part_image/dev/nflasha3_unpacked_unpacked/av-cam.bin','rb').read()
VB=0x43541000
st=int(sys.argv[1],16); en=int(sys.argv[2],16)
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB); md.skipdata=True
ins=list(md.disasm(d[st-VB:en-VB],st))
regs={}
i=0
while i<len(ins)-3:
    a=ins[i]
    # leaf accessor: [add.w r0,r0,#X] [add.w r0,r0,#Y]? (ldr|str) r?, [r0] [and/uxth/ubfx] bx lr
    m=re.match(r'r0, r0, #(0x[0-9a-f]+)$',a.op_str) if a.mnemonic=='add.w' else None
    if m:
        off=int(m.group(1),16); j=i+1
        while j<len(ins) and ins[j].mnemonic=='add.w' and re.match(r'r0, r0, #',ins[j].op_str):
            off+=int(ins[j].op_str.split('#')[1],16); j+=1
        if j<len(ins) and ins[j].mnemonic in('ldr','str','ldrh','strh','ldrb','strb') and re.search(r'\[r0(, #(0x[0-9a-f]+))?\]',ins[j].op_str):
            mm=re.search(r'\[r0(, #(0x[0-9a-f]+))?\]',ins[j].op_str); off+=int(mm.group(2),16) if mm.group(2) else 0
            kind='R' if ins[j].mnemonic.startswith('ldr') else 'W'
            post=''
            k=j+1
            while k<len(ins) and ins[k].mnemonic!='bx' and k<j+6:
                post+=ins[k].mnemonic+' '+ins[k].op_str+'; '; k+=1
            e=regs.setdefault(off,{'R':[], 'W':[]})
            e[kind].append((hex(a.address),post.strip()))
            i=j; continue
    i+=1
json.dump({hex(k):v for k,v in sorted(regs.items())},open(sys.argv[3],'w'),indent=0)
print(len(regs),'regs',hex(min(regs)),hex(max(regs)))

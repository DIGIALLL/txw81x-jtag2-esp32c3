from capstone import *
import collections, sys
d=open("fw.bin","rb").read(); base=0x8000000
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB+CS_MODE_MCLASS); md.skipdata=True
calls=collections.defaultdict(list); lines=[]
for i in md.disasm(d[0x4000:], base+0x4000):
    lines.append(f"{i.address:08x}  {i.mnemonic:8} {i.op_str}")
    if i.mnemonic in("bl","b.w") and i.op_str.startswith("#"):
        calls[int(i.op_str[1:],16)].append(i.address)
open("all.asm","w").write("\n".join(lines))
for t in (0x80060f4,0x80086c0,0x80086ec,0x8004940):
    print(hex(t), len(calls[t]), sorted(set(a&~0xff for a in calls[t]))[:40])

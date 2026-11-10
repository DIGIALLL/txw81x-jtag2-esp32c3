import sys, struct
from intelhex import IntelHex
from capstone import *
ih = IntelHex(sys.argv[1])
segs = ih.segments(); print("segments", [(hex(a),hex(b)) for a,b in segs])
lo, hi = segs[0][0], segs[-1][1]
data = bytes(ih.tobinarray(start=lo, end=hi-1))
sp, rst = struct.unpack_from("<II", data, 0); print("SP", hex(sp), "Reset", hex(rst))
# literal pool refs to GPIO registers
regs = {0x40010800:"GPIOA",0x40010C00:"GPIOB",0x40011000:"GPIOC"}
for off in range(0, len(data)-3, 4):
    v = struct.unpack_from("<I", data, off)[0]
    for base,n in regs.items():
        if base <= v < base+0x20:
            print(f"lit @{lo+off:08x}: {n}+{v-base:#x}")
open("fw.bin","wb").write(data); print("base", hex(lo), "len", len(data))

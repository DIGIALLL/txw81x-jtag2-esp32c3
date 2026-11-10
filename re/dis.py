import sys
from capstone import *
d=open("fw.bin","rb").read(); base=0x8000000
md=Cs(CS_ARCH_ARM, CS_MODE_THUMB+CS_MODE_MCLASS)
a=int(sys.argv[1],16); n=int(sys.argv[2],16)
for i in md.disasm(d[a-base:a-base+n], a): print(f"{i.address:08x}  {i.mnemonic:8} {i.op_str}")

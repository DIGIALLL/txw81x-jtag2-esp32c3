import pefile,struct,sys
from capstone import *
pe=pefile.PE("Target.dll"); IB=pe.OPTIONAL_HEADER.ImageBase
d=open("Target.dll","rb").read()
text=[s for s in pe.sections if s.Name.startswith(b".text")][0]
tb=d[text.PointerToRawData:text.PointerToRawData+text.SizeOfRawData]; tva=IB+text.VirtualAddress
def f2va(off):
    for s in pe.sections:
        if s.PointerToRawData<=off<s.PointerToRawData+s.SizeOfRawData: return IB+s.VirtualAddress+off-s.PointerToRawData
def refs(va):
    b=struct.pack("<I",va); r=[];i=tb.find(b)
    while i>=0: r.append(tva+i); i=tb.find(b,i+1)
    return r
md=Cs(CS_ARCH_X86,CS_MODE_32)
def dis(va,n=0x200):
    o=va-tva
    for i in md.disasm(tb[o:o+n],va): print(f"{i.address:08x} {i.mnemonic:7} {i.op_str}")
if __name__=="__main__":
    for s in sys.argv[1:]:
        off=int(s,16); va=f2va(off); print(s,"va",hex(va),"refs",[hex(x) for x in refs(va)])

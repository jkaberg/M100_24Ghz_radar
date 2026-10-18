# linear-sweep disassembly of whole image (skip undecodable halfwords); annotates PC-relative literal loads
import sys,capstone,struct
d=open(sys.argv[1],'rb').read()
end=int(sys.argv[2],16) if len(sys.argv)>2 else len(d)
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB|capstone.CS_MODE_MCLASS)
md.skipdata=True
B=0x08000000
for i in md.disasm(d[:end],B):
    s=f"{i.address-B:05x}: {i.bytes.hex():10s} {i.mnemonic} {i.op_str}"
    if i.mnemonic.startswith('ldr') and '[pc' in i.op_str:
        try:
            imm=int(i.op_str.split('#')[-1].rstrip(']'),16) if '#' in i.op_str else 0
            a=((i.address+4)&~3)+imm-B
            v=struct.unpack_from('<I',d,a)[0]
            s+=f"   ; [{a:05x}]=0x{v:08x}"
        except Exception as e: pass
    print(s)

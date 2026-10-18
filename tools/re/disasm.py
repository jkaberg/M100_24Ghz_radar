import sys,capstone
d=open(sys.argv[1],'rb').read()
start=int(sys.argv[2],16); n=int(sys.argv[3],16) if len(sys.argv)>3 else 0x80
md=capstone.Cs(capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB|capstone.CS_MODE_MCLASS)
for i in md.disasm(d[start:start+n],0x08000000+start):
    print(f"{i.address-0x08000000:05x}: {i.bytes.hex():10s} {i.mnemonic} {i.op_str}")

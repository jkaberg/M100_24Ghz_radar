import sys,struct
d=open(sys.argv[1],'rb').read()
B=0x08000000
# candidate vector tables: SP in RAM, reset vector odd and inside flash
for off in range(0,len(d)-8,0x100):
    sp,rv=struct.unpack_from('<II',d,off)
    if 0x20000000<=sp<=0x20002000 and rv&1 and B<=rv<B+len(d):
        print('vector table @',hex(off),'sp',hex(sp),'reset',hex(rv))
for name,c in [('FMC_KEY1',0x45670123),('FMC_KEY2',0xCDEF89AB),('RTC_BASE',0x40002800),('FMC_BASE',0x40022000),('BKP?',0x40002850)]:
    p=struct.pack('<I',c); i=d.find(p)
    while i!=-1:
        print(name,hex(c),'literal @',hex(i)); i=d.find(p,i+1)
# ascii strings
import re
for m in re.finditer(rb'[\x20-\x7e]{5,}',d):
    print('str',hex(m.start()),m.group())

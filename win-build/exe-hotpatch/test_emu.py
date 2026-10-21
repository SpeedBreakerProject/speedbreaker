# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the patched exe's real machine code in an emulator (Unicorn) from the AudioThread call site,
# with the Windows functions stubbed, and checks arguments, stack alignment, saved registers and fallbacks.
# Needs: pip install pefile unicorn.   usage: test_emu.py SpeedBreaker-hrtimer.exe
import sys, pefile, struct
from unicorn import *
from unicorn.x86_const import *
EXE=sys.argv[1]
pe=pefile.PE(EXE); base=pe.OPTIONAL_HEADER.ImageBase
iat={}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    for imp in d.imports:
        if imp.name and imp.name.decode() in ('GetModuleHandleW','GetProcAddress','WaitForSingleObject','CloseHandle'):
            iat[imp.name.decode()]=imp.address
ORIG_NS=0x141d59e90; START=0x1400020e0; END=0x140002100
STUB=0x600000000000; STACK=0x700000000000; STACK_SZ=0x100000
names=['GetModuleHandleW','GetProcAddress','WaitForSingleObject','CloseHandle','CreateWaitableTimerExW','SetWaitableTimer']
stub={n:STUB+0x10*i for i,n in enumerate(names)}
rev={v:k for k,v in stub.items()}
HK32=0x7FFE0000; HTIMER=0x1234

def run(sec,nsec,fail=None,verbose=False):
    uc=Uc(UC_ARCH_X86,UC_MODE_64)
    for s in pe.sections:
        va=base+s.VirtualAddress; size=(max(s.Misc_VirtualSize,s.SizeOfRawData)+0xfff)&~0xfff
        uc.mem_map(va,size); uc.mem_write(va,s.get_data())
    uc.mem_map(STUB,0x1000); uc.mem_write(STUB,b'\xc3'*0x100)
    uc.mem_map(STACK,STACK_SZ)
    for n in ('GetModuleHandleW','GetProcAddress','WaitForSingleObject','CloseHandle'):
        uc.mem_write(iat[n],struct.pack('<Q',stub[n]))
    uc.mem_write(ORIG_NS,b'\xc3')                      # emulator-only: original nanosleep just returns
    rsp=STACK+STACK_SZ-0x400; assert rsp%16==0
    uc.mem_write(rsp+0x28,struct.pack('<qi',sec,nsec))
    sentinel={UC_X86_REG_RBX:0xB1,UC_X86_REG_RBP:0xB2,UC_X86_REG_RSI:0xB3,UC_X86_REG_RDI:0xB4,
              UC_X86_REG_R12:0xB5,UC_X86_REG_R13:0xB6,UC_X86_REG_R14:0xB7,UC_X86_REG_R15:0xB8}
    for r,v in sentinel.items(): uc.reg_write(r,v)
    xs={}
    for i in range(6,16):
        r=getattr(__import__('unicorn.x86_const',fromlist=['x']),'UC_X86_REG_XMM%d'%i); xs[r]=(0xAA00+i)|(i<<64); uc.reg_write(r,xs[r])
    uc.reg_write(UC_X86_REG_RSP,rsp)
    calls=[]
    def rd_cstr(a):
        b=b''
        while True:
            c=uc.mem_read(a,1)
            if c==b'\0': return b.decode()
            b+=bytes(c); a+=1
    def rd_wstr(a):
        b=b''
        while True:
            c=bytes(uc.mem_read(a,2))
            if c==b'\0\0': return b.decode('utf-16le')
            b+=c; a+=2
    def hook(uc_,addr,size,_):
        rsp_=uc.reg_read(UC_X86_REG_RSP)
        if addr==ORIG_NS:
            rcx=uc.reg_read(UC_X86_REG_RCX); rdx=uc.reg_read(UC_X86_REG_RDX)
            calls.append(('nanosleep',rcx==rdx==rsp+0x28 if False else (rcx,rdx)))
            assert rsp_%16==8,"misaligned at nanosleep"   # entered via jmp: rsp is as at original call
            uc.reg_write(UC_X86_REG_RAX,0); return
        if addr not in rev: return
        n=rev[addr]
        assert rsp_%16==8, "stack misaligned entering %s (rsp=%#x)"%(n,rsp_)
        rcx=uc.reg_read(UC_X86_REG_RCX); rdx=uc.reg_read(UC_X86_REG_RDX); r8=uc.reg_read(UC_X86_REG_R8); r9=uc.reg_read(UC_X86_REG_R9)
        ret=0
        if n=='GetModuleHandleW':
            calls.append((n,rd_wstr(rcx))); ret=0 if fail==n else HK32
        elif n=='GetProcAddress':
            fn=rd_cstr(rdx); assert rcx==HK32; calls.append((n,fn))
            ret=0 if fail==fn else stub[fn]
        elif n=='CreateWaitableTimerExW':
            calls.append((n,(rcx,rdx,r8&0xffffffff,r9&0xffffffff))); ret=0 if fail==n else HTIMER
        elif n=='SetWaitableTimer':
            due=struct.unpack('<q',bytes(uc.mem_read(rdx,8)))[0]
            a5=struct.unpack('<Q',bytes(uc.mem_read(rsp_+0x28,8)))[0]; a6=struct.unpack('<Q',bytes(uc.mem_read(rsp_+0x30,8)))[0]
            calls.append((n,(rcx,due,r8,r9,a5,a6&0xffffffff))); ret=0 if fail=='SetWaitableTimer!' else 1
        elif n=='WaitForSingleObject': calls.append((n,(rcx,edx_:=rdx&0xffffffff)))
        elif n=='CloseHandle': calls.append((n,rcx)); ret=1
        uc.reg_write(UC_X86_REG_RAX,ret)
    uc.hook_add(UC_HOOK_CODE,hook,begin=STUB,end=STUB+0x100)
    uc.hook_add(UC_HOOK_CODE,hook,begin=ORIG_NS,end=ORIG_NS)
    uc.emu_start(START,END,count=100000)
    # post-conditions
    assert uc.reg_read(UC_X86_REG_RIP)==END
    assert uc.reg_read(UC_X86_REG_RSP)==rsp, "rsp not restored"
    for r,v in sentinel.items(): assert uc.reg_read(r)==v, "callee-saved reg clobbered %d"%r
    for r,v in xs.items(): assert uc.reg_read(r)==v, "xmm clobbered"
    assert uc.reg_read(UC_X86_REG_RAX)&0xffffffff==0
    return calls,rsp

def show(title,calls): print(title); [print("   ",c) for c in calls]
ok=lambda c:[x[0] for x in c]
# 1 normal
c,rsp=run(0,5333333); show("1. 5.333333 ms tick",c)
assert ok(c)==['GetModuleHandleW','GetProcAddress','CreateWaitableTimerExW','GetProcAddress','SetWaitableTimer','WaitForSingleObject','CloseHandle']
assert c[0][1]=='kernel32.dll' and c[1][1]=='CreateWaitableTimerExW' and c[3][1]=='SetWaitableTimer'
assert c[2][1]==(0,0,2,0x1F0003)
assert c[4][1]==(HTIMER,-53334,0,0,0,0) and c[5][1]==(HTIMER,0xFFFFFFFF) and c[6][1]==HTIMER
# due-time arithmetic
for sec,nsec,exp in [(0,0,-1),(0,1,-1),(0,100,-1),(0,101,-2),(3,250000000,-32500000),(0,999999999,-10000000),(86400,0,-864000000000)]:
    c,_=run(sec,nsec); assert c[4][1][1]==exp,(sec,nsec,c[4]); 
print("2. due-time arithmetic OK for 7 timespecs (ceil to 100 ns, relative/negative)")
# fallbacks
for fail,expect in [('GetModuleHandleW',['GetModuleHandleW','nanosleep']),
                    ('CreateWaitableTimerExW',['GetModuleHandleW','GetProcAddress','nanosleep']),
                    ('SetWaitableTimer',['GetModuleHandleW','GetProcAddress','CreateWaitableTimerExW','GetProcAddress','CloseHandle','nanosleep'])]:
    c,rsp=run(0,5333333,fail=fail); assert ok(c)==expect,(fail,ok(c)); 
    ns=[x for x in c if x[0]=='nanosleep'][0][1]; assert ns==(rsp+0x28,rsp+0x28),"fallback must pass the original timespec pointer twice"
    print("3. fallback when %-22s -> %s"%(fail+' fails',' > '.join(ok(c))))
c,rsp=run(0,5333333,fail='SetWaitableTimer!'); assert ok(c)==['GetModuleHandleW','GetProcAddress','CreateWaitableTimerExW','GetProcAddress','SetWaitableTimer','CloseHandle','nanosleep'],ok(c); print('3. fallback when the SetWaitableTimer call itself fails -> timer closed, then nanosleep')
# SetWaitableTimer proc not found (GetProcAddress 2nd call fails) -> close timer then fallback
c,rsp=run(0,5333333,fail='SetWaitableTimer'); 
print("ALL EMULATION CHECKS PASSED (stack alignment 16 at every call, callee-saved + xmm6-15 preserved, rsp restored, eax=0)")

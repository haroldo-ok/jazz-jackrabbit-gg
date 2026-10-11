#!/usr/bin/env python3
"""prof2.py dump.txt obj/game.noi obj/*.rst [--frames N] [--top N] [--asm FUNC]
Exclusive cycles folded onto the nearest preceding code label (all labels in the
relocated listings, so statics and inline-asm routines count), plus inclusive
per-function cost from the shadow call stack. --asm FUNC prints per-instruction
exclusive cycles inside FUNC (from the .rst lines)."""
import sys, re, bisect, collections
args=sys.argv[1:]; opt={}
for k in ('--frames','--top','--asm','--tree','--depth'):
    if k in args: i=args.index(k); opt[k]=args[i+1]; del args[i:i+2]
dump,noi,rsts=args[0],args[1],args[2:]
syms={}
for l in open(noi):
    m=re.match(r'DEF\s+(\S+)\s+0x([0-9A-Fa-f]+)',l)
    if m:
        a=int(m.group(2),16); n=m.group(1)
        if a>=0x10000 and 0x8000<=(a&0xFFFF)<0xC000: a&=0xFFFF
        if n in ('_SMS_SRAM',): continue
        if (a<0xC000) and not n.startswith(('l_','s_','.')) and '$' not in n: syms.setdefault(a,n.lstrip('_'))
lab=re.compile(r'^\s+([0-9A-F]{6})\s+\d+\s+([A-Za-z_][A-Za-z0-9_]*)::?')
lines={}   # addr -> source text (for --asm)
ins=re.compile(r'^\s+([0-9A-F]{6})\s+([0-9A-F ]{2,12}?)\s+\[\s*\d+\]\s+\d+\s+(.*)$')
for r in rsts:
    for l in open(r,errors='replace'):
        m=lab.match(l)
        if m:
            a=int(m.group(1),16)
            if a>=0x10000 and 0x8000<=(a&0xFFFF)<0xC000: a&=0xFFFF
            if a<0xC000: syms[a]=m.group(2).lstrip('_')
        m=ins.match(l)
        if m:
            a=int(m.group(1),16)
            if a>=0x10000 and 0x8000<=(a&0xFFFF)<0xC000: a&=0xFFFF
            lines[a]=m.group(3).strip()
addrs=sorted(syms); names=[syms[a] for a in addrs]
def owner(a):
    if a in (0x38,0x66): return 'IRQ' if a==0x38 else 'NMI'
    if a==0xFFFFFF: return 'ROOT'
    i=bisect.bisect_right(addrs,a)-1
    return names[i] if i>=0 else '?%06X'%a
T=0; ex=collections.Counter(); per={}; inc=[]; edg=[]
for l in open(dump):
    p=l.split()
    if p[0]=='T': T=int(p[1])
    elif p[0]=='X':
        a=int(p[1],16); c=int(p[2]); ex[owner(a)]+=c; per[a]=c
    elif p[0]=='I': inc.append((int(p[3]),int(p[2]),owner(int(p[1],16)),int(p[1],16)))
    elif p[0]=='E': edg.append((int(p[1],16),int(p[2],16),int(p[3]),int(p[4])))
fr=int(opt.get('--frames',0)); top=int(opt.get('--top',40))
def fmt(c): return '%10d %5.1f%%'%(c,100*c/T)+(('  %8.0f/gf'%(c/fr)) if fr else '')
print('total %d cycles (%.0f console frames)%s'%(T,T/59736,(', %d game frames -> %.0f cycles/game frame'%(fr,T/fr)) if fr else ''))
print('\n== exclusive (self) ==')
for n,c in ex.most_common(top): print('%-22s'%n,fmt(c))
print('\n== inclusive (entry -> return, ISR time excluded) ==')
for c,calls,n,a in sorted(inc,reverse=True)[:top]: print('%-22s'%n,fmt(c),' calls %7d  %7.0f cyc/call'%(calls,c/max(calls,1)))
if '--asm' in opt:
    f=opt['--asm']; a0=[a for a in addrs if syms[a]==f][0]; a1=addrs[addrs.index(a0)+1]
    print('\n== %s per instruction =='%f)
    for a in range(a0,a1):
        if a in lines: print('%06X %10d  %s'%(a,per.get(a,0),lines[a]))

if '--tree' in opt:
    root=opt['--tree']; depth=int(opt.get('--depth','3')) if False else 3
    kids=collections.defaultdict(list)
    for a,b,n,c in edg: kids[owner(a)].append((c,n,owner(b)))
    agg=collections.defaultdict(lambda:[0,0])
    def show(f,d,seen):
        kk=collections.defaultdict(lambda:[0,0])
        for c,n,b in kids.get(f,[]): kk[b][0]+=c; kk[b][1]+=n
        for b,(c,n) in sorted(kk.items(),key=lambda x:-x[1][0]):
            if c<T*0.004: continue
            print('  '*d+'%-24s'%b, fmt(c), ' calls %6d %7.0f/call'%(n,c/n))
            if d<int(opt.get('--depth',4)) and b not in seen: show(b,d+1,seen|{b})
    print('\n== call tree from %s (edges >= 0.4%%) =='%root); show(root,1,{root})

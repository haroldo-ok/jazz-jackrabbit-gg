#!/usr/bin/env python3
"""cmp.py a.bin b.bin: first differing frame record, decoded."""
import struct, sys
R = 20
def recs(p):
    d = open(p, 'rb').read()
    return [d[i:i + R] for i in range(0, len(d) - R + 1, R)]
def dec(r):
    f, x, y, vx, vy, sc = struct.unpack_from('<Hhhhh H', r, 0)
    return dict(frame=f, x=x, y=y, vx=vx, vy=vy, score=sc, health=r[12], lives=r[13], stage=r[14],
                gems=r[15], enemies=r[16], esum=r[17] | (r[18] << 8), bullets=r[19])
a, b = recs(sys.argv[1]), recs(sys.argv[2])
n = min(len(a), len(b))
for i in range(n):
    if a[i] != b[i]:
        print('DIVERGE at record %d' % i)
        for j in range(max(0, i - 2), min(n, i + 2)):
            print('  A', dec(a[j]))
            print('  B', dec(b[j]))
        sys.exit(1)
print('MATCH %d records (lengths %d / %d)' % (n, len(a), len(b)))

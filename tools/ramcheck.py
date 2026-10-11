#!/usr/bin/env python3
"""Fail the build if the C data would overlap the fixed tables in render.c
(blockRef at 0xD700, blockSlot 0xD800, slotRef 0xDC00; stack below 0xDFF0)."""
import re, sys
m = open(sys.argv[1]).read()
def sym(n):
    return int(re.search(r"([0-9A-F]{8})\s+" + n + r"\b", m).group(1), 16)
end = sym("s__INITIALIZED") + sym("l__INITIALIZED")
if end > 0xD700:
    sys.exit("RAM: C data ends at 0x%04X, past the fixed tables at 0xD700" % end)
print("RAM: C data ends at 0x%04X (fixed tables from 0xD700)" % end)

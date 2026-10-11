#!/usr/bin/env python3
"""Assemble the final .gg: code ROM (fixed 32 KB + logic code bank) followed
by the converter's data banks at DATA_BANK0, padded to a power of two."""
import re, sys
code_path, data_path, hdr_path, out_path = sys.argv[1:5]
code = bytearray(open(code_path, "rb").read())
data = open(data_path, "rb").read()
bank0 = int(re.search(r"DATA_BANK0 (\d+)", open(hdr_path).read()).group(1))
limit = bank0 * 0x4000
if len(code) > limit:
    sys.exit("code ROM is %d bytes, overlaps data bank %d" % (len(code), bank0))
rom = code + bytes(limit - len(code)) + data
size = 0x8000
while size < len(rom):
    size <<= 1
rom += b"\xFF" * (size - len(rom))
open(out_path, "wb").write(rom)
print("%s: %d KB (code %d bytes, data banks %d..%d)" % (out_path, size // 1024, len(code),
      bank0, bank0 + len(data) // 0x4000 - 1))

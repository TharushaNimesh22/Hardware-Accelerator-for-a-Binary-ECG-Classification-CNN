#!/usr/bin/env python3
"""firmware.bin -> firmware_b0..b3.hex (one byte lane per file, 8192 entries each)"""
import sys
data = open(sys.argv[1], "rb").read()
words = 16384
assert len(data) <= words * 4, "firmware larger than 32 KB RAM"
data = data + bytes(words * 4 - len(data))
for lane in range(4):
    with open(f"{sys.argv[2]}_b{lane}.hex", "w") as f:
        f.write("".join(f"{data[4*i+lane]:02x}\n" for i in range(words)))
print(f"{len(data.rstrip(bytes(1)))} bytes used of {words*4}")

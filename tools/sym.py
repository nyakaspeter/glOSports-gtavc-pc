"""Attribute profile samples to the nearest preceding symbol in an ARM binary.

Usage (inside a container with binutils-arm-linux-gnueabihf):
    python3 sym.py BINARY OFFSETS.txt [TOP_N]
Stripped libraries (libc, libMali) only have exported symbols, so a large
share landing on one name usually means "some internal function after it".
"""
import bisect
import collections
import subprocess
import sys

lib, offs = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 25
syms = []
for flags in (['-D'], []):
    r = subprocess.run(['arm-linux-gnueabihf-nm', '-C', '--defined-only'] + flags + [lib],
                       capture_output=True, text=True)
    for l in r.stdout.splitlines():
        p = l.split(None, 2)
        if len(p) == 3 and p[1] in 'tTwW':
            syms.append((int(p[0], 16) & ~1, p[2]))
syms.sort()
addrs = [a for a, _ in syms]
cnt = collections.Counter()
total = 0
for l in open(offs):
    a, c = l.split()
    a, c = int(a, 16), int(c)
    total += c
    i = bisect.bisect_right(addrs, a) - 1
    cnt[syms[i][1] if i >= 0 else '?'] += c
print(f'{lib.split("/")[-1]}: {total} samples, {len(syms)} symbols')
for k, v in cnt.most_common(top):
    print(f'{100 * v / total:5.1f}%  {k[:110]}')

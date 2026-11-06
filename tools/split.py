"""Split a re3prof.txt dump (from shim/glos_prof.cpp) per library.

Usage: python3 split.py re3prof.txt OUTDIR
Prints each library's share of samples and writes OUTDIR/<lib>.txt with
"<file offset hex> <count>" lines, ready for sym.py.
"""
import collections
import os
import sys

maps, samples = [], []
for line in open(sys.argv[1]):
    p = line.split()
    if not p:
        continue
    if p[0] == 'map':
        lo, hi = (int(x, 16) for x in p[1].split('-'))
        maps.append((lo, hi, int(p[3], 16), os.path.basename(p[-1])))
    elif p[0] != 'samples' and len(p) == 2:
        samples.append((int(p[0], 16), int(p[1])))

total = sum(c for _, c in samples)
perlib = collections.defaultdict(list)
for pc, c in samples:
    for lo, hi, off, name in maps:
        if lo <= pc < hi:
            perlib[name].append((pc - lo + off, c))
            break
    else:
        perlib['?'].append((pc, c))

os.makedirs(sys.argv[2], exist_ok=True)
print(f'{total} samples')
for name, rows in sorted(perlib.items(), key=lambda kv: -sum(c for _, c in kv[1])):
    share = sum(c for _, c in rows) * 100 / total
    if share >= 0.5:
        print(f'{share:5.1f}%  {name}')
    with open(os.path.join(sys.argv[2], name + '.txt'), 'w') as f:
        for off, c in rows:
            f.write(f'{off:x} {c}\n')

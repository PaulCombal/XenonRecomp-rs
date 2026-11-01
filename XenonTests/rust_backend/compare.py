#!/usr/bin/env python3
"""Compares the two harness outputs and reports what differs, per case."""
import collections
import re
import sys

cpp, rust, cases_path = sys.argv[1:4]
texts = [re.match(r'\{ 0x([0-9A-F]+), (\d), "(.*)" \},', l).group(3) for l in open(cases_path)]

runs = same = nan = 0
faulted = 0
bad = collections.defaultdict(list)
for a, b in zip(open(cpp), open(rust)):
    x, y = a.split(), b.split()
    case, inp = int(x[0]), int(x[1])
    runs += 1
    if x[2] != '0':
        faulted += 1
    if x == y:
        same += 1
    elif x[2:6] == y[2:5] + y[5:6] and x[2:5] == y[2:5] and x[6] == y[6]:
        nan += 1
    else:
        what = 'outcome' if x[2:4] != y[2:4] else 'memory' if x[4] != y[4] else 'registers'
        bad[case].append((inp, what))

print(f'{runs} runs: {same} identical, {nan} differ only in a NaN sign or payload, {sum(map(len, bad.values()))} DIFFERENT ({faulted} ended in a fault or trap)')
by_name = collections.Counter()
for case, items in bad.items():
    by_name[texts[case].split()[0]] += len(items)
for name, n in by_name.most_common(60):
    print(f'  {n:6} {name}')
shown = 0
for case, items in sorted(bad.items()):
    if shown < 40:
        print(f'  case {case} [{texts[case]}]: input {items[0][0]} {items[0][1]} ({len(items)} inputs)')
        shown += 1
sys.exit(1 if bad else 0)

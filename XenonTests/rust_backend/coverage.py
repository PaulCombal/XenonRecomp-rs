#!/usr/bin/env python3
"""Lists the cases of rust_backend.cpp that no test exercised."""
import re
import sys

source, cases_path = sys.argv[1:3]
ids = set(re.findall(r'case PPC_INST_(\w+):', open(source).read()))
seen = set()
for line in open(cases_path):
    name = re.match(r'\{ 0x[0-9A-F]+, \d, "([^\s"]+)', line).group(1)
    seen.add(name.rstrip('.').upper())
# Labels that share their code with the plain form the disassembler does produce.
shared = {'VUPKHSH128': 'VUPKHSH', 'VUPKLSH128': 'VUPKLSH'}
missing = sorted(i for i in ids if i not in seen and shared.get(i) not in seen)
print(f'{len(ids)} ids in the backend, {len(ids) - len(missing)} exercised; not exercised: {" ".join(missing)}')

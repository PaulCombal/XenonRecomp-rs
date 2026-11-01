#!/usr/bin/env python3
"""The whole title, one chunk at a time, C++ against Rust. See title_fuzz.cpp.

  title_fuzz.py <XenonRecomp> <title TOML> <C++ output directory> <work directory> [seeds] [jobs]

The C++ directory is what the C++ backend wrote for the same TOML.
"""
import collections
import concurrent.futures
import glob
import os
import re
import shutil
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
xenon, toml, ppc, work = map(os.path.abspath, sys.argv[1:5])
seeds = int(sys.argv[5]) if len(sys.argv) > 5 else 4
jobs = int(sys.argv[6]) if len(sys.argv) > 6 else 10
CXX = os.environ.get('CXX', 'clang++-18')
rs = f'{work}/rs'
os.makedirs(f'{work}/chunk', exist_ok=True)


def sh(command, **kw):
    subprocess.run(command, shell=True, check=True, **kw)


sh(f'"{xenon}" --backend=rust --out={rs} --rust-hooks={HERE}/fuzz_hooks.rs "{os.path.basename(toml)}" "{ROOT}/XenonUtils/ppc_context.h" | tail -1', cwd=os.path.dirname(toml))
sh(f'CARGO_TARGET_DIR={work}/target PPC_SKIP_CPP_FALLBACK=1 cargo build --release --offline --quiet -j{jobs}', cwd=rs)

INCLUDE = f'-I{rs}/cpp/include -I{ROOT}/thirdparty/simde'
open(f'{work}/decl.h', 'w').write('#include <cstdint>\nextern "C" void stub_indirect(uint32_t, struct PPCContext&);\n')
# -fwrapv: the C++ output overflows signed registers (mulld, add), undefined in
# C++, and clang uses that. Without it Rust, which wraps as the hardware does,
# reports differences that are the C++ build's.
FLAGS = f'-std=c++20 -O2 -march=x86-64-v2 -w {INCLUDE} ' + os.environ.get('FUZZ_CXXFLAGS', '-fwrapv')
sh(f'{CXX} {FLAGS} -c {HERE}/title_fuzz.cpp -o {work}/harness.o')

fallback = set(re.findall(r'PPC_FUNC_IMPL\(__imp__(\w+)\)', open(f'{rs}/cpp/ppc_fallback.cpp').read()))
chunks = sorted(int(os.path.basename(c)[1:]) for c in glob.glob(f'{rs}/chunks/c*'))


# EXPLAIN="sub_X:seed,..." prints what differs in those runs, and nothing else.
explain = [tuple(e.split(':')) for e in os.environ['EXPLAIN'].split(',')] if 'EXPLAIN' in os.environ else None
if explain is not None:
    names = {name for name, _ in explain}
    chunks = [n for n in chunks if names & set(re.findall(r'"\.weak (\w+)"', open(f'{rs}/chunks/c{n:03}/src/lib.rs').read()))]


def chunk(n):
    d = f'{work}/chunk/{n}'
    os.makedirs(d, exist_ok=True)
    source = open(f'{rs}/chunks/c{n:03}/src/lib.rs').read()
    defined = re.findall(r'"\.weak (\w+)"', source)
    externs = set(re.findall(r'^\tfn (\w+)\(ctx', source, re.M)) - set(defined)

    # The C++ chunk, without what the Rust side leaves to the fallback: those are
    # stubs for both, and tested by nothing here.
    cxx = open(f'{ppc}/ppc_recomp.{n}.cpp').read()
    parts = re.split(r'(?=^__attribute__\(\(alias\("__imp__)', cxx, flags=re.M)
    kept = [p for p in parts[1:] if re.search(r'alias\("__imp__(\w+)"', p).group(1) not in fallback]
    open(f'{d}/chunk.cpp', 'w').write(parts[0] + ''.join(kept))

    glue = ['#include "ppc_recomp_shared.h"',
            'extern "C" void stub_call(uint32_t, PPCContext&);',
            'struct Entry { const char* name; uint32_t id; PPCFunc* function; };']
    for name in sorted(externs):
        glue.append(f'extern "C" void {name}(PPCContext& ctx, uint8_t*) {{ stub_call({zlib.crc32(name.encode())}u, ctx); }}')
    for name in defined:
        glue.append(f'extern "C" PPC_FUNC(__imp__{name});')
    glue.append('Entry g_entries[] = {')
    for name in defined:
        glue.append(f'{{ "{name}", {zlib.crc32(name.encode())}u, __imp__{name} }},')
    glue.append('};')
    glue.append(f'size_t g_count = {len(defined)};')
    open(f'{d}/glue.cpp', 'w').write('\n'.join(glue))

    define = "'-DPPC_CALL_INDIRECT_FUNC(x)=stub_indirect(x,ctx)'"
    sh(f'{CXX} {FLAGS} -include {work}/decl.h {define} -c {d}/chunk.cpp -o {d}/chunk.o')
    sh(f'{CXX} -std=c++20 -O0 -w {INCLUDE} -c {d}/glue.cpp -o {d}/glue.o')
    sh(f'{CXX} {work}/harness.o {d}/glue.o {d}/chunk.o -o {d}/test_cpp -lm')
    sh(f'{CXX} {work}/harness.o {d}/glue.o {work}/target/release/libppc_recomp.a -o {d}/test_rust -lm -lpthread -ldl -lgcc_s')

    if explain is not None:
        import struct
        size = None
        for name, seed in explain:
            if name not in defined:
                continue
            files = {}
            for side in ('cpp', 'rust'):
                path = f'{d}/{side}.dump'
                subprocess.run([f'{d}/test_{side}', '1', name, seed], env=dict(os.environ, FUZZ_DUMP=path), capture_output=True)
                files[side] = open(path, 'rb').read()
            a, b = files['cpp'], files['rust']
            CTX = 2688
            print(f'{name} seed {seed}: trace {"same" if a[CTX:CTX+8] == b[CTX:CTX+8] else "DIFFERENT"}')
            for o in range(0, CTX, 8):
                if a[o:o+8] != b[o:o+8]:
                    print(f'   ctx+{o:#x}: {a[o:o+8][::-1].hex()} vs {b[o:o+8][::-1].hex()}')
            pages = lambda f: {f[i:i+8]: f[i+8:i+4104] for i in range(CTX + 8, len(f), 4104)}
            pa, pb = pages(a), pages(b)
            shown = 0
            for k in sorted(set(pa) | set(pb)):
                x, y = pa.get(k, bytes(4096)), pb.get(k, bytes(4096))
                for o in range(0, 4096, 4):
                    if x[o:o+4] != y[o:o+4] and shown < 8:
                        print(f'   mem {struct.unpack("<Q", k)[0] + o:#x}: {x[o:o+4].hex()} vs {y[o:o+4].hex()}')
                        shown += 1
        shutil.rmtree(d)
        return n, ([], [])

    results = []
    for side in ('cpp', 'rust'):
        output = subprocess.run([f'{d}/test_{side}', str(seeds)], capture_output=True, text=True, cwd=d)
        results.append(output.stdout.splitlines())
    shutil.rmtree(d)
    return n, results


count = collections.Counter()
bad = []
with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
    for n, (cpp, rust) in pool.map(chunk, chunks):
        if explain is not None:
            continue
        if len(cpp) != len(rust) or not cpp:
            count['BROKEN CHUNK'] += 1
            bad.append(f'chunk {n}: {len(cpp)} against {len(rust)} lines')
            continue
        for a, b in zip(cpp, rust):
            x, y = a.split(), b.split()
            kx, ky = int(x[2]), int(y[2])
            if kx != ky:
                if {kx, ky} & {14, 77}:
                    count['one side hit a limit'] += 1
                else:
                    count['DIFFERENT OUTCOME'] += 1
                    bad.append(f'{x[0]} seed {x[1]}: outcome {kx} against {ky}')
            elif kx in (14, 77):
                count['both hit a limit'] += 1
            elif kx != 0:
                count['same fault'] += 1
            elif x[3:] == y[3:]:
                count['identical'] += 1
            elif x[4] == y[4] and x[6] == y[6]:
                count['NaN sign or payload only'] += 1
            elif x[4] == y[4]:
                count['DIFFERENT MEMORY'] += 1
                bad.append(f'{x[0]} seed {x[1]}: memory')
            else:
                count['DIFFERENT REGISTERS'] += 1
                bad.append(f'{x[0]} seed {x[1]}: registers')
        print(f'chunk {n}: {dict(count)}', flush=True)

if explain is not None:
    sys.exit(0)
print('total:', dict(count))
for line in bad[:60]:
    print(' ', line)
sys.exit(1 if bad else 0)

#!/bin/bash
# The differential test of the Rust backend: see harness.cpp.
#   run.sh <XenonRecomp binary> <work directory> [seed] [inputs per case]
# Needs clang++ (CXX), cargo, and a checkout's simde and ppc_context.h.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
BIN=$1; WORK=$2; SEED=${3:-1}; INPUTS=${4:-64}
CXX=${CXX:-clang++-18}
mkdir -p "$WORK"

rustc --edition=2024 -O -Ctarget-cpu=x86-64-v2 "$HERE/union_locals.rs" -o "$WORK/union_locals"
"$WORK/union_locals"

"$BIN" --selftest="$WORK/gen" --seed="$SEED" "$ROOT/XenonUtils/ppc_context.h" > "$WORK/generate.log"
tail -2 "$WORK/generate.log"

FLAGS="-std=c++20 -O2 -fwrapv -march=x86-64-v2 -w -I$WORK/gen/cpp/include -I$WORK/gen -I$ROOT/thirdparty/simde"
$CXX $FLAGS -c "$WORK/gen/cxx/selftest.cpp" -o "$WORK/cxx.o"
$CXX $FLAGS -c "$HERE/harness.cpp" -o "$WORK/harness.o"
$CXX "$WORK/harness.o" "$WORK/cxx.o" -o "$WORK/test_cpp" -lm

(cd "$WORK/gen" && CARGO_TARGET_DIR="$WORK/target" PPC_SKIP_CPP_FALLBACK=1 cargo build --release --offline --quiet)
$CXX "$WORK/harness.o" "$WORK/target/release/libppc_recomp.a" -o "$WORK/test_rust" -lm -lpthread -ldl

"$WORK/test_cpp" "$WORK/cpp.txt" "$INPUTS" &
"$WORK/test_rust" "$WORK/rust.txt" "$INPUTS" &
wait
python3 "$HERE/coverage.py" "$ROOT/XenonRecomp/rust_backend.cpp" "$WORK/gen/cases.inc"
python3 "$HERE/compare.py" "$WORK/cpp.txt" "$WORK/rust.txt" "$WORK/gen/cases.inc"

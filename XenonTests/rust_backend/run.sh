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

# On arm64 the x86 intrinsics are neon.rs's (and simde's, for the C++ side), so
# that file's own test runs first, and neither compiler gets an x86 CPU.
case $(uname -m) in
    x86_64) RUST_CPU=-Ctarget-cpu=x86-64-v2; CXX_CPU=-march=x86-64-v2 ;;
    arm64|aarch64)
        RUST_CPU=; CXX_CPU=
        # The generated workspace's x86 rustflags are not for this host.
        export CARGO_BUILD_RUSTFLAGS=
        rustc --edition 2024 -O --test "$HERE/neon_semantics.rs" -o "$WORK/neon_semantics"
        "$WORK/neon_semantics" --quiet ;;
    *) echo "run.sh: x86-64 or arm64 only" >&2; exit 1 ;;
esac

rustc --edition=2024 -O $RUST_CPU "$HERE/union_locals.rs" -o "$WORK/union_locals"
"$WORK/union_locals"

"$BIN" --selftest="$WORK/gen" --seed="$SEED" "$ROOT/XenonUtils/ppc_context.h" > "$WORK/generate.log"
tail -2 "$WORK/generate.log"

# -D__restrict=: with ctx restrict, clang deletes the C++ backend's `ctx = env` after a
# setjmp as a copy of something to itself, and no register comes back.
FLAGS="-std=c++20 -O2 -fwrapv -D__restrict= -D__restrict__= $CXX_CPU -w -I$WORK/gen/cpp/include -I$WORK/gen -I$ROOT/thirdparty/simde"
$CXX $FLAGS -c "$WORK/gen/cxx/selftest.cpp" -o "$WORK/cxx.o"
$CXX $FLAGS -c "$HERE/harness.cpp" -o "$WORK/harness.o"
$CXX "$WORK/harness.o" "$WORK/cxx.o" -o "$WORK/test_cpp" -lm

(cd "$WORK/gen" && CARGO_TARGET_DIR="$WORK/target" PPC_SKIP_CPP_FALLBACK=1 cargo build --release --offline --quiet)
$CXX "$WORK/harness.o" "$WORK/target/release/libppc_recomp.a" -o "$WORK/test_rust" -lm -lpthread -ldl

timeout 900 "$WORK/test_cpp" "$WORK/cpp.txt" "$INPUTS" & cpp=$!
timeout 900 "$WORK/test_rust" "$WORK/rust.txt" "$INPUTS" & rust=$!
wait $cpp; wait $rust
python3 "$HERE/coverage.py" "$ROOT/XenonRecomp/rust_backend.cpp" "$WORK/gen/cases.inc"
python3 "$HERE/compare.py" "$WORK/cpp.txt" "$WORK/rust.txt" "$WORK/gen/cases.inc"

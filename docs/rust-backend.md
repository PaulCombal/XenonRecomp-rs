# The Rust backend

`XenonRecomp --backend=rust` writes the recompiled code as a Cargo workspace
instead of C++ files. It shares everything up to the printing with the C++
backend: the analysis, the switch tables and the register flow
(`register_flow.cpp`). The two agree on what each function does, and a function
of either can call a function of the other, because both use the same symbols,
the same `PPCContext` and the same calling convention.

    XenonRecomp --backend=rust --out=../ppc_rs --rust-hooks=mw2_hooks.rs MW2.toml XenonUtils/ppc_context.h
    cd ppc_rs && cargo build --release

Both options can also be set in the TOML, as `backend = "rust"` and
`rust_hooks_file_path`. `XenonUtils/rust/` must stay beside `ppc_context.h`, the
generator reads the runtime crate from there. The output is x86-64 only, and was
built and tested with Rust 1.97.

## What it writes

| path | what |
|---|---|
| `Cargo.toml`, `.cargo/config.toml` | the workspace: `panic = "abort"`, `-Ctarget-cpu=x86-64-v2` |
| `ppc_rt/` | the types and helpers, the counterpart of `ppc_context.h` (`XenonUtils/rust/ppc_rt.rs`), the title's constants and context (`context.rs`), and `hooks.rs` |
| `chunks/cNNN/` | 256 functions each, a crate each |
| `ppc_recomp/` | links the chunks as an rlib and a staticlib, and holds `PPCFuncMappings` |
| `cpp/` | `ppc_fallback.cpp`, the functions Rust cannot express, and the headers to build it |

One crate per chunk, because rustc scales worse than linearly with the size of a
crate, and chunks then build in parallel. They reach each other by symbol, so
there is no dependency cycle. A whole title takes about 30 seconds on 16 threads.

If your own workspace builds these crates, it has to set the same two things: the
target CPU (`ppc_rt` stops the build with a message otherwise, because without
SSSE3 and SSE4.1 every vector instruction would be a function call), and
`panic = "abort"`.

## Using it from a runtime

From Rust, depend on `ppc_recomp`. If `cpp/ppc_fallback.cpp` has any functions, its
`build.rs` compiles them with `clang++` (`CXX`, `AR`, `PPC_SIMDE_INCLUDE`). A runtime
that compiles that file itself sets `PPC_SKIP_CPP_FALLBACK`.

From C++, link `libppc_recomp.a` in place of the compiled `ppc_recomp.*.cpp` and
`ppc_func_mapping.cpp`, and nothing else changes. Everything is `extern "C"` and
the runtime supplies the imports (`__imp__Xam...`) and `PPC_QueryTimebase` as
before. Rust's static library needs `gcc_s`, `dl`, `m` and `pthread` on Linux.

### Hooks

`__imp__sub_X` is the recompiled function and `sub_X` a weak alias of it, as in
C++, and every call goes through `sub_X`. So a strong `sub_X` anywhere in the
program, in C++ or in Rust with `#[unsafe(no_mangle)]`, replaces the function, and
can still call `__imp__sub_X`. Rust has no stable weak linkage, but its assembler
does: each chunk has a `global_asm!` of `.weak` and `.set` lines.

A hook written in Rust must not panic across guest frames, and may be skipped by a
`longjmp`, so nothing in it can rely on a destructor while it calls guest code. That
means no `Vec`, `Box` or lock guard held across a call into the guest.

### What C++ does with macros

`PPC_MM_STORE_*` and `PPC_CALL_INDIRECT_FUNC` are functions in `hooks.rs`.
`rust_hooks_file_path` replaces that file and must define all of them.

## Control flow

Rust has no goto. `rust_cfg.cpp` rebuilds each function from its basic blocks:

- A reducible graph goes directly into labeled blocks and loops, after Ramsey,
  "Beyond Relooper" (ICFP 2022). Each block is placed once, inside the block that
  dominates it; a back edge is `continue 'l`, an edge to a join is `break 'b`, and
  an edge to a block with no other way in places it there. There is no state
  variable and no overhead: the machine code comes out the size of the C++.
- A loop with several entries has no header to start a `loop` at. It gets a
  dispatcher, a `match` on a `__state` local that the edges into the loop set, and
  the rest of the function stays as it is. This is what LLVM's FixIrreducible does.
  MW2 has 12 such regions in the campaign, on 84 edges, counting the ones setjmp makes.

There is no case this leaves over, and no assembly is involved: `asm!` could not
help anyway, since there is no x86 to wrap, and its jumps cannot reach between
Rust blocks.

## setjmp and longjmp

A function that calls `setjmp` returns twice, a thing rustc cannot be told, and a
Rust function that does it anyway is miscompiled. So in the Rust output nothing
returns twice. 12 functions of the MW2 campaign call it, one of them 9,018 lines,
and 5 more only call `longjmp`.

The generator cuts such a function into a body that can start after any of its
`setjmp`s, by the same dispatcher as a multi-entry loop. At a `setjmp` the body
saves the context in `env`, as the C++ does, and returns. A small assembly
trampoline in `ppc_rt`, `ppc_setjmp_call`, saves the callee-saved registers on the
stack, leaves its stack pointer in the guest's `jmp_buf`, and calls the body at the
point after that `setjmp`. `ppc_longjmp` puts the stack pointer back and makes the
same call with the value, the body first getting `env` back. To the compiler those
are two ordinary calls. It is also why only 8 bytes of a `jmp_buf` are used.

These functions keep all their registers in the context (`register_flow.cpp`),
so nothing they need lives in a frame that went away. `longjmp` drops frames, which
is defined as long as they have no destructors: generated code has none, all its
locals being `Copy`, and the release profile has no landing pads.

## What goes through C++

`cpp/ppc_fallback.cpp` is the C++ backend's output for a function with a mid-asm
hook, or a `setjmp` reached other than by a plain `bl` or in a function that keeps
registers in locals. MW2 has none: the file is empty, and neither title needs a C++
compiler for guest code.

## What is different from the C++ output

Rust is stricter in three places, and in each the difference is only visible to
input a real guest does not produce. The C++ result is the one that is off:

- `lwarx`, `stwcx.` and friends add `base + rA + rB` in C++, which does not wrap at
  4 GB. In Rust an address is always a `u32`.
- `lwz r3,-4(0)` is below `base` in C++, near the top of the 4 GB in Rust.
- `vpkd3d128` overflows a signed int in C++, undefined, far from the 3.0 its input
  normally is. Rust wraps.

C++ restores the context after a `longjmp` with `ctx = env`, which is a copy of
`ctx` to itself as far as `__restrict` is concerned, and clang can delete it. It did
in the test programs of `run.sh`, which build with `-D__restrict=` for that reason.
It did not in the MW2 function checked, `sub_82283C10`; the others were not.

A branch that decrements the counter and leaves the function (`bdz` to a tail call)
does not compile in C++ and does in Rust.

The C++ output overflows signed registers, in `mulld` and `add` among others, which
is undefined in C++, and clang acts on it: MW2's `sub_82136128` loses bit 63 of a
`mulld` in a build without `-fwrapv`. Rust wraps, as the console does. A C++ build
should use `-fwrapv`.

A NaN's sign and payload can differ, since x86 takes them from the first operand
and the compilers are free to commute one. On the console's hardware such a value
is not defined either.

## The rustc bug

rustc 1.97 miscompiles this, which is valid Rust:

```rust
let mut r = R::ZERO;               // a named const of a union type
r.u64 = x;
r.u64 = r.u64.wrapping_sub(1);
if r.u32 == 0 { 7 } else { 9 }     // always 7
```

A MIR pass folds the read of `.u32` from the constant, in spite of the writes to its
sibling `.u64`. It made a branch of MW2's script compiler unconditional, and the
level failed to load. `PPCRegister { u64: 0 }` is not affected, so that is how
generated code initialises its locals, and `ppc_rt` has no such constant.
`XenonTests/rust_backend/union_locals.rs` covers the union patterns the generator
uses, and says when the named constant stops being miscompiled.

## Things to know

- All generated code is `unsafe`: it is a machine's instructions on a raw 4 GB
  memory. The backend gives you a single toolchain and Rust hooks, not memory safety
  in guest code.
- Guest memory goes through volatile accesses, as in C++, because other threads and
  the GPU watch it. Formally a data race, as it is in C++: this is the same
  contract as the Linux kernel's, resting on the compiler not tearing a volatile
  aligned access. Unaligned loads are defined here (a packed wrapper). A release
  build's unaligned stores use `write_volatile`, which compiles to exactly the
  store C++ makes; a debug build, whose checks would abort there, uses inline
  assembly, which release does not because it costs 2-7% of the code.
- MXCSR is changed with inline assembly, and Rust assumes the default floating
  point environment. So does clang without `-frounding-math`, and the same
  constant-folding caveat applies to both.
- Cross-language LTO needs rustc and clang on the same LLVM. ThinLTO among the Rust
  crates alone builds, and changes little: calls go through weak symbols, which
  stops inlining across functions. PGO has not been tried on the guest code.
- Windows is `x86_64-pc-windows-gnullvm`, linked with llvm-mingw. A chunk compiles
  for it, and weak hooks and the setjmp trampoline work there (run under Wine); no
  whole game has been built.

## Testing

`XenonTests/rust_backend/`:

- `union_locals.rs`, for the bug above.
- `run.sh`: `XenonRecomp --selftest` writes a synthetic image through both backends,
  with every form of every handled instruction with random operands, and random
  control flow graphs with jump tables and multi-entry loops, and the two programs
  run on hostile inputs (NaNs, denormals, every rounding and flush mode, stores that
  fault), and `setjmp` programs: there and back, from a callee, around a loop, with
  two buffers. `coverage.py` lists any case of `rust_backend.cpp` that was not reached.
  Atomics run on one thread here: what they do under contention is the game's test.
- `title_fuzz.py`: every function of a real title, one chunk at a time, in the
  configuration it really has. Registers in locals is what `run.sh` cannot see, as
  it keeps everything in the context to compare it.

For a title that misbehaves, the two backends can be mixed in one program: link the
C++ objects of some chunks before `libppc_recomp.a`, with
`-Wl,--allow-multiple-definition`. Bisecting on that finds the function in about
twenty runs. It is how the bug above was found.

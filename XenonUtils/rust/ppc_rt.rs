// Everything recompiled Rust code needs, the counterpart of ppc_context.h. The
// generator copies this file into the crate it writes (ppc_rt/src/lib.rs),
// beside context.rs, which it generates, and hooks.rs, which a title may
// replace (rust_hooks_file in its TOML).
//
// The types are laid out as in ppc_context.h, so a PPCContext can be shared
// with C++: a C++ runtime and Rust hooks see the same fields at the same
// offsets, which context.rs and the C++ fallback file both assert.
//
// What sits next to each helper is the C++ it matches, where the semantics are
// not obvious. tests/rust_backend runs both against each other.

#![no_std]
#![allow(non_snake_case, non_camel_case_types, non_upper_case_globals)]
#![allow(unsafe_op_in_unsafe_fn, clippy::all)]
// __m128i is a NEON register on arm64, which the lint does not know how to pass.
#![allow(improper_ctypes)]

#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
compile_error!("the Rust backend targets x86-64 and AArch64 only");

// Without these the intrinsics below are real calls, one per guest vector
// instruction, and SSE4.1 ones may not link at all.
#[cfg(all(target_arch = "x86_64", not(all(target_feature = "ssse3", target_feature = "sse4.1"))))]
compile_error!("recompiled code needs SSSE3 and SSE4.1: build with -Ctarget-cpu=x86-64-v2 (the generated .cargo/config.toml says so, for builds from that workspace)");

use core::arch::asm;
#[cfg(target_arch = "x86_64")]
pub use core::arch::x86_64::*;
// On arm64 the x86 intrinsics the recompiled code names are neon.rs's, in NEON.
#[cfg(target_arch = "aarch64")]
mod neon;
#[cfg(target_arch = "aarch64")]
pub use neon::*;
#[cfg(target_arch = "aarch64")]
use core::arch::aarch64::{vcvtd_s64_f64, vdup_n_f64, vget_lane_f64, vrndi_f64, vsqrt_f64};
use core::hint::cold_path;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// ---- registers ---------------------------------------------------------------

#[repr(C)]
#[derive(Copy, Clone)]
pub union PPCRegister {
    pub s8: i8,
    pub u8: u8,
    pub s16: i16,
    pub u16: u16,
    pub s32: i32,
    pub u32: u32,
    pub s64: i64,
    pub u64: u64,
    pub f32: f32,
    pub f64: f64,
}

// No `const ZERO` here, nor on PPCVRegister. rustc 1.97 miscompiles a union local
// initialised from a named constant: after `let mut r = ZERO; r.u64 = x;` a read
// of r.u32 can fold to the constant's value, where no write to r.u32 happened. It
// took a branch out of MW2's script compiler. Generated code initialises locals
// with `PPCRegister { u64: 0 }`, which is not affected, and
// XenonTests/rust_backend/union_locals.rs keeps watch.

#[repr(C)]
#[derive(Copy, Clone)]
pub struct PPCXERRegister {
    pub so: u8,
    pub ov: u8,
    pub ca: u8,
}

impl PPCXERRegister {
    pub const ZERO: Self = Self { so: 0, ov: 0, ca: 0 };
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct PPCCRRegister {
    pub lt: u8,
    pub gt: u8,
    pub eq: u8,
    pub so: u8,
}

impl PPCCRRegister {
    pub const ZERO: Self = Self { lt: 0, gt: 0, eq: 0, so: 0 };

    #[inline(always)]
    pub fn cmp<T: PartialOrd + Copy>(&mut self, left: T, right: T, so: u8) {
        self.lt = (left < right) as u8;
        self.gt = (left > right) as u8;
        self.eq = (left == right) as u8;
        self.so = so;
    }

    #[inline(always)]
    pub fn cmpf(&mut self, left: f64, right: f64) {
        let un = left.is_nan() || right.is_nan();
        self.so = un as u8;
        self.lt = (!un && left < right) as u8;
        self.gt = (!un && left > right) as u8;
        self.eq = (!un && left == right) as u8;
    }

    #[inline(always)]
    pub unsafe fn setFromMaskPs(&mut self, mask: __m128, imm: i32) {
        let m = _mm_movemask_ps(mask);
        self.lt = (m == imm) as u8;
        self.gt = 0;
        self.eq = (m == 0) as u8;
        self.so = 0;
    }

    #[inline(always)]
    pub unsafe fn setFromMaskSi(&mut self, mask: __m128i, imm: i32) {
        let m = _mm_movemask_epi8(mask);
        self.lt = (m == imm) as u8;
        self.gt = 0;
        self.eq = (m == 0) as u8;
        self.so = 0;
    }
}

#[repr(C, align(16))]
#[derive(Copy, Clone)]
pub union PPCVRegister {
    pub s8: [i8; 16],
    pub u8: [u8; 16],
    pub s16: [i16; 8],
    pub u16: [u16; 8],
    pub s32: [i32; 4],
    pub u32: [u32; 4],
    pub s64: [i64; 2],
    pub u64: [u64; 2],
    pub f32: [f32; 4],
    pub f64: [f64; 2],
    // The same 128 bits as the intrinsics take them.
    pub i: __m128i,
    pub ps: __m128,
}

// ---- floating point state ----------------------------------------------------

#[cfg(target_arch = "x86_64")]
const ROUND_MASK: u32 = 0x6000;
#[cfg(target_arch = "x86_64")]
const ROUND_SHIFT: u32 = 13;
#[cfg(target_arch = "x86_64")]
const FLUSH_MASK: u32 = 0x8000 | 0x0040; // FTZ and DAZ
#[cfg(target_arch = "x86_64")]
const GUEST_TO_HOST: [u32; 4] = [0x0000, 0x6000, 0x4000, 0x2000];
#[cfg(target_arch = "x86_64")]
const HOST_TO_GUEST: [u32; 4] = [0, 3, 2, 1];

// On arm64 the same state is FPCR: RMode in bits 23:22 (0 nearest, 1 up, 2 down,
// 3 to zero, where the guest's rounding field is 0 nearest, 1 to zero, 2 up, 3 down)
// and FZ in bit 24, which flushes denormal inputs and results both.
#[cfg(target_arch = "aarch64")]
const ROUND_MASK: u32 = 3 << 22;
#[cfg(target_arch = "aarch64")]
const ROUND_SHIFT: u32 = 22;
#[cfg(target_arch = "aarch64")]
const FLUSH_MASK: u32 = 1 << 24;
#[cfg(target_arch = "aarch64")]
const GUEST_TO_HOST: [u32; 4] = [0, 3 << 22, 1 << 22, 2 << 22];
#[cfg(target_arch = "aarch64")]
const HOST_TO_GUEST: [u32; 4] = [0, 2, 3, 1];

// _mm_getcsr and _mm_setcsr are deprecated in Rust. These are the same two
// instructions. Like clang's, nothing here stops an optimiser from moving pure
// arithmetic across them; the emitted order is what holds in practice, and
// ppc_context.h relies on the same.
#[cfg(target_arch = "x86_64")]
#[inline(always)]
unsafe fn getcsr() -> u32 {
    let mut csr: u32 = 0;
    asm!("stmxcsr [{}]", in(reg) &raw mut csr, options(nostack, preserves_flags));
    csr
}

#[cfg(target_arch = "x86_64")]
#[inline(always)]
unsafe fn setcsr(csr: u32) {
    asm!("ldmxcsr [{}]", in(reg) &raw const csr, options(nostack, preserves_flags, readonly));
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn getcsr() -> u32 {
    let fpcr: u64;
    asm!("mrs {}, fpcr", out(reg) fpcr, options(nostack, preserves_flags, nomem));
    fpcr as u32
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn setcsr(csr: u32) {
    asm!("msr fpcr, {}", in(reg) csr as u64, options(nostack, preserves_flags, nomem));
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct PPCFPSCRRegister {
    pub csr: u32,
}

impl PPCFPSCRRegister {
    #[inline(always)]
    pub unsafe fn loadFromHost(&mut self) -> u32 {
        self.csr = getcsr();
        HOST_TO_GUEST[((self.csr & ROUND_MASK) >> ROUND_SHIFT) as usize]
    }

    #[inline(always)]
    pub unsafe fn storeFromGuest(&mut self, value: u32) {
        self.csr &= !ROUND_MASK;
        self.csr |= GUEST_TO_HOST[(value & 3) as usize];
        setcsr(self.csr);
    }

    #[inline(always)]
    pub unsafe fn enableFlushModeUnconditional(&mut self) {
        self.csr |= FLUSH_MASK;
        setcsr(self.csr);
    }

    #[inline(always)]
    pub unsafe fn disableFlushModeUnconditional(&mut self) {
        self.csr &= !FLUSH_MASK;
        setcsr(self.csr);
    }

    #[inline(always)]
    pub unsafe fn enableFlushMode(&mut self) {
        if (self.csr & FLUSH_MASK) != FLUSH_MASK {
            cold_path();
            self.csr |= FLUSH_MASK;
            setcsr(self.csr);
        }
    }

    #[inline(always)]
    pub unsafe fn disableFlushMode(&mut self) {
        if (self.csr & FLUSH_MASK) != 0 {
            cold_path();
            self.csr &= !FLUSH_MASK;
            setcsr(self.csr);
        }
    }
}

include!("context.rs");
include!("hooks.rs");

pub type PPCFunc = unsafe extern "C" fn(ctx: &mut PPCContext, base: *mut u8);

#[repr(C)]
pub struct PPCFuncMapping {
    pub guest: usize,
    pub host: Option<PPCFunc>,
}

unsafe extern "C" {
    pub fn PPC_QueryTimebase() -> u64;
    pub fn exp2f(x: f32) -> f32;
    pub fn log2f(x: f32) -> f32;
}

/// PPC_LOOKUP_FUNC. The table sits after the image, one slot per halfword of code.
#[inline(always)]
pub unsafe fn ppc_lookup_func(base: *mut u8, address: u32) -> Option<PPCFunc> {
    let offset = PPC_IMAGE_BASE + PPC_IMAGE_SIZE + (address.wrapping_sub(PPC_CODE_BASE as u32) as u64) * 2;
    *(base.add(offset as usize) as *const Option<PPCFunc>)
}

#[inline(always)]
pub unsafe fn ppc_debugtrap() {
    #[cfg(target_arch = "x86_64")]
    asm!("int3", options(nomem, nostack));
    #[cfg(target_arch = "aarch64")]
    asm!("brk #0xf000", options(nomem, nostack));
}

// ---- the console's memory barriers ---------------------------------------------
//
// sync, lwsync and eieio. x86 keeps loads and stores in the order the program gave
// them as closely as the title needs, so there they are nothing; arm64 does not,
// and the title's locks, ring buffers and flags are written for a machine that
// orders only where it is told to. (The guest's own memory accesses are volatile,
// which the compiler leaves in order, and these are what orders them in the
// processor.)

#[inline(always)]
pub fn ppc_sync() {
    #[cfg(target_arch = "aarch64")]
    core::sync::atomic::fence(Ordering::SeqCst);
}

#[inline(always)]
pub fn ppc_lwsync() {
    #[cfg(target_arch = "aarch64")]
    core::sync::atomic::fence(Ordering::AcqRel);
}

#[inline(always)]
pub fn ppc_eieio() {
    #[cfg(target_arch = "aarch64")]
    core::sync::atomic::fence(Ordering::Release);
}

// ---- setjmp and longjmp ------------------------------------------------------
//
// A function that calls setjmp returns twice, which rustc cannot be told, and a
// Rust function that does so anyway is miscompiled. So nothing here returns twice.
// The generator cuts such a function at each setjmp into a body that can start at
// any of them, the way a multi-entry loop is handled, and ppc_setjmp_call calls
// that body. It leaves its stack pointer in the guest's jmp_buf, and ppc_longjmp
// goes back to it and makes the call again, with the value. To the compiler those
// are two ordinary calls of a function. The frames longjmp drops have no
// destructors, as ever. All of the guest's registers are in the context in these
// functions (register_flow.cpp), and env holds them as they were, as in C++.

#[repr(C)]
pub struct SetjmpFrame {
    ctx: *mut PPCContext,
    base: *mut u8,
    env: *mut PPCContext,
    body: unsafe extern "C" fn(&mut PPCContext, *mut u8, *mut SetjmpFrame),
    pub resume: u32, // where the body starts: 0, or the setjmp it continues after
    site: u32,       // the setjmp the body just stopped at, 0 when it finished
    buffer: u32,     // that setjmp's jmp_buf
}

// One setjmp in force, which outlives the calls of Resume it makes.
#[repr(C)]
struct SetjmpActivation {
    frame: *mut SetjmpFrame,
    site: u32,
    buffer: u32,
}

unsafe extern "C" {
    fn ppc_setjmp_call(buffer: *mut u8, function: unsafe extern "C" fn(*mut SetjmpActivation, i32), argument: *mut SetjmpActivation);
    pub fn ppc_longjmp(buffer: *mut u8, value: i32) -> !;
}

#[cfg(all(target_arch = "x86_64", not(windows)))]
core::arch::global_asm!(
    ".text",
    ".globl ppc_setjmp_call",
    "ppc_setjmp_call:",
    "push rbp", "push rbx", "push r12", "push r13", "push r14", "push r15",
    "push rdx", // [rsp+16] the argument
    "push rsi", // [rsp+8] the function
    "sub rsp, 8",
    "mov [rdi], rsp",
    "mov rdi, rdx",
    "xor esi, esi",
    "call [rsp+8]",
    "2:",
    "add rsp, 24",
    "pop r15", "pop r14", "pop r13", "pop r12", "pop rbx", "pop rbp",
    "ret",
    ".globl ppc_longjmp",
    "ppc_longjmp:",
    "mov rsp, [rdi]",
    "mov eax, 1",
    "test esi, esi",
    "cmove esi, eax", // longjmp(buffer, 0) returns 1
    "mov rdi, [rsp+16]",
    "call [rsp+8]",
    "jmp 2b",
);

#[cfg(all(target_arch = "x86_64", windows))]
core::arch::global_asm!(
    ".text",
    ".globl ppc_setjmp_call",
    "ppc_setjmp_call:",
    "push rbp", "push rbx", "push rdi", "push rsi", "push r12", "push r13", "push r14", "push r15",
    "push r8",  // [rsp+208] the argument
    "push rdx", // [rsp+200] the function
    "sub rsp, 200", // the shadow space, xmm6-15, and alignment
    "movups [rsp+32], xmm6", "movups [rsp+48], xmm7", "movups [rsp+64], xmm8", "movups [rsp+80], xmm9",
    "movups [rsp+96], xmm10", "movups [rsp+112], xmm11", "movups [rsp+128], xmm12", "movups [rsp+144], xmm13",
    "movups [rsp+160], xmm14", "movups [rsp+176], xmm15",
    "mov [rcx], rsp",
    "mov rcx, r8",
    "xor edx, edx",
    "call [rsp+200]",
    "2:",
    "movups xmm6, [rsp+32]", "movups xmm7, [rsp+48]", "movups xmm8, [rsp+64]", "movups xmm9, [rsp+80]",
    "movups xmm10, [rsp+96]", "movups xmm11, [rsp+112]", "movups xmm12, [rsp+128]", "movups xmm13, [rsp+144]",
    "movups xmm14, [rsp+160]", "movups xmm15, [rsp+176]",
    "add rsp, 216",
    "pop r15", "pop r14", "pop r13", "pop r12", "pop rsi", "pop rdi", "pop rbx", "pop rbp",
    "ret",
    ".globl ppc_longjmp",
    "ppc_longjmp:",
    "mov rsp, [rcx]",
    "mov eax, 1",
    "test edx, edx",
    "cmove edx, eax",
    "mov rcx, [rsp+208]",
    "call [rsp+200]",
    "jmp 2b",
);


// The same on arm64: the callee-saved registers (x19-x28, the frame pointer and link
// register, d8-d15) saved on the stack, its pointer left in the jmp_buf, and the
// function called with the argument and 0. x18 is the platform's.
#[cfg(target_arch = "aarch64")]
macro_rules! arm64_setjmp {
    ($call:literal, $long:literal) => {
        core::arch::global_asm!(
            ".text",
            ".p2align 2",
            concat!(".globl ", $call),
            concat!($call, ":"),
            "stp x29, x30, [sp, #-16]!",
            "stp x27, x28, [sp, #-16]!",
            "stp x25, x26, [sp, #-16]!",
            "stp x23, x24, [sp, #-16]!",
            "stp x21, x22, [sp, #-16]!",
            "stp x19, x20, [sp, #-16]!",
            "stp d14, d15, [sp, #-16]!",
            "stp d12, d13, [sp, #-16]!",
            "stp d10, d11, [sp, #-16]!",
            "stp d8, d9, [sp, #-16]!",
            "stp x2, x1, [sp, #-16]!", // [sp] the argument, [sp+8] the function
            "mov x3, sp",
            "str x3, [x0]",
            "mov x0, x2",
            "mov w2, wzr",
            "mov w1, wzr",
            "ldr x3, [sp, #8]",
            "blr x3",
            "2:",
            "add sp, sp, #16",
            "ldp d8, d9, [sp], #16",
            "ldp d10, d11, [sp], #16",
            "ldp d12, d13, [sp], #16",
            "ldp d14, d15, [sp], #16",
            "ldp x19, x20, [sp], #16",
            "ldp x21, x22, [sp], #16",
            "ldp x23, x24, [sp], #16",
            "ldp x25, x26, [sp], #16",
            "ldp x27, x28, [sp], #16",
            "ldp x29, x30, [sp], #16",
            "ret",
            ".p2align 2",
            concat!(".globl ", $long),
            concat!($long, ":"),
            "ldr x2, [x0]",
            "mov sp, x2",
            "mov w3, #1",
            "cmp w1, #0",
            "csel w1, w3, w1, eq", // longjmp(buffer, 0) returns 1
            "ldr x0, [sp]",
            "ldr x3, [sp, #8]",
            "blr x3",
            "b 2b",
        );
    };
}

#[cfg(all(target_arch = "aarch64", target_vendor = "apple"))]
arm64_setjmp!("_ppc_setjmp_call", "_ppc_longjmp");

#[cfg(all(target_arch = "aarch64", not(target_vendor = "apple")))]
arm64_setjmp!("ppc_setjmp_call", "ppc_longjmp");

/// `*env = *ctx`, then stops the body at this setjmp. The body returns next.
#[inline(always)]
pub unsafe fn ppc_setjmp_site(ctx: &mut PPCContext, frame: *mut SetjmpFrame, site: u32) {
    core::ptr::copy_nonoverlapping(ctx as *const PPCContext, (*frame).env, 1);
    (*frame).site = site;
    (*frame).buffer = ctx.r3.u32;
}

unsafe extern "C" fn setjmp_resume(activation: *mut SetjmpActivation, value: i32) {
    let frame = (*activation).frame;
    if value != 0 {
        core::ptr::copy_nonoverlapping((*frame).env, (*frame).ctx, 1);
    }
    (*(*frame).ctx).r3.s64 = value as i64;

    (*frame).resume = (*activation).site;
    (*frame).site = 0;
    ((*frame).body)(&mut *(*frame).ctx, (*frame).base, frame);

    // A setjmp on another buffer goes inside this one, which stays in force. On
    // the same buffer it replaces this one, which therefore ends here: the stack
    // does not grow with each turn of a loop around setjmp.
    while (*frame).site != 0 && (*frame).buffer != (*activation).buffer {
        setjmp_call(frame);
    }
}

unsafe fn setjmp_call(frame: *mut SetjmpFrame) {
    let mut activation = SetjmpActivation { frame, site: (*frame).site, buffer: (*frame).buffer };
    (*frame).site = 0;
    ppc_setjmp_call((*frame).base.add(activation.buffer as usize), setjmp_resume, &raw mut activation);
}

/// The whole of a function that calls setjmp.
#[inline(never)]
pub unsafe fn ppc_run_setjmp_function(
    ctx: &mut PPCContext,
    base: *mut u8,
    body: unsafe extern "C" fn(&mut PPCContext, *mut u8, *mut SetjmpFrame),
) {
    let mut env = core::mem::MaybeUninit::<PPCContext>::uninit();
    let mut frame = SetjmpFrame { ctx: ctx as *mut PPCContext, base, env: env.as_mut_ptr(), body, resume: 0, site: 0, buffer: 0 };
    let frame = &raw mut frame;
    body(&mut *(*frame).ctx, base, frame);
    while (*frame).site != 0 {
        setjmp_call(frame);
    }
}

// ---- guest memory ------------------------------------------------------------
//
// Volatile, as in ppc_context.h, and for the same reason: other guest threads
// and the GPU watch this memory, and lwsync is a no-op because x86's ordering
// is already stronger, as long as the compiler moves and merges nothing.
//
// A guest may access memory unaligned, which read_volatile and write_volatile on
// a u32 forbid. Through a packed wrapper a load is a defined `load volatile, align
// 1`. A store through one is not usable, because rustc copies the value through
// the stack first, and inline assembly is defined but costs 2-7% of the code: it
// counts as touching all memory, so the fields of the context are reloaded around
// every guest store. A release build therefore stores with write_volatile, which
// is the store ppc_context.h compiles to, `align` claim included: x86 does not
// care and LLVM derives nothing from it. A debug build, whose checks would abort
// on the misaligned pointer, takes the assembly.

#[repr(C, packed)]
#[derive(Copy, Clone)]
struct Unaligned<T>(T);

#[inline(always)]
unsafe fn guest(base: *mut u8, address: u32) -> *mut u8 {
    base.add(address as usize)
}

#[inline(always)]
pub unsafe fn ld8(base: *mut u8, address: u32) -> u8 {
    (guest(base, address) as *const u8).read_volatile()
}

#[inline(always)]
pub unsafe fn ld16(base: *mut u8, address: u32) -> u16 {
    (guest(base, address) as *const Unaligned<u16>).read_volatile().0.swap_bytes()
}

#[inline(always)]
pub unsafe fn ld32(base: *mut u8, address: u32) -> u32 {
    (guest(base, address) as *const Unaligned<u32>).read_volatile().0.swap_bytes()
}

#[inline(always)]
pub unsafe fn ld64(base: *mut u8, address: u32) -> u64 {
    (guest(base, address) as *const Unaligned<u64>).read_volatile().0.swap_bytes()
}

#[inline(always)]
pub unsafe fn st8(base: *mut u8, address: u32, value: u8) {
    (guest(base, address)).write_volatile(value);
}

macro_rules! guest_store {
    ($name:ident, $t:ty, $size:literal, $register:literal) => {
        #[inline(always)]
        pub unsafe fn $name(base: *mut u8, address: u32, value: $t) {
            #[cfg(not(debug_assertions))]
            (guest(base, address) as *mut $t).write_volatile(value.swap_bytes());
            // arm64 stores unaligned as it loads, so the wrapper that makes the write
            // defined costs nothing there.
            #[cfg(all(debug_assertions, target_arch = "aarch64"))]
            (guest(base, address) as *mut Unaligned<$t>).write_volatile(Unaligned(value.swap_bytes()));
            #[cfg(all(debug_assertions, target_arch = "x86_64"))]
            asm!(
                concat!("mov ", $size, " ptr [{}+{}], {:", $register, "}"),
                in(reg) base,
                in(reg) address as u64,
                in(reg) value.swap_bytes(),
                options(nostack, preserves_flags),
            );
        }
    };
}
guest_store!(st16, u16, "word", "x");
guest_store!(st32, u32, "dword", "e");
guest_store!(st64, u64, "qword", "r");

// lwarx and ldarx read without a swap, for stwcx. and stdcx. to compare with
// what memory holds. The reservation is on an aligned word, as on the console.
#[inline(always)]
pub unsafe fn lwarx(base: *mut u8, address: u32) -> u32 {
    AtomicU32::from_ptr(guest(base, address) as *mut u32).load(Ordering::Relaxed)
}

#[inline(always)]
pub unsafe fn ldarx(base: *mut u8, address: u32) -> u64 {
    AtomicU64::from_ptr(guest(base, address) as *mut u64).load(Ordering::Relaxed)
}

#[inline(always)]
pub unsafe fn stwcx(base: *mut u8, address: u32, old: u32, new: u32) -> bool {
    AtomicU32::from_ptr(guest(base, address) as *mut u32)
        .compare_exchange(old, new, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok()
}

#[inline(always)]
pub unsafe fn stdcx(base: *mut u8, address: u32, old: u64, new: u64) -> bool {
    AtomicU64::from_ptr(guest(base, address) as *mut u64)
        .compare_exchange(old, new, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok()
}

#[inline(always)]
pub unsafe fn dcbz(base: *mut u8, address: u32) {
    guest(base, address & !31).write_bytes(0, 32);
}

#[inline(always)]
pub unsafe fn dcbzl(base: *mut u8, address: u32) {
    guest(base, address & !127).write_bytes(0, 128);
}

// ---- integer helpers ---------------------------------------------------------

/// (a + b < a) | (a + b + c < c), the carry out of a three-term 32-bit add.
#[inline(always)]
pub fn carry3(a: u32, b: u32, c: u8) -> u8 {
    let sum = a.wrapping_add(b);
    ((sum < a) as u8) | ((sum.wrapping_add(c as u32) < c as u32) as u8)
}

/// v ? mask : 0, one bit of mfcr.
#[inline(always)]
pub fn crbit(v: u8, mask: u64) -> u64 {
    if v != 0 { mask } else { 0 }
}

macro_rules! divide {
    ($name:ident, $t:ty) => {
        /// PPCDivide: a divide that gives 0 where the host's would trap.
        #[inline(always)]
        pub fn $name(a: $t, b: $t) -> $t {
            match a.checked_div(b) {
                Some(q) => q,
                None => 0,
            }
        }
    };
}
divide!(ppc_divide_s32, i32);
divide!(ppc_divide_u32, u32);
divide!(ppc_divide_s64, i64);
divide!(ppc_divide_u64, u64);

#[inline(always)]
pub fn sat_s32(v: i64) -> i32 {
    if v > i32::MAX as i64 {
        i32::MAX
    } else if v < i32::MIN as i64 {
        i32::MIN
    } else {
        v as i32
    }
}

// ---- floating point helpers --------------------------------------------------

#[cfg(target_arch = "x86_64")]
#[inline(always)]
pub unsafe fn fsqrt(x: f64) -> f64 {
    let v = _mm_set_sd(x);
    _mm_cvtsd_f64(_mm_sqrt_sd(v, v))
}

#[cfg(target_arch = "x86_64")]
#[inline(always)]
pub unsafe fn fctid(x: f64) -> i64 {
    if x > i64::MAX as f64 { i64::MAX } else { _mm_cvtsd_si64(_mm_set_sd(x)) }
}

#[cfg(target_arch = "x86_64")]
#[inline(always)]
pub unsafe fn fctidz(x: f64) -> i64 {
    if x > i64::MAX as f64 { i64::MAX } else { _mm_cvttsd_si64(_mm_set_sd(x)) }
}

#[cfg(target_arch = "x86_64")]
#[inline(always)]
pub unsafe fn fctiwz(x: f64) -> i64 {
    if x > i32::MAX as f64 { i32::MAX as i64 } else { _mm_cvttsd_si32(_mm_set_sd(x)) as i64 }
}

// On arm64 the conversions saturate and give 0 for a NaN, where cvtsd2si gives the
// integer indefinite (the type's minimum) for a NaN and for a value too far below;
// the guest expects x86's.
#[cfg(target_arch = "aarch64")]
#[inline(always)]
pub unsafe fn fsqrt(x: f64) -> f64 {
    vget_lane_f64::<0>(vsqrt_f64(vdup_n_f64(x)))
}

// The current rounding mode (FPCR's), then the conversion.
#[cfg(target_arch = "aarch64")]
#[inline(always)]
pub unsafe fn fctid(x: f64) -> i64 {
    if x != x {
        i64::MIN
    } else if x > i64::MAX as f64 {
        i64::MAX
    } else if x == i64::MAX as f64 {
        i64::MIN // 2^63, which cvtsd2si cannot hold
    } else {
        vcvtd_s64_f64(vget_lane_f64::<0>(vrndi_f64(vdup_n_f64(x))))
    }
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
pub unsafe fn fctidz(x: f64) -> i64 {
    if x != x {
        i64::MIN
    } else if x > i64::MAX as f64 {
        i64::MAX
    } else if x == i64::MAX as f64 {
        i64::MIN
    } else {
        vcvtd_s64_f64(x)
    }
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
pub unsafe fn fctiwz(x: f64) -> i64 {
    if x != x {
        i32::MIN as i64
    } else if x > i32::MAX as f64 {
        i32::MAX as i64
    } else if x < i32::MIN as f64 {
        i32::MIN as i64
    } else {
        x as i32 as i64
    }
}

// ---- vector memory -----------------------------------------------------------

#[repr(C, align(16))]
pub struct VectorTable(pub [u8; 256]);

pub static VectorMaskL: VectorTable = VectorTable([
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D, 0x0C,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E, 0x0D,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F, 0x0E,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x0F,
]);

pub static VectorMaskR: VectorTable = VectorTable([
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF, 0xFF,
    0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF, 0xFF,
    0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00, 0xFF,
]);

pub static VectorShiftTableL: VectorTable = VectorTable([
    0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
]);

pub static VectorShiftTableR: VectorTable = VectorTable([
    0x1F, 0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10,
    0x1E, 0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F,
    0x1D, 0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E,
    0x1C, 0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D,
    0x1B, 0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C,
    0x1A, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B,
    0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A,
    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09,
    0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08,
    0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07,
    0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06,
    0x14, 0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05,
    0x13, 0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04,
    0x12, 0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03,
    0x11, 0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02,
    0x10, 0x0F, 0x0E, 0x0D, 0x0C, 0x0B, 0x0A, 0x09, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,
]);

#[inline(always)]
unsafe fn table(t: &'static VectorTable, row: u32) -> __m128i {
    _mm_load_si128(t.0.as_ptr().add((row & 0xF) as usize * 16) as *const __m128i)
}

#[inline(always)]
unsafe fn aligned_load(base: *mut u8, address: u32) -> __m128i {
    _mm_load_si128(guest(base, address & !0xF) as *const __m128i)
}

/// The byte reversal that puts a guest vector in host order, row 0 of VectorMaskL.
/// Every instruction below accounts for it (dp3 sums yzw rather than xyz, and so on).
#[inline(always)]
pub unsafe fn vreverse(v: __m128i) -> __m128i {
    _mm_shuffle_epi8(v, _mm_set_epi8(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15))
}

#[inline(always)]
pub unsafe fn lvx(base: *mut u8, address: u32) -> __m128i {
    vreverse(aligned_load(base, address))
}

#[inline(always)]
pub unsafe fn stvx(base: *mut u8, address: u32, v: __m128i) {
    _mm_store_si128(guest(base, address & !0xF) as *mut __m128i, vreverse(v));
}

#[inline(always)]
pub unsafe fn lvlx(base: *mut u8, address: u32) -> __m128i {
    _mm_shuffle_epi8(aligned_load(base, address), table(&VectorMaskL, address))
}

#[inline(always)]
pub unsafe fn lvrx(base: *mut u8, address: u32) -> __m128i {
    if address & 0xF != 0 {
        _mm_shuffle_epi8(aligned_load(base, address), table(&VectorMaskR, address))
    } else {
        _mm_setzero_si128()
    }
}

#[inline(always)]
pub unsafe fn lvsl(address: u32) -> __m128i {
    table(&VectorShiftTableL, address)
}

#[inline(always)]
pub unsafe fn lvsr(address: u32) -> __m128i {
    table(&VectorShiftTableR, address)
}

#[inline(always)]
pub unsafe fn stvehx(base: *mut u8, address: u32, v: &PPCVRegister) {
    st16(base, address, v.u16[(7 - ((address & 0xF) >> 1)) as usize]);
}

#[inline(always)]
pub unsafe fn stvewx(base: *mut u8, address: u32, v: &PPCVRegister) {
    st32(base, address, v.u32[(3 - ((address & 0xF) >> 2)) as usize]);
}

#[inline(always)]
pub unsafe fn stvlx(base: *mut u8, address: u32, v: &PPCVRegister) {
    for i in 0..(16 - (address & 0xF)) {
        st8(base, address.wrapping_add(i), v.u8[(15 - i) as usize]);
    }
}

#[inline(always)]
pub unsafe fn stvrx(base: *mut u8, address: u32, v: &PPCVRegister) {
    for i in 0..(address & 0xF) {
        st8(base, address.wrapping_sub(i).wrapping_sub(1), v.u8[i as usize]);
    }
}

// ---- vector arithmetic -------------------------------------------------------

#[inline(always)]
pub unsafe fn mm_adds_epu32(a: __m128i, b: __m128i) -> __m128i {
    _mm_add_epi32(a, _mm_min_epu32(_mm_xor_si128(a, _mm_cmpeq_epi32(a, a)), b))
}

#[inline(always)]
pub unsafe fn mm_avg_epi8(a: __m128i, b: __m128i) -> __m128i {
    let c = _mm_set1_epi8(-128);
    _mm_xor_si128(c, _mm_avg_epu8(_mm_xor_si128(c, a), _mm_xor_si128(c, b)))
}

#[inline(always)]
pub unsafe fn mm_avg_epi16(a: __m128i, b: __m128i) -> __m128i {
    let c = _mm_set1_epi16(-32768);
    _mm_xor_si128(c, _mm_avg_epu16(_mm_xor_si128(c, a), _mm_xor_si128(c, b)))
}

#[inline(always)]
pub unsafe fn mm_cvtepu32_ps(src: __m128i) -> __m128 {
    let xmm1 = _mm_add_epi32(src, _mm_set1_epi32(127));
    let mut xmm0 = _mm_slli_epi32::<23>(src);
    xmm0 = _mm_srli_epi32::<31>(xmm0);
    xmm0 = _mm_add_epi32(xmm0, xmm1);
    xmm0 = _mm_srai_epi32::<8>(xmm0);
    xmm0 = _mm_add_epi32(xmm0, _mm_set1_epi32(0x4F800000));
    let xmm2 = _mm_cvtepi32_ps(src);
    _mm_blendv_ps(xmm2, _mm_castsi128_ps(xmm0), _mm_castsi128_ps(src))
}

#[inline(always)]
pub unsafe fn mm_perm_epi8(a: __m128i, b: __m128i, c: __m128i) -> __m128i {
    let d = _mm_set1_epi8(0xF);
    let e = _mm_sub_epi8(d, _mm_and_si128(c, d));
    _mm_blendv_epi8(_mm_shuffle_epi8(a, e), _mm_shuffle_epi8(b, e), _mm_slli_epi32::<3>(c))
}

#[inline(always)]
pub unsafe fn mm_cmpgt_epu8(a: __m128i, b: __m128i) -> __m128i {
    let c = _mm_set1_epi8(-128);
    _mm_cmpgt_epi8(_mm_xor_si128(a, c), _mm_xor_si128(b, c))
}

#[inline(always)]
pub unsafe fn mm_cmpgt_epu16(a: __m128i, b: __m128i) -> __m128i {
    let c = _mm_set1_epi16(-32768);
    _mm_cmpgt_epi16(_mm_xor_si128(a, c), _mm_xor_si128(b, c))
}

#[inline(always)]
pub unsafe fn mm_vctsxs(src: __m128) -> __m128i {
    let xmm2 = _mm_cmpunord_ps(src, src);
    let xmm0 = _mm_cvttps_epi32(src);
    let mut xmm1 = _mm_cmpeq_epi32(xmm0, _mm_set1_epi32(i32::MIN));
    xmm1 = _mm_andnot_si128(_mm_castps_si128(src), xmm1);
    let dest = _mm_blendv_ps(_mm_castsi128_ps(xmm0), _mm_castsi128_ps(_mm_set1_epi32(i32::MAX)), _mm_castsi128_ps(xmm1));
    _mm_andnot_si128(_mm_castps_si128(xmm2), _mm_castps_si128(dest))
}

#[inline(always)]
pub unsafe fn mm_vctuxs(src: __m128) -> __m128i {
    // float -> unsigned word with saturation, NaN -> 0.
    let ord = _mm_cmpord_ps(src, src);
    let mut x = _mm_and_ps(src, ord);
    x = _mm_max_ps(x, _mm_set1_ps(0.0));
    x = _mm_min_ps(x, _mm_set1_ps(4294967040.0)); // largest float <= UINT32_MAX
    // cvttps_epi32 is signed, so split at 2^31 and re-add the bias afterwards.
    let bias = _mm_set1_ps(2147483648.0);
    let hi_mask = _mm_cmpge_ps(x, bias);
    let lo = _mm_cvttps_epi32(_mm_andnot_ps(hi_mask, x));
    let mut hi = _mm_cvttps_epi32(_mm_and_ps(hi_mask, _mm_sub_ps(x, bias)));
    hi = _mm_add_epi32(hi, _mm_and_si128(_mm_castps_si128(hi_mask), _mm_set1_epi32(i32::MIN)));
    _mm_or_si128(lo, hi)
}

#[inline(always)]
pub unsafe fn mm_vsr(a: __m128i, b: __m128i) -> __m128i {
    let b = _mm_srli_epi64::<61>(_mm_slli_epi64::<61>(b));
    _mm_castps_si128(_mm_insert_ps::<0x10>(
        _mm_castsi128_ps(_mm_srl_epi64(a, b)),
        _mm_castsi128_ps(_mm_srl_epi64(_mm_srli_si128::<4>(a), b)),
    ))
}

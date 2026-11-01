// The shapes in which generated code uses a register held in a local, checked at
// the optimisation level it is built with. rustc 1.97 folds a read of one union
// field to a named constant the local started from, whatever was written to a
// sibling since; these keep watch on that bug's whole neighbourhood, including
// the form that is broken today, which must stay out of the generator.
//
//   rustc --edition=2024 -O union_locals.rs && ./union_locals

#![allow(unsafe_op_in_unsafe_fn)]

use std::hint::black_box as b;

#[repr(C)]
#[derive(Copy, Clone)]
union R {
    u8: u8,
    u16: u16,
    u32: u32,
    s32: i32,
    u64: u64,
    s64: i64,
    f32: f32,
    f64: f64,
}

#[repr(C, align(16))]
#[derive(Copy, Clone)]
union V {
    u8: [u8; 16],
    u32: [u32; 4],
    u64: [u64; 2],
    f32: [f32; 4],
}

const ZERO: R = R { u64: 0 };

macro_rules! check {
    ($failed:ident, $name:literal, $got:expr, $want:expr) => {
        if ($got) != ($want) {
            println!("{}: {:x?}, not {:x?}", $name, $got, $want);
            $failed += 1;
        }
    };
}

#[inline(never)]
unsafe fn generated_forms(x: u64) -> u32 {
    let mut failed = 0;
    let want = x as u32;

    // A write to one field, read through a narrower one.
    let mut r = R { u64: 0 };
    r.u64 = x;
    check!(failed, "u64 to u32", r.u32, want);

    // The shape that found the bug: read-modify-write, then a narrow test.
    let mut ctr = R { u64: 0 };
    ctr.u64 = x;
    ctr.u64 = ctr.u64.wrapping_sub(1);
    check!(failed, "ctr", ctr.u32, want.wrapping_sub(1));

    // A constant written to one field and read through another (li, lis).
    let mut r = R { u64: 0 };
    r.s64 = 5;
    check!(failed, "li", r.u32, 5);
    r.s64 = -1;
    check!(failed, "li negative", r.u32, u32::MAX);
    check!(failed, "li, bytes", r.u8, 0xFF);

    // The same as a branch: the bug shows when the narrow read meets a constant.
    let mut ctr = R { u64: 0 };
    ctr.u64 = x;
    ctr.u64 = ctr.u64.wrapping_sub(1);
    check!(failed, "ctr tested", ctr.u32 == 0, want == 1);
    ctr.u64 = ctr.u64.wrapping_sub(1);
    check!(failed, "ctr tested again", ctr.u32 == 0, want == 2);

    let mut r = R { u64: 0 };
    r.u64 = x;
    check!(failed, "narrow test", r.u32 == 0, want == 0);
    check!(failed, "byte test", r.u8 == 0, (x as u8) == 0);

    let mut r = R { u64: 0 };
    r.s64 = b(1) as i64;
    r.s64 -= 1;
    check!(failed, "signed test", r.s32 == 0, true);

    let mut v = V { u64: [0, 0] };
    v.u64 = [x, x];
    check!(failed, "lane test", v.u32[0] == 0, want == 0);

    // A narrow write keeps the rest of the register (stwu's write-back).
    let mut r = R { u64: 0 };
    r.u64 = x;
    r.u32 = 7;
    check!(failed, "narrow write", r.u64, (x & 0xFFFF_FFFF_0000_0000) | 7);
    r.u8 = 9;
    check!(failed, "byte write", r.u64, (x & 0xFFFF_FFFF_0000_0000) | 9);

    // A constant float read as its bits (vupkd3d128's temp).
    let mut temp = R { u64: 0 };
    temp.f32 = 3.0;
    check!(failed, "float bits", temp.u32, 0x4040_0000);
    temp.s32 = temp.s32.wrapping_add(x as i32);
    check!(failed, "float bits plus", temp.u32, 0x4040_0000u32.wrapping_add(want));

    // The same with doubles.
    let mut f = R { u64: 0 };
    f.f64 = 1.0;
    check!(failed, "double bits", f.u64, 0x3FF0_0000_0000_0000u64);

    // Vector locals, lane by lane.
    let mut v = V { u64: [0, 0] };
    v.u64 = [x, !x];
    check!(failed, "vector u32", v.u32[1], (x >> 32) as u32);
    check!(failed, "vector u8", v.u8[8], !x as u8);
    v.u32[0] = 1;
    check!(failed, "vector lane", v.u64[0], (x & 0xFFFF_FFFF_0000_0000) | 1);
    v.f32[3] = 1.0;
    check!(failed, "vector float", v.u32[3], 0x3F80_0000);

    // Through a copy, as `v = vTemp` is.
    let mut t = V { u64: [0, 0] };
    t.u32[2] = want;
    let w = t;
    check!(failed, "vector copy", w.u64[1] as u32, want);

    // A branch between the write and the read.
    let mut r = R { u64: 0 };
    if b(x) & 1 == 0 {
        r.u64 = x;
    } else {
        r.u64 = x;
    }
    check!(failed, "over a branch", r.u32, want);

    // A loop, so that the local has been both constant and not.
    let mut r = R { u64: 0 };
    for _ in 0..b(3) {
        r.u64 = r.u64.wrapping_add(x);
    }
    check!(failed, "after a loop", r.u32, want.wrapping_mul(3));

    failed
}

// What must stay out of the generator. Reports, without failing: when it comes
// out correct, the workaround can go.
#[inline(never)]
unsafe fn named_constant(x: u64) -> u32 {
    let mut r = ZERO;
    r.u64 = x;
    r.u64 = r.u64.wrapping_sub(1);
    if r.u32 == 0 { 7 } else { 9 }
}

fn main() {
    unsafe {
        let mut failed = 0;
        for x in [0x1_0000_0002u64, 1, 0xFFFF_FFFF_FFFF_FFFF, 0x1234_5678_9ABC_DEF0, 7] {
            failed += generated_forms(b(x));
        }
        let broken = named_constant(b(0x1234_5678_9ABC_DEF0)) != 9;
        println!("{failed} failures; a named constant is {}", if broken { "still miscompiled" } else { "compiled correctly" });
        std::process::exit(if failed == 0 { 0 } else { 1 });
    }
}

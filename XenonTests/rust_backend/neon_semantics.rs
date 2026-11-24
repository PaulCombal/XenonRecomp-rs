// XenonUtils/rust/neon.rs against a lane-by-lane model of the x86 manuals, on random
// vectors and the awkward values (NaN, infinities, zeros of both signs, the extremes
// of each integer width). Run on an arm64 host:
//
//   rustc --edition 2024 -O --test XenonTests/rust_backend/neon_semantics.rs -o /tmp/neon_semantics && /tmp/neon_semantics
//
// (x86 needs no such test: the intrinsics there are the manuals' own.)

#![cfg(target_arch = "aarch64")]
#![allow(non_snake_case, non_camel_case_types, unsafe_op_in_unsafe_fn, clippy::all)]

#[path = "../../XenonUtils/rust/neon.rs"]
mod neon;
use neon::*;
use core::mem::transmute;

type V = [u8; 16];

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn vector(&mut self) -> V {
        let mut v = [0u8; 16];
        // Mostly random bytes, with runs of the extremes so saturation is reached.
        for i in 0..16 {
            let r = self.next();
            v[i] = match r % 8 {
                0 => 0x00,
                1 => 0xFF,
                2 => 0x7F,
                3 => 0x80,
                _ => (r >> 8) as u8,
            };
        }
        v
    }
    fn floats(&mut self) -> V {
        const SPECIAL: [u32; 14] = [
            0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0xFFC00000,
            0x00000001, 0x80000001, 0x4F000000, 0xCF000000, 0x4EFFFFFF, 0xCF000001,
        ];
        let mut v = [0u8; 16];
        for lane in 0..4 {
            let r = self.next();
            let bits: u32 = match r % 4 {
                0 => SPECIAL[(r >> 8) as usize % SPECIAL.len()],
                1 => ((r >> 8) as f32 / 1000.0 - 1.0e6).to_bits(),
                _ => (r >> 16) as u32,
            };
            v[lane * 4..lane * 4 + 4].copy_from_slice(&bits.to_le_bytes());
        }
        v
    }
}

fn mi(v: V) -> __m128i {
    unsafe { transmute(v) }
}
fn mf(v: V) -> __m128 {
    unsafe { transmute(v) }
}
fn bytes_i(m: __m128i) -> V {
    unsafe { transmute(m) }
}
fn bytes_f(m: __m128) -> V {
    unsafe { transmute(m) }
}

fn s8(v: V) -> [i8; 16] {
    unsafe { transmute(v) }
}
fn u16s(v: V) -> [u16; 8] {
    unsafe { transmute(v) }
}
fn s16(v: V) -> [i16; 8] {
    unsafe { transmute(v) }
}
fn u32s(v: V) -> [u32; 4] {
    unsafe { transmute(v) }
}
fn s32(v: V) -> [i32; 4] {
    unsafe { transmute(v) }
}
fn u64s(v: V) -> [u64; 2] {
    unsafe { transmute(v) }
}
fn f32s(v: V) -> [f32; 4] {
    unsafe { transmute(v) }
}
fn from_u16(a: [u16; 8]) -> V {
    unsafe { transmute(a) }
}
fn from_s16(a: [i16; 8]) -> V {
    unsafe { transmute(a) }
}
fn from_u32(a: [u32; 4]) -> V {
    unsafe { transmute(a) }
}
fn from_s32(a: [i32; 4]) -> V {
    unsafe { transmute(a) }
}
fn from_u64(a: [u64; 2]) -> V {
    unsafe { transmute(a) }
}
fn from_f32(a: [f32; 4]) -> V {
    unsafe { transmute(a) }
}
fn from_s8(a: [i8; 16]) -> V {
    unsafe { transmute(a) }
}

const ROUNDS: usize = 200_000;

fn binary_i(name: &str, f: impl Fn(__m128i, __m128i) -> __m128i, model: impl Fn(V, V) -> V) {
    let mut rng = Rng(0x9E3779B97F4A7C15);
    for _ in 0..ROUNDS {
        let (a, b) = (rng.vector(), rng.vector());
        let got = bytes_i(f(mi(a), mi(b)));
        assert_eq!(got, model(a, b), "{name}({a:02X?}, {b:02X?})");
    }
}

/// Floats compare by bits except NaNs, which compare as NaNs: x86 and NEON agree on
/// which lanes are NaN but not always on which NaN comes out of arithmetic, and the
/// code does not look.
fn same_floats(a: V, b: V) -> bool {
    let (x, y) = (f32s(a), f32s(b));
    (0..4).all(|i| if x[i].is_nan() { y[i].is_nan() } else { x[i].to_bits() == y[i].to_bits() })
}

fn binary_f(name: &str, f: impl Fn(__m128, __m128) -> __m128, model: impl Fn(f32, f32) -> f32) {
    let mut rng = Rng(0xD1B54A32D192ED03);
    for _ in 0..ROUNDS {
        let (a, b) = (rng.floats(), rng.floats());
        let got = bytes_f(f(mf(a), mf(b)));
        let want = from_f32([0, 1, 2, 3].map(|i| model(f32s(a)[i], f32s(b)[i])));
        assert!(same_floats(got, want), "{name}({:?}, {:?}) = {:?}, want {:?}", f32s(a), f32s(b), f32s(got), f32s(want));
    }
}

fn mask_f(name: &str, f: impl Fn(__m128, __m128) -> __m128, model: impl Fn(f32, f32) -> bool) {
    let mut rng = Rng(0x2545F4914F6CDD1D);
    for _ in 0..ROUNDS {
        let (a, b) = (rng.floats(), rng.floats());
        let got = u32s(bytes_f(f(mf(a), mf(b))));
        let want = [0, 1, 2, 3].map(|i| if model(f32s(a)[i], f32s(b)[i]) { !0u32 } else { 0 });
        assert_eq!(got, want, "{name}({:?}, {:?})", f32s(a), f32s(b));
    }
}

#[test]
fn bitwise() {
    unsafe {
        binary_i("and", |a, b| _mm_and_si128(a, b), |a, b| core::array::from_fn(|i| a[i] & b[i]));
        binary_i("or", |a, b| _mm_or_si128(a, b), |a, b| core::array::from_fn(|i| a[i] | b[i]));
        binary_i("xor", |a, b| _mm_xor_si128(a, b), |a, b| core::array::from_fn(|i| a[i] ^ b[i]));
        binary_i("andnot", |a, b| _mm_andnot_si128(a, b), |a, b| core::array::from_fn(|i| !a[i] & b[i]));
        let mut rng = Rng(7);
        for _ in 0..1000 {
            let (a, b) = (rng.floats(), rng.floats());
            assert_eq!(bytes_f(_mm_and_ps(mf(a), mf(b))), core::array::from_fn(|i| a[i] & b[i]));
            assert_eq!(bytes_f(_mm_xor_ps(mf(a), mf(b))), core::array::from_fn(|i| a[i] ^ b[i]));
            assert_eq!(bytes_f(_mm_andnot_ps(mf(a), mf(b))), core::array::from_fn(|i| !a[i] & b[i]));
        }
    }
}

#[test]
fn integer_arithmetic() {
    unsafe {
        binary_i("add8", |a, b| _mm_add_epi8(a, b), |a, b| core::array::from_fn(|i| a[i].wrapping_add(b[i])));
        binary_i("sub8", |a, b| _mm_sub_epi8(a, b), |a, b| core::array::from_fn(|i| a[i].wrapping_sub(b[i])));
        binary_i(
            "add16",
            |a, b| _mm_add_epi16(a, b),
            |a, b| from_u16(core::array::from_fn(|i| u16s(a)[i].wrapping_add(u16s(b)[i]))),
        );
        binary_i(
            "sub16",
            |a, b| _mm_sub_epi16(a, b),
            |a, b| from_u16(core::array::from_fn(|i| u16s(a)[i].wrapping_sub(u16s(b)[i]))),
        );
        binary_i(
            "add32",
            |a, b| _mm_add_epi32(a, b),
            |a, b| from_u32(core::array::from_fn(|i| u32s(a)[i].wrapping_add(u32s(b)[i]))),
        );
        binary_i("adds_u8", |a, b| _mm_adds_epu8(a, b), |a, b| core::array::from_fn(|i| a[i].saturating_add(b[i])));
        binary_i("subs_u8", |a, b| _mm_subs_epu8(a, b), |a, b| core::array::from_fn(|i| a[i].saturating_sub(b[i])));
        binary_i(
            "adds_s16",
            |a, b| _mm_adds_epi16(a, b),
            |a, b| from_s16(core::array::from_fn(|i| s16(a)[i].saturating_add(s16(b)[i]))),
        );
        binary_i(
            "avg_u8",
            |a, b| _mm_avg_epu8(a, b),
            |a, b| core::array::from_fn(|i| ((a[i] as u16 + b[i] as u16 + 1) >> 1) as u8),
        );
        binary_i(
            "avg_u16",
            |a, b| _mm_avg_epu16(a, b),
            |a, b| from_u16(core::array::from_fn(|i| ((u16s(a)[i] as u32 + u16s(b)[i] as u32 + 1) >> 1) as u16)),
        );
        binary_i(
            "min_u32",
            |a, b| _mm_min_epu32(a, b),
            |a, b| from_u32(core::array::from_fn(|i| u32s(a)[i].min(u32s(b)[i]))),
        );
        binary_i(
            "max_s32",
            |a, b| _mm_max_epi32(a, b),
            |a, b| from_s32(core::array::from_fn(|i| s32(a)[i].max(s32(b)[i]))),
        );
        binary_i(
            "cmpeq8",
            |a, b| _mm_cmpeq_epi8(a, b),
            |a, b| core::array::from_fn(|i| if a[i] == b[i] { 0xFF } else { 0 }),
        );
        binary_i(
            "cmpeq32",
            |a, b| _mm_cmpeq_epi32(a, b),
            |a, b| from_u32(core::array::from_fn(|i| if u32s(a)[i] == u32s(b)[i] { !0 } else { 0 })),
        );
        binary_i(
            "cmpgt8",
            |a, b| _mm_cmpgt_epi8(a, b),
            |a, b| core::array::from_fn(|i| if s8(a)[i] > s8(b)[i] { 0xFF } else { 0 }),
        );
        binary_i(
            "cmpgt16",
            |a, b| _mm_cmpgt_epi16(a, b),
            |a, b| from_u16(core::array::from_fn(|i| if s16(a)[i] > s16(b)[i] { !0 } else { 0 })),
        );
    }
}

#[test]
fn shifts() {
    unsafe {
        macro_rules! shifts {
            ($($n:literal),*) => {$(
                binary_i(concat!("slli32::", $n), |a, _| _mm_slli_epi32::<$n>(a), |a, _| from_u32(u32s(a).map(|x| if $n > 31 { 0 } else { x << $n })));
                binary_i(concat!("srli32::", $n), |a, _| _mm_srli_epi32::<$n>(a), |a, _| from_u32(u32s(a).map(|x| if $n > 31 { 0 } else { x >> $n })));
                binary_i(concat!("srai32::", $n), |a, _| _mm_srai_epi32::<$n>(a), |a, _| from_s32(s32(a).map(|x| x >> ($n as u32).min(31))));
                binary_i(concat!("slli64::", $n), |a, _| _mm_slli_epi64::<$n>(a), |a, _| from_u64(u64s(a).map(|x| if $n > 63 { 0 } else { x << $n })));
                binary_i(concat!("srli64::", $n), |a, _| _mm_srli_epi64::<$n>(a), |a, _| from_u64(u64s(a).map(|x| if $n > 63 { 0 } else { x >> $n })));
            )*};
        }
        shifts!(0, 1, 3, 8, 23, 31, 32, 33, 61, 63, 64, 70);
        binary_i(
            "srl64",
            |a, c| _mm_srl_epi64(a, c),
            |a, c| {
                let n = u64s(c)[0];
                from_u64(u64s(a).map(|x| if n > 63 { 0 } else { x >> n }))
            },
        );
        // Counts in the range the code uses.
        let mut rng = Rng(11);
        for _ in 0..ROUNDS {
            let a = rng.vector();
            let n = rng.next() % 70;
            let got = u64s(bytes_i(_mm_srl_epi64(mi(a), mi(from_u64([n, rng.next()])))));
            let want = u64s(a).map(|x| if n > 63 { 0 } else { x >> n });
            assert_eq!(got, want);
        }
        macro_rules! bytes {
            ($($n:literal),*) => {$(
                binary_i(concat!("srli_si128::", $n), |a, _| _mm_srli_si128::<$n>(a), |a, _| core::array::from_fn(|i| if i + $n < 16 { a[i + $n] } else { 0 }));
            )*};
        }
        bytes!(0, 1, 4, 8, 15);
    }
}

#[test]
fn shuffles_and_unpacks() {
    unsafe {
        binary_i(
            "shuffle_epi8",
            |a, m| _mm_shuffle_epi8(a, m),
            |a, m| core::array::from_fn(|i| if m[i] & 0x80 != 0 { 0 } else { a[(m[i] & 15) as usize] }),
        );
        macro_rules! shuffle32 {
            ($($imm:literal),*) => {$(
                binary_i(concat!("shuffle_epi32::", $imm), |a, _| _mm_shuffle_epi32::<$imm>(a), |a, _| {
                    let l = u32s(a);
                    from_u32([l[$imm & 3], l[($imm >> 2) & 3], l[($imm >> 4) & 3], l[($imm >> 6) & 3]])
                });
                binary_i(concat!("shuffle_ps::", $imm), |a, b| bytes_i_f(_mm_shuffle_ps::<$imm>(mf_i(a), mf_i(b))), |a, b| {
                    let (l, m) = (u32s(a), u32s(b));
                    from_u32([l[$imm & 3], l[($imm >> 2) & 3], m[($imm >> 4) & 3], m[($imm >> 6) & 3]])
                });
            )*};
        }
        shuffle32!(0x00, 0x1B, 0xE4, 0xFF, 0x4E, 0xB1, 0x39, 0x93, 0xC6);
        macro_rules! align {
            ($($n:literal),*) => {$(
                binary_i(concat!("alignr::", $n), |a, b| _mm_alignr_epi8::<$n>(a, b), |a, b| {
                    let mut both = [0u8; 32];
                    both[..16].copy_from_slice(&b);
                    both[16..].copy_from_slice(&a);
                    core::array::from_fn(|i| both[i + $n])
                });
            )*};
        }
        align!(0, 1, 4, 8, 12, 15, 16);
        binary_i("unpacklo8", |a, b| _mm_unpacklo_epi8(a, b), |a, b| core::array::from_fn(|i| if i % 2 == 0 { a[i / 2] } else { b[i / 2] }));
        binary_i("unpackhi8", |a, b| _mm_unpackhi_epi8(a, b), |a, b| core::array::from_fn(|i| if i % 2 == 0 { a[8 + i / 2] } else { b[8 + i / 2] }));
        binary_i("unpacklo16", |a, b| _mm_unpacklo_epi16(a, b), |a, b| {
            from_u16(core::array::from_fn(|i| if i % 2 == 0 { u16s(a)[i / 2] } else { u16s(b)[i / 2] }))
        });
        binary_i("unpackhi16", |a, b| _mm_unpackhi_epi16(a, b), |a, b| {
            from_u16(core::array::from_fn(|i| if i % 2 == 0 { u16s(a)[4 + i / 2] } else { u16s(b)[4 + i / 2] }))
        });
        binary_i("unpacklo32", |a, b| _mm_unpacklo_epi32(a, b), |a, b| {
            from_u32(core::array::from_fn(|i| if i % 2 == 0 { u32s(a)[i / 2] } else { u32s(b)[i / 2] }))
        });
        binary_i("unpackhi32", |a, b| _mm_unpackhi_epi32(a, b), |a, b| {
            from_u32(core::array::from_fn(|i| if i % 2 == 0 { u32s(a)[2 + i / 2] } else { u32s(b)[2 + i / 2] }))
        });
        binary_i("unpacklo64", |a, b| _mm_unpacklo_epi64(a, b), |a, b| from_u64([u64s(a)[0], u64s(b)[0]]));
        binary_i("unpackhi64", |a, b| _mm_unpackhi_epi64(a, b), |a, b| from_u64([u64s(a)[1], u64s(b)[1]]));
    }
}

fn mf_i(a: __m128i) -> __m128 {
    unsafe { _mm_castsi128_ps(a) }
}
fn bytes_i_f(a: __m128) -> __m128i {
    unsafe { _mm_castps_si128(a) }
}

#[test]
fn pack_and_widen() {
    unsafe {
        binary_i("packus16", |a, b| _mm_packus_epi16(a, b), |a, b| {
            let (x, y) = (s16(a), s16(b));
            core::array::from_fn(|i| {
                let v = if i < 8 { x[i] } else { y[i - 8] };
                v.clamp(0, 255) as u8
            })
        });
        binary_i("packs32", |a, b| _mm_packs_epi32(a, b), |a, b| {
            let (x, y) = (s32(a), s32(b));
            from_s16(core::array::from_fn(|i| {
                let v = if i < 4 { x[i] } else { y[i - 4] };
                v.clamp(i16::MIN as i32, i16::MAX as i32) as i16
            }))
        });
        binary_i("cvtepi8_epi16", |a, _| _mm_cvtepi8_epi16(a), |a, _| from_s16(core::array::from_fn(|i| s8(a)[i] as i16)));
        binary_i("cvtepi16_epi32", |a, _| _mm_cvtepi16_epi32(a), |a, _| from_s32(core::array::from_fn(|i| s16(a)[i] as i32)));
    }
}

#[test]
fn float_arithmetic() {
    unsafe {
        binary_f("add", |a, b| _mm_add_ps(a, b), |a, b| a + b);
        binary_f("sub", |a, b| _mm_sub_ps(a, b), |a, b| a - b);
        binary_f("mul", |a, b| _mm_mul_ps(a, b), |a, b| a * b);
        binary_f("div", |a, b| _mm_div_ps(a, b), |a, b| a / b);
        // minps and maxps: the second operand unless the first is strictly less (greater).
        binary_f("min", |a, b| _mm_min_ps(a, b), |a, b| if a < b { a } else { b });
        binary_f("max", |a, b| _mm_max_ps(a, b), |a, b| if a > b { a } else { b });
        binary_f("sqrt", |a, _| _mm_sqrt_ps(a), |a, _| a.sqrt());
        mask_f("cmpeq", |a, b| _mm_cmpeq_ps(a, b), |a, b| a == b);
        mask_f("cmpgt", |a, b| _mm_cmpgt_ps(a, b), |a, b| a > b);
        mask_f("cmpge", |a, b| _mm_cmpge_ps(a, b), |a, b| a >= b);
        mask_f("cmpord", |a, b| _mm_cmpord_ps(a, b), |a, b| !a.is_nan() && !b.is_nan());
        mask_f("cmpunord", |a, b| _mm_cmpunord_ps(a, b), |a, b| a.is_nan() || b.is_nan());
    }
}

#[test]
fn rounding_and_conversion() {
    unsafe {
        binary_f("round_nearest", |a, _| _mm_round_ps::<{ _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC }>(a), |a, _| a.round_ties_even());
        binary_f("round_down", |a, _| _mm_round_ps::<{ _MM_FROUND_TO_NEG_INF | _MM_FROUND_NO_EXC }>(a), |a, _| a.floor());
        binary_f("round_up", |a, _| _mm_round_ps::<{ _MM_FROUND_TO_POS_INF | _MM_FROUND_NO_EXC }>(a), |a, _| a.ceil());
        binary_f("round_zero", |a, _| _mm_round_ps::<{ _MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC }>(a), |a, _| a.trunc());

        let mut rng = Rng(99);
        for _ in 0..ROUNDS {
            let a = rng.floats();
            // cvttps2dq: truncate, 0x80000000 for a NaN or anything outside i32.
            let want = from_s32(f32s(a).map(|x| if x.is_nan() || x >= 2147483648.0 || x < -2147483648.0 { i32::MIN } else { x as i32 }));
            assert_eq!(bytes_i(_mm_cvttps_epi32(mf(a))), want, "cvttps_epi32({:?})", f32s(a));
            // cvtdq2ps, rounding to nearest.
            let b = rng.vector();
            assert_eq!(bytes_f(_mm_cvtepi32_ps(mi(b))), from_f32(s32(b).map(|x| x as f32)));
        }
    }
}

#[test]
fn blends_and_inserts() {
    unsafe {
        macro_rules! blend {
            ($($imm:literal),*) => {$(
                binary_i(concat!("blend_ps::", $imm), |a, b| bytes_i_f(_mm_blend_ps::<$imm>(mf_i(a), mf_i(b))), |a, b| {
                    from_u32(core::array::from_fn(|i| if ($imm >> i) & 1 != 0 { u32s(b)[i] } else { u32s(a)[i] }))
                });
            )*};
        }
        blend!(0x0, 0x1, 0x5, 0xA, 0xF, 0x8);
        let mut rng = Rng(5);
        for _ in 0..ROUNDS {
            let (a, b, m) = (rng.vector(), rng.vector(), rng.vector());
            let want = from_u32(core::array::from_fn(|i| if u32s(m)[i] >> 31 != 0 { u32s(b)[i] } else { u32s(a)[i] }));
            assert_eq!(bytes_f(_mm_blendv_ps(mf(a), mf(b), mf(m))), want);
            let want = core::array::from_fn(|i| if m[i] & 0x80 != 0 { b[i] } else { a[i] });
            assert_eq!(bytes_i(_mm_blendv_epi8(mi(a), mi(b), mi(m))), want);
        }
        macro_rules! insert {
            ($($imm:literal),*) => {$(
                binary_i(concat!("insert_ps::", $imm), |a, b| bytes_i_f(_mm_insert_ps::<$imm>(mf_i(a), mf_i(b))), |a, b| {
                    let mut l = u32s(a);
                    l[($imm >> 4) & 3] = u32s(b)[($imm >> 6) & 3];
                    for i in 0..4 {
                        if ($imm >> i) & 1 != 0 {
                            l[i] = 0;
                        }
                    }
                    from_u32(l)
                });
            )*};
        }
        insert!(0x10, 0x00, 0xFF, 0x4A, 0x9C, 0xE5);
    }
}

#[test]
fn dot_products() {
    unsafe {
        let mut rng = Rng(21);
        macro_rules! check {
            ($imm:literal) => {
                for _ in 0..ROUNDS {
                    let (a, b) = (rng.floats(), rng.floats());
                    let (x, y) = (f32s(a), f32s(b));
                    let p: [f32; 4] = core::array::from_fn(|i| if ($imm >> (4 + i)) & 1 != 0 { x[i] * y[i] } else { 0.0 });
                    let sum = (p[0] + p[1]) + (p[2] + p[3]);
                    let want = from_f32(core::array::from_fn(|i| if ($imm >> i) & 1 != 0 { sum } else { 0.0 }));
                    let got = bytes_f(_mm_dp_ps::<$imm>(mf(a), mf(b)));
                    assert!(same_floats(got, want), "dp_ps::<{:#X}>({:?}, {:?}) = {:?}, want {:?}", $imm, x, y, f32s(got), f32s(want));
                }
            };
        }
        check!(0xEF);
        check!(0xFF);
        check!(0x71);
        check!(0x33);
    }
}

#[test]
fn masks_and_memory() {
    unsafe {
        let mut rng = Rng(1234);
        for _ in 0..ROUNDS {
            let a = rng.vector();
            let want_ps = (0..4).map(|i| ((u32s(a)[i] >> 31) << i) as i32).sum::<i32>();
            assert_eq!(_mm_movemask_ps(mf(a)), want_ps);
            let want_b = (0..16).map(|i| ((a[i] >> 7) as i32) << i).sum::<i32>();
            assert_eq!(_mm_movemask_epi8(mi(a)), want_b);
            assert_eq!(bytes_i(_mm_setzero_si128()), [0; 16]);
            assert_eq!(bytes_i(_mm_set1_epi8(a[0] as i8)), [a[0]; 16]);
            assert_eq!(bytes_i(_mm_set1_epi16(i16::from_le_bytes([a[0], a[1]]))), from_u16([u16::from_le_bytes([a[0], a[1]]); 8]));
            assert_eq!(bytes_i(_mm_set1_epi32(i32::from_le_bytes([a[0], a[1], a[2], a[3]]))), from_u32([u32::from_le_bytes([a[0], a[1], a[2], a[3]]); 4]));
            let mut slot = [0u8; 16];
            _mm_store_si128(slot.as_mut_ptr() as *mut __m128i, mi(a));
            assert_eq!(slot, a);
            assert_eq!(bytes_i(_mm_load_si128(slot.as_ptr() as *const __m128i)), a);
        }
        let high_to_low = _mm_set_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
        assert_eq!(bytes_i(high_to_low), core::array::from_fn(|i| i as u8));
        assert_eq!(from_s8(s8(bytes_i(high_to_low))), bytes_i(high_to_low));
    }
}

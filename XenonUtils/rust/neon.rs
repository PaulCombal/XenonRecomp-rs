// The x86 intrinsics recompiled code and ppc_rt.rs use, on AArch64: the same names,
// the same lanes, the same results, in NEON. ppc_rt.rs includes this file in place of
// core::arch::x86_64 when it is built for arm64, so recompiled code needs no change.
//
// Where x86 and NEON differ, x86's behaviour is the one kept, as the console's code
// (and the x86 build of it) expects it: cvttps_epi32 gives 0x80000000 for a NaN or a
// value out of range where NEON saturates, min and max return their second operand
// for a NaN or a pair of zeros where NEON returns a NaN, a shift by more than the
// width gives 0 (or the sign) and so on. The unit tests of XenonTests/rust_backend
// (neon_semantics.rs) check each against a plain lane-by-lane model of the manual.

use core::arch::aarch64::*;
use core::mem::transmute;

/// 128 bits of integer lanes, as __m128i.
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct __m128i(int64x2_t);

/// Four f32 lanes, as __m128.
#[repr(transparent)]
#[derive(Copy, Clone)]
pub struct __m128(float32x4_t);

pub const _MM_FROUND_TO_NEAREST_INT: i32 = 0x00;
pub const _MM_FROUND_TO_NEG_INF: i32 = 0x01;
pub const _MM_FROUND_TO_POS_INF: i32 = 0x02;
pub const _MM_FROUND_TO_ZERO: i32 = 0x03;
pub const _MM_FROUND_NO_EXC: i32 = 0x08;

// ---- views -------------------------------------------------------------------

#[inline(always)]
fn u8x(a: __m128i) -> uint8x16_t {
    unsafe { vreinterpretq_u8_s64(a.0) }
}
#[inline(always)]
fn s8x(a: __m128i) -> int8x16_t {
    unsafe { vreinterpretq_s8_s64(a.0) }
}
#[inline(always)]
fn u16x(a: __m128i) -> uint16x8_t {
    unsafe { vreinterpretq_u16_s64(a.0) }
}
#[inline(always)]
fn s16x(a: __m128i) -> int16x8_t {
    unsafe { vreinterpretq_s16_s64(a.0) }
}
#[inline(always)]
fn u32x(a: __m128i) -> uint32x4_t {
    unsafe { vreinterpretq_u32_s64(a.0) }
}
#[inline(always)]
fn s32x(a: __m128i) -> int32x4_t {
    unsafe { vreinterpretq_s32_s64(a.0) }
}
#[inline(always)]
fn u64x(a: __m128i) -> uint64x2_t {
    unsafe { vreinterpretq_u64_s64(a.0) }
}
#[inline(always)]
fn fx(a: __m128) -> float32x4_t {
    a.0
}

#[inline(always)]
fn from_u8(v: uint8x16_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_u8(v) })
}
#[inline(always)]
fn from_s8(v: int8x16_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_s8(v) })
}
#[inline(always)]
fn from_u16(v: uint16x8_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_u16(v) })
}
#[inline(always)]
fn from_s16(v: int16x8_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_s16(v) })
}
#[inline(always)]
fn from_u32(v: uint32x4_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_u32(v) })
}
#[inline(always)]
fn from_s32(v: int32x4_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_s32(v) })
}
#[inline(always)]
fn from_u64(v: uint64x2_t) -> __m128i {
    __m128i(unsafe { vreinterpretq_s64_u64(v) })
}
#[inline(always)]
fn from_f(v: float32x4_t) -> __m128 {
    __m128(v)
}

// ---- casts, loads, stores, constants -----------------------------------------

#[inline(always)]
pub unsafe fn _mm_castsi128_ps(a: __m128i) -> __m128 {
    __m128(vreinterpretq_f32_s64(a.0))
}
#[inline(always)]
pub unsafe fn _mm_castps_si128(a: __m128) -> __m128i {
    __m128i(vreinterpretq_s64_f32(a.0))
}

#[inline(always)]
pub unsafe fn _mm_load_si128(p: *const __m128i) -> __m128i {
    __m128i(vld1q_s64(p as *const i64))
}
#[inline(always)]
pub unsafe fn _mm_store_si128(p: *mut __m128i, a: __m128i) {
    vst1q_s64(p as *mut i64, a.0)
}

#[inline(always)]
pub unsafe fn _mm_setzero_si128() -> __m128i {
    __m128i(vdupq_n_s64(0))
}
#[inline(always)]
pub unsafe fn _mm_set1_ps(x: f32) -> __m128 {
    __m128(vdupq_n_f32(x))
}
#[inline(always)]
pub unsafe fn _mm_set1_epi8(x: i8) -> __m128i {
    from_s8(vdupq_n_s8(x))
}
#[inline(always)]
pub unsafe fn _mm_set1_epi16(x: i16) -> __m128i {
    from_s16(vdupq_n_s16(x))
}
#[inline(always)]
pub unsafe fn _mm_set1_epi32(x: i32) -> __m128i {
    from_s32(vdupq_n_s32(x))
}

/// The arguments run from the highest byte to the lowest, as in x86.
#[inline(always)]
pub unsafe fn _mm_set_epi8(
    e15: i8, e14: i8, e13: i8, e12: i8, e11: i8, e10: i8, e9: i8, e8: i8,
    e7: i8, e6: i8, e5: i8, e4: i8, e3: i8, e2: i8, e1: i8, e0: i8,
) -> __m128i {
    let bytes = [e0, e1, e2, e3, e4, e5, e6, e7, e8, e9, e10, e11, e12, e13, e14, e15];
    from_s8(vld1q_s8(bytes.as_ptr()))
}

// ---- bitwise ------------------------------------------------------------------

#[inline(always)]
pub unsafe fn _mm_and_si128(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vandq_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_or_si128(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vorrq_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_xor_si128(a: __m128i, b: __m128i) -> __m128i {
    from_u8(veorq_u8(u8x(a), u8x(b)))
}
/// `!a & b`.
#[inline(always)]
pub unsafe fn _mm_andnot_si128(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vbicq_u8(u8x(b), u8x(a)))
}
#[inline(always)]
pub unsafe fn _mm_and_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(_mm_and_si128(_mm_castps_si128(a), _mm_castps_si128(b)))
}
#[inline(always)]
pub unsafe fn _mm_xor_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(_mm_xor_si128(_mm_castps_si128(a), _mm_castps_si128(b)))
}
/// `!a & b`.
#[inline(always)]
pub unsafe fn _mm_andnot_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(_mm_andnot_si128(_mm_castps_si128(a), _mm_castps_si128(b)))
}

// ---- single-precision arithmetic and comparison --------------------------------

#[inline(always)]
pub unsafe fn _mm_add_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vaddq_f32(fx(a), fx(b)))
}
#[inline(always)]
pub unsafe fn _mm_sub_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vsubq_f32(fx(a), fx(b)))
}
#[inline(always)]
pub unsafe fn _mm_mul_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vmulq_f32(fx(a), fx(b)))
}
#[inline(always)]
pub unsafe fn _mm_div_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vdivq_f32(fx(a), fx(b)))
}
#[inline(always)]
pub unsafe fn _mm_sqrt_ps(a: __m128) -> __m128 {
    from_f(vsqrtq_f32(fx(a)))
}
/// `a < b ? a : b`: the second operand for a NaN and for equal values, as minps.
#[inline(always)]
pub unsafe fn _mm_min_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vbslq_f32(vcltq_f32(fx(a), fx(b)), fx(a), fx(b)))
}
/// `a > b ? a : b`.
#[inline(always)]
pub unsafe fn _mm_max_ps(a: __m128, b: __m128) -> __m128 {
    from_f(vbslq_f32(vcgtq_f32(fx(a), fx(b)), fx(a), fx(b)))
}

#[inline(always)]
pub unsafe fn _mm_cmpeq_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(from_u32(vceqq_f32(fx(a), fx(b))))
}
#[inline(always)]
pub unsafe fn _mm_cmpgt_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(from_u32(vcgtq_f32(fx(a), fx(b))))
}
#[inline(always)]
pub unsafe fn _mm_cmpge_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(from_u32(vcgeq_f32(fx(a), fx(b))))
}
#[inline(always)]
pub unsafe fn _mm_cmpord_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(from_u32(vandq_u32(vceqq_f32(fx(a), fx(a)), vceqq_f32(fx(b), fx(b)))))
}
#[inline(always)]
pub unsafe fn _mm_cmpunord_ps(a: __m128, b: __m128) -> __m128 {
    _mm_castsi128_ps(from_u32(vmvnq_u32(vandq_u32(vceqq_f32(fx(a), fx(a)), vceqq_f32(fx(b), fx(b))))))
}

/// The dot product of the lanes IMM's high nibble names, in every lane its low
/// nibble names and 0 in the others. The sum is (p0 + p1) + (p2 + p3), as the
/// instruction adds them.
#[inline(always)]
pub unsafe fn _mm_dp_ps<const IMM: i32>(a: __m128, b: __m128) -> __m128 {
    let select = |nibble: i32| -> uint32x4_t {
        let lanes = [
            if nibble & 1 != 0 { !0u32 } else { 0 },
            if nibble & 2 != 0 { !0u32 } else { 0 },
            if nibble & 4 != 0 { !0u32 } else { 0 },
            if nibble & 8 != 0 { !0u32 } else { 0 },
        ];
        vld1q_u32(lanes.as_ptr())
    };
    let product = vreinterpretq_u32_f32(vmulq_f32(fx(a), fx(b)));
    let product = vreinterpretq_f32_u32(vandq_u32(product, select((IMM >> 4) & 0xF)));
    let pairs = vpaddq_f32(product, product);
    let sum = vpaddq_f32(pairs, pairs);
    let sum = vreinterpretq_u32_f32(sum);
    from_f(vreinterpretq_f32_u32(vandq_u32(sum, select(IMM & 0xF))))
}

/// IMM's low two bits name the rounding, as x86's: nearest even, down, up, to zero.
/// (IMM's bit 2, which asks for the processor's own mode, is not used by the code.)
#[inline(always)]
pub unsafe fn _mm_round_ps<const IMM: i32>(a: __m128) -> __m128 {
    match IMM & 3 {
        0 => from_f(vrndnq_f32(fx(a))),
        1 => from_f(vrndmq_f32(fx(a))),
        2 => from_f(vrndpq_f32(fx(a))),
        _ => from_f(vrndq_f32(fx(a))),
    }
}

/// Lane i from b where IMM's bit i is set, from a elsewhere.
#[inline(always)]
pub unsafe fn _mm_blend_ps<const IMM: i32>(a: __m128, b: __m128) -> __m128 {
    let lanes = [
        if IMM & 1 != 0 { !0u32 } else { 0 },
        if IMM & 2 != 0 { !0u32 } else { 0 },
        if IMM & 4 != 0 { !0u32 } else { 0 },
        if IMM & 8 != 0 { !0u32 } else { 0 },
    ];
    from_f(vbslq_f32(vld1q_u32(lanes.as_ptr()), fx(b), fx(a)))
}

/// Lane i from b where the top bit of mask's lane i is set.
#[inline(always)]
pub unsafe fn _mm_blendv_ps(a: __m128, b: __m128, mask: __m128) -> __m128 {
    let select = vreinterpretq_u32_s32(vshrq_n_s32::<31>(vreinterpretq_s32_f32(fx(mask))));
    from_f(vbslq_f32(select, fx(b), fx(a)))
}

/// Byte i from b where the top bit of mask's byte i is set.
#[inline(always)]
pub unsafe fn _mm_blendv_epi8(a: __m128i, b: __m128i, mask: __m128i) -> __m128i {
    let select = vreinterpretq_u8_s8(vshrq_n_s8::<7>(s8x(mask)));
    from_u8(vbslq_u8(select, u8x(b), u8x(a)))
}

/// Bits 7:6 of IMM pick the lane of b, bits 5:4 the lane of a it replaces, and the
/// lanes bits 3:0 name are cleared.
#[inline(always)]
pub unsafe fn _mm_insert_ps<const IMM: i32>(a: __m128, b: __m128) -> __m128 {
    let mut lanes: [f32; 4] = transmute(fx(a));
    let source: [f32; 4] = transmute(fx(b));
    lanes[((IMM >> 4) & 3) as usize] = source[((IMM >> 6) & 3) as usize];
    for (i, lane) in lanes.iter_mut().enumerate() {
        if (IMM >> i) & 1 != 0 {
            *lane = 0.0;
        }
    }
    from_f(transmute(lanes))
}

/// The byte indexes into the 32 bytes of (a, b) that pick f32 lane `lane` of a
/// shuffle: IMM's two bits at `shift`, from b when `from_b`.
const fn lane_bytes(selector: i32, from_b: bool) -> [u8; 4] {
    let base = (selector as u8) * 4 + if from_b { 16 } else { 0 };
    [base, base + 1, base + 2, base + 3]
}

const fn shuffle_ps_table(imm: i32) -> [u8; 16] {
    let l0 = lane_bytes(imm & 3, false);
    let l1 = lane_bytes((imm >> 2) & 3, false);
    let l2 = lane_bytes((imm >> 4) & 3, true);
    let l3 = lane_bytes((imm >> 6) & 3, true);
    [
        l0[0], l0[1], l0[2], l0[3], l1[0], l1[1], l1[2], l1[3],
        l2[0], l2[1], l2[2], l2[3], l3[0], l3[1], l3[2], l3[3],
    ]
}

const fn shuffle_epi32_table(imm: i32) -> [u8; 16] {
    let l0 = lane_bytes(imm & 3, false);
    let l1 = lane_bytes((imm >> 2) & 3, false);
    let l2 = lane_bytes((imm >> 4) & 3, false);
    let l3 = lane_bytes((imm >> 6) & 3, false);
    [
        l0[0], l0[1], l0[2], l0[3], l1[0], l1[1], l1[2], l1[3],
        l2[0], l2[1], l2[2], l2[3], l3[0], l3[1], l3[2], l3[3],
    ]
}

/// Lanes 0 and 1 of the result from a, 2 and 3 from b, each named by two bits of IMM.
#[inline(always)]
pub unsafe fn _mm_shuffle_ps<const IMM: i32>(a: __m128, b: __m128) -> __m128 {
    let table = const { shuffle_ps_table(IMM) };
    let sources = uint8x16x2_t(vreinterpretq_u8_f32(fx(a)), vreinterpretq_u8_f32(fx(b)));
    from_f(vreinterpretq_f32_u8(vqtbl2q_u8(sources, vld1q_u8(table.as_ptr()))))
}

#[inline(always)]
pub unsafe fn _mm_shuffle_epi32<const IMM: i32>(a: __m128i) -> __m128i {
    let table = const { shuffle_epi32_table(IMM) };
    from_u8(vqtbl1q_u8(u8x(a), vld1q_u8(table.as_ptr())))
}

/// Byte i of the result is a's byte `mask[i] & 15`, or 0 when the mask byte's top
/// bit is set.
#[inline(always)]
pub unsafe fn _mm_shuffle_epi8(a: __m128i, mask: __m128i) -> __m128i {
    let index = vandq_u8(u8x(mask), vdupq_n_u8(0x8F));
    from_u8(vqtbl1q_u8(u8x(a), index))
}

const fn alignr_table(n: i32) -> [u8; 16] {
    let mut table = [0u8; 16];
    let mut i = 0;
    while i < 16 {
        let index = n + i as i32;
        table[i] = if index < 32 { index as u8 } else { 0xFF };
        i += 1;
    }
    table
}

/// The 32 bytes a:b shifted right by N bytes, the low 16 of them.
#[inline(always)]
pub unsafe fn _mm_alignr_epi8<const N: i32>(a: __m128i, b: __m128i) -> __m128i {
    let table = const { alignr_table(N) };
    let sources = uint8x16x2_t(u8x(b), u8x(a));
    from_u8(vqtbl2q_u8(sources, vld1q_u8(table.as_ptr())))
}

// ---- integer arithmetic ---------------------------------------------------------

#[inline(always)]
pub unsafe fn _mm_add_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_s8(vaddq_s8(s8x(a), s8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_add_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_s16(vaddq_s16(s16x(a), s16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_add_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_s32(vaddq_s32(s32x(a), s32x(b)))
}
#[inline(always)]
pub unsafe fn _mm_sub_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_s8(vsubq_s8(s8x(a), s8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_sub_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_s16(vsubq_s16(s16x(a), s16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_adds_epu8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vqaddq_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_adds_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_s16(vqaddq_s16(s16x(a), s16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_subs_epu8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vqsubq_u8(u8x(a), u8x(b)))
}
/// `(a + b + 1) >> 1` per unsigned byte.
#[inline(always)]
pub unsafe fn _mm_avg_epu8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vrhaddq_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_avg_epu16(a: __m128i, b: __m128i) -> __m128i {
    from_u16(vrhaddq_u16(u16x(a), u16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_min_epu32(a: __m128i, b: __m128i) -> __m128i {
    from_u32(vminq_u32(u32x(a), u32x(b)))
}
#[inline(always)]
pub unsafe fn _mm_max_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_s32(vmaxq_s32(s32x(a), s32x(b)))
}

#[inline(always)]
pub unsafe fn _mm_cmpeq_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vceqq_s8(s8x(a), s8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_cmpeq_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_u32(vceqq_s32(s32x(a), s32x(b)))
}
#[inline(always)]
pub unsafe fn _mm_cmpgt_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vcgtq_s8(s8x(a), s8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_cmpgt_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_u16(vcgtq_s16(s16x(a), s16x(b)))
}

// ---- shifts ----------------------------------------------------------------------
//
// A variable shift (ushl, sshl) takes its count from the low byte of each lane,
// signed, and a count of the lane's width or more gives 0 (or, right-shifting a
// signed lane, its sign): x86's rule for the immediate forms too. The counts here
// are at most 255, so the low byte holds them.

#[inline(always)]
pub unsafe fn _mm_slli_epi32<const N: i32>(a: __m128i) -> __m128i {
    from_u32(vshlq_u32(u32x(a), vdupq_n_s32(N.min(255))))
}
#[inline(always)]
pub unsafe fn _mm_srli_epi32<const N: i32>(a: __m128i) -> __m128i {
    from_u32(vshlq_u32(u32x(a), vdupq_n_s32(-N.min(255))))
}
#[inline(always)]
pub unsafe fn _mm_srai_epi32<const N: i32>(a: __m128i) -> __m128i {
    from_s32(vshlq_s32(s32x(a), vdupq_n_s32(-N.min(31))))
}
#[inline(always)]
pub unsafe fn _mm_slli_epi64<const N: i32>(a: __m128i) -> __m128i {
    from_u64(vshlq_u64(u64x(a), vdupq_n_s64(N.min(255) as i64)))
}
#[inline(always)]
pub unsafe fn _mm_srli_epi64<const N: i32>(a: __m128i) -> __m128i {
    from_u64(vshlq_u64(u64x(a), vdupq_n_s64(-(N.min(255) as i64))))
}

/// A logical right shift of both 64-bit lanes by the low 64 bits of `count`.
#[inline(always)]
pub unsafe fn _mm_srl_epi64(a: __m128i, count: __m128i) -> __m128i {
    let n = vgetq_lane_u64::<0>(u64x(count)).min(64) as i64;
    from_u64(vshlq_u64(u64x(a), vdupq_n_s64(-n)))
}

/// A byte shift right of the whole register, N from 0 to 15 (more gives 0).
#[inline(always)]
pub unsafe fn _mm_srli_si128<const N: i32>(a: __m128i) -> __m128i {
    _mm_alignr_epi8::<N>(_mm_setzero_si128(), a)
}

// ---- unpacking, packing, widening -----------------------------------------------

#[inline(always)]
pub unsafe fn _mm_unpacklo_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vzip1q_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpackhi_epi8(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vzip2q_u8(u8x(a), u8x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpacklo_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_u16(vzip1q_u16(u16x(a), u16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpackhi_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_u16(vzip2q_u16(u16x(a), u16x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpacklo_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_u32(vzip1q_u32(u32x(a), u32x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpackhi_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_u32(vzip2q_u32(u32x(a), u32x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpacklo_epi64(a: __m128i, b: __m128i) -> __m128i {
    from_u64(vzip1q_u64(u64x(a), u64x(b)))
}
#[inline(always)]
pub unsafe fn _mm_unpackhi_epi64(a: __m128i, b: __m128i) -> __m128i {
    from_u64(vzip2q_u64(u64x(a), u64x(b)))
}

/// Signed 16-bit lanes of a then b to unsigned bytes, saturating.
#[inline(always)]
pub unsafe fn _mm_packus_epi16(a: __m128i, b: __m128i) -> __m128i {
    from_u8(vcombine_u8(vqmovun_s16(s16x(a)), vqmovun_s16(s16x(b))))
}
/// Signed 32-bit lanes of a then b to signed 16-bit, saturating.
#[inline(always)]
pub unsafe fn _mm_packs_epi32(a: __m128i, b: __m128i) -> __m128i {
    from_s16(vcombine_s16(vqmovn_s32(s32x(a)), vqmovn_s32(s32x(b))))
}
/// The low eight bytes, sign-extended to 16 bits.
#[inline(always)]
pub unsafe fn _mm_cvtepi8_epi16(a: __m128i) -> __m128i {
    from_s16(vmovl_s8(vget_low_s8(s8x(a))))
}
/// The low four 16-bit lanes, sign-extended to 32 bits.
#[inline(always)]
pub unsafe fn _mm_cvtepi16_epi32(a: __m128i) -> __m128i {
    from_s32(vmovl_s16(vget_low_s16(s16x(a))))
}

// ---- conversion --------------------------------------------------------------------

#[inline(always)]
pub unsafe fn _mm_cvtepi32_ps(a: __m128i) -> __m128 {
    from_f(vcvtq_f32_s32(s32x(a)))
}

/// Truncates; a NaN or a value outside i32 gives 0x80000000, where NEON saturates
/// and gives 0 for a NaN.
#[inline(always)]
pub unsafe fn _mm_cvttps_epi32(a: __m128) -> __m128i {
    let x = fx(a);
    let in_range = vandq_u32(vcgeq_f32(x, vdupq_n_f32(-2147483648.0)), vcltq_f32(x, vdupq_n_f32(2147483648.0)));
    let truncated = vcvtq_s32_f32(x);
    from_s32(vbslq_s32(in_range, truncated, vdupq_n_s32(i32::MIN)))
}

// ---- masks -------------------------------------------------------------------------

/// Bit i is the top bit of f32 lane i.
#[inline(always)]
pub unsafe fn _mm_movemask_ps(a: __m128) -> i32 {
    let signs = vshrq_n_u32::<31>(vreinterpretq_u32_f32(fx(a)));
    let weights = vld1q_u32([1u32, 2, 4, 8].as_ptr());
    vaddvq_u32(vmulq_u32(signs, weights)) as i32
}

/// Bit i is the top bit of byte i.
#[inline(always)]
pub unsafe fn _mm_movemask_epi8(a: __m128i) -> i32 {
    let signs = vshrq_n_u8::<7>(u8x(a));
    let weights = vld1q_u8([1u8, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128].as_ptr());
    let weighted = vmulq_u8(signs, weights);
    let low = vaddv_u8(vget_low_u8(weighted)) as i32;
    let high = vaddv_u8(vget_high_u8(weighted)) as i32;
    low | (high << 8)
}

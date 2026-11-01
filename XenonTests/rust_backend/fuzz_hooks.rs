// title_fuzz.py: an indirect call is a recorded event, like every call out of the
// chunk under test.

unsafe extern "C" {
    fn stub_indirect(address: u32, ctx: &mut PPCContext);
}

#[inline(always)]
pub unsafe fn ppc_mm_store_u8(base: *mut u8, address: u32, value: u8) {
    st8(base, address, value);
}

#[inline(always)]
pub unsafe fn ppc_mm_store_u16(base: *mut u8, address: u32, value: u16) {
    st16(base, address, value);
}

#[inline(always)]
pub unsafe fn ppc_mm_store_u32(base: *mut u8, address: u32, value: u32) {
    st32(base, address, value);
}

#[inline(always)]
pub unsafe fn ppc_mm_store_u64(base: *mut u8, address: u32, value: u64) {
    st64(base, address, value);
}

#[inline(always)]
pub unsafe fn ppc_call_indirect(ctx: &mut PPCContext, _base: *mut u8, address: u32) {
    stub_indirect(address, ctx);
}

// The points a title may change, as a C++ build does by defining PPC_MM_STORE_*
// and PPC_CALL_INDIRECT_FUNC before ppc_context.h. A title's own file, named by
// rust_hooks_file in its TOML, replaces this one and must define all of these.

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
pub unsafe fn ppc_call_indirect(ctx: &mut PPCContext, base: *mut u8, address: u32) {
    ppc_lookup_func(base, address).unwrap_unchecked()(ctx, base);
}

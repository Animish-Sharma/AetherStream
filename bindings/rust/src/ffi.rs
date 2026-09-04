use std::ffi::c_char;

pub(crate) type AetherStatus = i32;

pub(crate) const AETHER_OK: AetherStatus = 0;
pub(crate) const AETHER_ERR_INVALID_ARG: AetherStatus = 1;
pub(crate) const AETHER_ERR_BUFFER_TOO_SMALL: AetherStatus = 2;
pub(crate) const AETHER_ERR_CORRUPTED_STREAM: AetherStatus = 3;
pub(crate) const AETHER_ERR_RATE_BUDGET_EXCEEDED: AetherStatus = 4;
pub(crate) const AETHER_ERR_DEPRECATED_FORMAT: AetherStatus = 5;
pub(crate) const AETHER_ERR_INTERNAL: AetherStatus = 6;

#[repr(C)]
pub(crate) struct AetherConfig {
    pub target_rate: f32,
    pub absolute_error_bound: f32,
    pub deadzone_factor: f32,
    pub enable_crc: u8,
    pub enable_index: u8,
}

extern "C" {
    pub(crate) fn aether_compress(
        input: *const f32,
        input_count: usize,
        config: *const AetherConfig,
        output: *mut u8,
        output_capacity: usize,
        bytes_written: *mut usize,
    ) -> AetherStatus;
    pub(crate) fn aether_decompress(
        input: *const u8,
        input_bytes: usize,
        output: *mut f32,
        output_capacity: usize,
        samples_written: *mut usize,
    ) -> AetherStatus;
    pub(crate) fn aether_decompress_slice(
        input: *const u8,
        input_bytes: usize,
        start_sample: usize,
        count: usize,
        output: *mut f32,
        output_capacity: usize,
        samples_written: *mut usize,
    ) -> AetherStatus;
    pub(crate) fn aether_c_abi_version() -> u32;
    pub(crate) fn aether_version_string() -> *const c_char;
}

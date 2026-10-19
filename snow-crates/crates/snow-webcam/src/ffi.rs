//! Shared opaque C handle. Consumers clone Source during configuration, never borrow its handle.
#![allow(clippy::missing_safety_doc)]
use crate::{CaptureOptions, Source};
use std::ffi::c_char;
use std::time::Instant;

pub type SnowWebcam = Source;
#[unsafe(no_mangle)]
pub extern "C" fn snow_webcam_create(size: u32, fps: u32) -> *mut SnowWebcam {
    Source::new(CaptureOptions { size, fps }).map_or(std::ptr::null_mut(), |source| {
        Box::into_raw(Box::new(source))
    })
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_webcam_configure(
    source: *const SnowWebcam,
    size: u32,
    fps: u32,
) -> u8 {
    unsafe { source.as_ref() }
        .is_some_and(|source| source.configure(CaptureOptions { size, fps }).is_ok())
        .into()
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_webcam_set_active(source: *const SnowWebcam, active: u8) {
    if let Some(source) = unsafe { source.as_ref() } {
        source.set_active(active != 0);
    }
}
/// Explicit user re-enable or a new recording session permits one new startup attempt.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_webcam_retry(source: *const SnowWebcam) {
    if let Some(source) = unsafe { source.as_ref() } {
        source.retry_failed();
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_webcam_destroy(source: *mut SnowWebcam) {
    if !source.is_null() {
        drop(unsafe { Box::from_raw(source) });
    }
}
#[unsafe(no_mangle)]
pub unsafe extern "C" fn snow_webcam_error(
    source: *const SnowWebcam,
    buffer: *mut c_char,
    capacity: usize,
) -> usize {
    let error = unsafe { source.as_ref() }
        .and_then(|source| source.snapshot(Instant::now()).error)
        .unwrap_or_default();
    if capacity > 0 && !buffer.is_null() {
        let count = error.len().min(capacity - 1);
        unsafe {
            std::ptr::copy_nonoverlapping(error.as_ptr(), buffer.cast(), count);
            *buffer.add(count) = 0;
        }
    }
    if error.is_empty() { 0 } else { error.len() + 1 }
}

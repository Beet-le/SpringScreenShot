use rayon::prelude::*;

use crate::{
    config::{RecImage, VisionBackend},
    error::{RapidOcrError, Result},
    vision::{
        backend::resolve_backend_strict,
        image_backend::resize_image,
        resize::{LinearResizeScratch, resize_bgr_inter_linear_into_with_scratch},
    },
};

#[derive(Debug, Clone)]
pub struct DetPreProcess {
    pub limit_side_len: usize,
    pub limit_type: String,
    pub mean: [f32; 3],
    pub std: [f32; 3],
    pub vision_backend: VisionBackend,
}

impl Default for DetPreProcess {
    fn default() -> Self {
        Self {
            limit_side_len: 736,
            limit_type: "min".to_string(),
            mean: [0.5, 0.5, 0.5],
            std: [0.5, 0.5, 0.5],
            vision_backend: VisionBackend::PureRust,
        }
    }
}

#[derive(Debug, Default)]
pub(crate) struct DetPreprocessScratch {
    pub tmp_bgr: Vec<u8>,
    pub resize: LinearResizeScratch,
}

impl DetPreProcess {
    pub(crate) fn run_into_buffer_with_scratch(
        &self,
        img: &RecImage,
        buffer: &mut Vec<f32>,
        scratch: &mut DetPreprocessScratch,
        limit_side_len: Option<usize>,
    ) -> Result<(usize, usize)> {
        let (resize_h, resize_w) =
            self.compute_resize_hw(img, limit_side_len.unwrap_or(self.limit_side_len))?;
        let expected_len = 3usize
            .checked_mul(resize_h)
            .and_then(|v| v.checked_mul(resize_w))
            .ok_or_else(|| {
                RapidOcrError::InvalidInput("det output buffer size overflow".to_string())
            })?;
        buffer.resize(expected_len, 0.0);

        let backend = resolve_backend_strict(self.vision_backend)?;
        match backend {
            VisionBackend::PureRust => {
                let src = img.as_bgr_cow();
                resize_bgr_inter_linear_into_with_scratch(
                    src.as_ref(),
                    img.width(),
                    img.height(),
                    resize_w,
                    resize_h,
                    &mut scratch.tmp_bgr,
                    &mut scratch.resize,
                )?;
                self.normalize_bgr_into_slice(
                    &scratch.tmp_bgr,
                    resize_w,
                    resize_h,
                    &mut buffer[..expected_len],
                )?;
            }
            VisionBackend::OpenCv => {
                let resized = resize_image(img, resize_w, resize_h, backend)?;
                self.normalize_resized_into_slice(&resized, &mut buffer[..expected_len])?;
            }
        }

        Ok((resize_h, resize_w))
    }

    fn compute_resize_hw(&self, img: &RecImage, limit_side_len: usize) -> Result<(usize, usize)> {
        let h = img.height();
        let w = img.width();
        if h == 0 || w == 0 {
            return Err(RapidOcrError::InvalidImage(
                "detector image width/height cannot be zero".to_string(),
            ));
        }

        let ratio = if self.limit_type == "max" {
            if h.max(w) > limit_side_len {
                limit_side_len as f32 / h.max(w) as f32
            } else {
                1.0
            }
        } else if h.min(w) < limit_side_len {
            limit_side_len as f32 / h.min(w) as f32
        } else {
            1.0
        };

        // Keep parity with Python: int(h * ratio), int(w * ratio), then round to 32-multiples.
        let mut resize_h = ((h as f32 * ratio) as usize).max(1);
        let mut resize_w = ((w as f32 * ratio) as usize).max(1);
        resize_h = ((resize_h as f32 / 32.0).round_ties_even() as usize * 32).max(32);
        resize_w = ((resize_w as f32 / 32.0).round_ties_even() as usize * 32).max(32);
        Ok((resize_h, resize_w))
    }

    fn normalize_resized_into_slice(
        &self,
        resized: &RecImage,
        out_slice: &mut [f32],
    ) -> Result<()> {
        let src = resized.as_bgr_cow();
        self.normalize_bgr_into_slice(src.as_ref(), resized.width(), resized.height(), out_slice)
    }

    fn normalize_bgr_into_slice(
        &self,
        src: &[u8],
        width: usize,
        height: usize,
        out_slice: &mut [f32],
    ) -> Result<()> {
        let expected_len = 3usize
            .checked_mul(height)
            .and_then(|v| v.checked_mul(width))
            .ok_or_else(|| {
                RapidOcrError::InvalidInput("det output buffer size overflow".to_string())
            })?;
        let expected_src_len = expected_len;
        if src.len() != expected_src_len {
            return Err(RapidOcrError::InvalidInput(format!(
                "det source BGR size mismatch: expected {expected_src_len}, got {}",
                src.len()
            )));
        }
        if out_slice.len() != expected_len {
            return Err(RapidOcrError::InvalidInput(format!(
                "det output buffer size mismatch: expected {expected_len}, got {}",
                out_slice.len()
            )));
        }

        let plane_stride = width * height;
        let row_src_stride = width * 3;
        let row_parallel = should_parallelize_rows(width, height);
        let norm_mul = [
            (1.0 / 255.0) / self.std[0],
            (1.0 / 255.0) / self.std[1],
            (1.0 / 255.0) / self.std[2],
        ];
        let norm_add = [
            -self.mean[0] / self.std[0],
            -self.mean[1] / self.std[1],
            -self.mean[2] / self.std[2],
        ];
        #[cfg(target_arch = "x86_64")]
        let use_avx2 = std::arch::is_x86_feature_detected!("avx2");

        if row_parallel {
            let out_addr = out_slice.as_mut_ptr() as usize;
            let src_addr = src.as_ptr() as usize;
            (0..height).into_par_iter().for_each(|y| {
                let out_ptr = out_addr as *mut f32;
                let src_ptr = src_addr as *const u8;
                // Safety:
                // - `y` is unique per parallel iteration, so each worker writes disjoint row ranges.
                // - `out_ptr` points to a contiguous CHW buffer of size `3 * width * height`.
                // - `src_ptr` points to a contiguous BGR buffer of size `3 * width * height`.
                unsafe {
                    let row_ptr = src_ptr.add(y * row_src_stride);
                    #[cfg(target_arch = "x86_64")]
                    if use_avx2 {
                        write_normalized_row_avx2(
                            row_ptr,
                            out_ptr,
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    } else {
                        write_normalized_row_scalar(
                            row_ptr,
                            out_ptr,
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    }
                    #[cfg(target_arch = "aarch64")]
                    write_normalized_row_neon_dispatch(
                        row_ptr,
                        out_ptr,
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
                    write_normalized_row_scalar(
                        row_ptr,
                        out_ptr,
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                }
            });
        } else {
            for y in 0..height {
                // Safety:
                // - `y` stays within `[0, height)`.
                // - Source and destination pointers are derived from validated contiguous buffers.
                unsafe {
                    let row_ptr = src.as_ptr().add(y * row_src_stride);
                    #[cfg(target_arch = "x86_64")]
                    if use_avx2 {
                        write_normalized_row_avx2(
                            row_ptr,
                            out_slice.as_mut_ptr(),
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    } else {
                        write_normalized_row_scalar(
                            row_ptr,
                            out_slice.as_mut_ptr(),
                            y,
                            width,
                            plane_stride,
                            norm_mul,
                            norm_add,
                        );
                    }
                    #[cfg(target_arch = "aarch64")]
                    write_normalized_row_neon_dispatch(
                        row_ptr,
                        out_slice.as_mut_ptr(),
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                    #[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
                    write_normalized_row_scalar(
                        row_ptr,
                        out_slice.as_mut_ptr(),
                        y,
                        width,
                        plane_stride,
                        norm_mul,
                        norm_add,
                    );
                }
            }
        }
        Ok(())
    }
}

#[inline]
fn should_parallelize_rows(width: usize, height: usize) -> bool {
    width
        .checked_mul(height)
        .is_some_and(|v| v >= 512 * 512 && rayon::current_num_threads() > 1)
}

#[cfg(any(test, not(target_arch = "aarch64")))]
#[inline]
unsafe fn write_normalized_row_scalar(
    src_row_ptr: *const u8,
    out_ptr: *mut f32,
    y: usize,
    width: usize,
    plane_stride: usize,
    norm_mul: [f32; 3],
    norm_add: [f32; 3],
) {
    // Safety:
    // - Caller guarantees all pointers are valid for the computed row range.
    // - The row writes target disjoint regions when used in parallel.
    unsafe {
        let mut src_px = src_row_ptr;
        let row_base = y * width;
        for x in 0..width {
            let row_offset = row_base + x;
            let b = (*src_px) as f32 * norm_mul[0] + norm_add[0];
            let g = (*src_px.add(1)) as f32 * norm_mul[1] + norm_add[1];
            let r = (*src_px.add(2)) as f32 * norm_mul[2] + norm_add[2];

            *out_ptr.add(row_offset) = b;
            *out_ptr.add(plane_stride + row_offset) = g;
            *out_ptr.add(plane_stride * 2 + row_offset) = r;
            src_px = src_px.add(3);
        }
    }
}

#[cfg(target_arch = "x86_64")]
#[target_feature(enable = "avx2")]
unsafe fn write_normalized_row_avx2(
    src_row_ptr: *const u8,
    out_ptr: *mut f32,
    y: usize,
    width: usize,
    plane_stride: usize,
    norm_mul: [f32; 3],
    norm_add: [f32; 3],
) {
    use std::arch::x86_64::{
        __m256, __m256i, _mm256_add_ps, _mm256_cvtepi32_ps, _mm256_mul_ps, _mm256_set1_ps,
        _mm256_setr_epi32, _mm256_storeu_ps,
    };

    let row_base = y * width;
    let mut x = 0usize;
    let b_mul: __m256 = _mm256_set1_ps(norm_mul[0]);
    let g_mul: __m256 = _mm256_set1_ps(norm_mul[1]);
    let r_mul: __m256 = _mm256_set1_ps(norm_mul[2]);
    let b_add: __m256 = _mm256_set1_ps(norm_add[0]);
    let g_add: __m256 = _mm256_set1_ps(norm_add[1]);
    let r_add: __m256 = _mm256_set1_ps(norm_add[2]);

    while x + 8 <= width {
        // De-interleave 8 BGR pixels into channel vectors.
        let mut b = [0_i32; 8];
        let mut g = [0_i32; 8];
        let mut r = [0_i32; 8];
        for lane in 0..8 {
            // Safety:
            // - Caller guarantees `src_row_ptr` is valid for the current row.
            // - `x + lane < width` and each pixel has 3 channels.
            unsafe {
                let p = src_row_ptr.add((x + lane) * 3);
                b[lane] = *p as i32;
                g[lane] = *p.add(1) as i32;
                r[lane] = *p.add(2) as i32;
            }
        }

        let b_i32: __m256i = _mm256_setr_epi32(b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7]);
        let g_i32: __m256i = _mm256_setr_epi32(g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7]);
        let r_i32: __m256i = _mm256_setr_epi32(r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);

        let b_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(b_i32), b_mul), b_add);
        let g_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(g_i32), g_mul), g_add);
        let r_f32 = _mm256_add_ps(_mm256_mul_ps(_mm256_cvtepi32_ps(r_i32), r_mul), r_add);

        let row_offset = row_base + x;
        // Safety:
        // - Destination pointer is a contiguous CHW buffer sized for the full image.
        // - Row offsets are in-bounds for this row and channel planes.
        unsafe {
            _mm256_storeu_ps(out_ptr.add(row_offset), b_f32);
            _mm256_storeu_ps(out_ptr.add(plane_stride + row_offset), g_f32);
            _mm256_storeu_ps(out_ptr.add(plane_stride * 2 + row_offset), r_f32);
        }
        x += 8;
    }

    if x < width {
        // Safety:
        // - Tail starts within current source row; each iteration advances by one pixel.
        let mut src_px = unsafe { src_row_ptr.add(x * 3) };
        for px in x..width {
            let row_offset = row_base + px;
            // Safety:
            // - Source and destination pointers remain in-bounds for the tail range.
            unsafe {
                let b = (*src_px) as f32 * norm_mul[0] + norm_add[0];
                let g = (*src_px.add(1)) as f32 * norm_mul[1] + norm_add[1];
                let r = (*src_px.add(2)) as f32 * norm_mul[2] + norm_add[2];
                *out_ptr.add(row_offset) = b;
                *out_ptr.add(plane_stride + row_offset) = g;
                *out_ptr.add(plane_stride * 2 + row_offset) = r;
                src_px = src_px.add(3);
            }
        }
    }
}

#[cfg(all(test, target_arch = "aarch64"))]
static NEON_NORMALIZE_PIXELS: std::sync::atomic::AtomicUsize =
    std::sync::atomic::AtomicUsize::new(0);

// NEON is part of the ARM64 baseline. Structured SIMD loads deinterleave exactly sixteen
// BGR pixels without reading padding beyond the validated row.
#[cfg(target_arch = "aarch64")]
#[target_feature(enable = "neon")]
#[inline]
unsafe fn write_normalized_row_neon(
    src_row_ptr: *const u8,
    out_ptr: *mut f32,
    y: usize,
    width: usize,
    plane_stride: usize,
    norm_mul: [f32; 3],
    norm_add: [f32; 3],
) -> usize {
    unsafe {
        let row_base = y * width;
        let mut x = 0;
        while x + 32 <= width {
            normalize_sixteen_neon(
                src_row_ptr.add(x * 3),
                out_ptr.add(row_base + x),
                plane_stride,
                norm_mul,
                norm_add,
            );
            normalize_sixteen_neon(
                src_row_ptr.add((x + 16) * 3),
                out_ptr.add(row_base + x + 16),
                plane_stride,
                norm_mul,
                norm_add,
            );
            x += 32;
        }
        if x + 16 <= width {
            normalize_sixteen_neon(
                src_row_ptr.add(x * 3),
                out_ptr.add(row_base + x),
                plane_stride,
                norm_mul,
                norm_add,
            );
            x += 16;
        }
        for px in x..width {
            for channel in 0..3 {
                *out_ptr.add(channel * plane_stride + row_base + px) =
                    f32::from(*src_row_ptr.add(px * 3 + channel)) * norm_mul[channel]
                        + norm_add[channel];
            }
        }
        x
    }
}

#[cfg(target_arch = "aarch64")]
#[inline(always)]
unsafe fn normalize_sixteen_neon(
    src: *const u8,
    out: *mut f32,
    stride: usize,
    mul: [f32; 3],
    add: [f32; 3],
) {
    use std::arch::aarch64::*;
    unsafe {
        let packed = vld3q_u8(src);
        // A table lookup widens four bytes directly into four u32 lanes.
        // This needs fewer shuffle instructions than two-stage u8→u16→u32 widening.
        let indices = vld1q_u8(
            [
                0_u8, 128, 128, 128, 1, 128, 128, 128, 2, 128, 128, 128, 3, 128, 128, 128,
            ]
            .as_ptr(),
        );
        for (channel, bytes) in [packed.0, packed.1, packed.2].into_iter().enumerate() {
            let multiplier = vdupq_n_f32(mul[channel]);
            let addition = vdupq_n_f32(add[channel]);
            for quarter in 0..4 {
                let lookup = vaddq_u8(indices, vdupq_n_u8(quarter * 4));
                let values = vreinterpretq_u32_u8(vqtbl1q_u8(bytes, lookup));
                let value = vaddq_f32(vmulq_f32(vcvtq_f32_u32(values), multiplier), addition);
                vst1q_f32(out.add(channel * stride + usize::from(quarter) * 4), value);
            }
        }
    }
}

#[cfg(target_arch = "aarch64")]
#[inline]
unsafe fn write_normalized_row_neon_dispatch(
    src: *const u8,
    out: *mut f32,
    y: usize,
    width: usize,
    stride: usize,
    mul: [f32; 3],
    add: [f32; 3],
) {
    let _pixels = unsafe { write_normalized_row_neon(src, out, y, width, stride, mul, add) };
    #[cfg(test)]
    NEON_NORMALIZE_PIXELS.fetch_add(_pixels, std::sync::atomic::Ordering::Relaxed);
}

#[cfg(all(test, target_arch = "aarch64"))]
mod neon_tests {
    use super::*;

    #[test]
    fn neon_normalization_executes_for_unaligned_rows_tails_and_rounding() {
        assert!(std::arch::is_aarch64_feature_detected!("neon"));
        let mul = [0.017124753, 0.017507004, 0.017429193];
        let add = [-2.117904, -2.0357141, -1.8044444];
        let mut vector_pixels = 0;
        for width in 0..66 {
            let storage: Vec<u8> = (0..width * 3 + 1).map(|i| (i * 73 + i / 5) as u8).collect();
            let source = &storage[1..];
            let stride = width * 3;
            let mut expected = vec![123.0; stride * 3 + 1];
            let mut actual = expected.clone();
            unsafe {
                write_normalized_row_scalar(
                    source.as_ptr(),
                    expected.as_mut_ptr().add(1),
                    1,
                    width,
                    stride,
                    mul,
                    add,
                );
                vector_pixels += write_normalized_row_neon(
                    source.as_ptr(),
                    actual.as_mut_ptr().add(1),
                    1,
                    width,
                    stride,
                    mul,
                    add,
                );
            }
            assert_eq!(
                actual.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
                expected.iter().map(|v| v.to_bits()).collect::<Vec<_>>(),
                "width {width}"
            );
        }
        assert!(
            vector_pixels > 0,
            "the direct NEON calls must execute full vector blocks"
        );
    }
    #[test]
    fn normalization_dispatch_executes_neon_for_serial_and_parallel_rows() {
        let preprocess = DetPreProcess::default();
        for (width, height) in [(17, 3), (513, 512)] {
            let source: Vec<u8> = (0..width * height * 3)
                .map(|i| (i * 73 + i / 5) as u8)
                .collect();
            let mut expected = vec![0.0; width * height * 3];
            let mut actual = expected.clone();
            for y in 0..height {
                unsafe {
                    write_normalized_row_scalar(
                        source.as_ptr().add(y * width * 3),
                        expected.as_mut_ptr(),
                        y,
                        width,
                        width * height,
                        preprocess.std.map(|v| (1.0 / 255.0) / v),
                        std::array::from_fn(|i| -preprocess.mean[i] / preprocess.std[i]),
                    )
                };
            }
            let before = NEON_NORMALIZE_PIXELS.load(std::sync::atomic::Ordering::Relaxed);
            preprocess
                .normalize_bgr_into_slice(&source, width, height, &mut actual)
                .unwrap();
            assert!(NEON_NORMALIZE_PIXELS.load(std::sync::atomic::Ordering::Relaxed) > before);
            assert_eq!(actual, expected);
        }
    }
    #[test]
    #[ignore = "paired Release-only ARM64 kernel measurement"]
    fn neon_release_benchmark_normalization() {
        use std::hint::black_box;
        let width = 1280;
        let height = 720;
        let stride = width * height;
        let source: Vec<u8> = (0..stride * 3)
            .map(|i| (i * 73 + i / 11 * 29) as u8)
            .collect();
        let mut scalar_output = vec![0.0; stride * 3];
        let mut neon_output = scalar_output.clone();
        crate::neon_benchmark::paired(
            "ocr_normalized_chw",
            8,
            || {
                for y in 0..height {
                    unsafe {
                        write_normalized_row_scalar(
                            black_box(source.as_ptr()).add(y * width * 3),
                            scalar_output.as_mut_ptr(),
                            y,
                            width,
                            stride,
                            [0.007843138; 3],
                            [-1.0; 3],
                        )
                    };
                }
                black_box(&scalar_output);
                scalar_output[stride / 2].to_bits()
            },
            || {
                for y in 0..height {
                    unsafe {
                        write_normalized_row_neon(
                            black_box(source.as_ptr()).add(y * width * 3),
                            neon_output.as_mut_ptr(),
                            y,
                            width,
                            stride,
                            [0.007843138; 3],
                            [-1.0; 3],
                        )
                    };
                }
                black_box(&neon_output);
                neon_output[stride / 2].to_bits()
            },
        );
        assert_eq!(scalar_output, neon_output);
    }
}

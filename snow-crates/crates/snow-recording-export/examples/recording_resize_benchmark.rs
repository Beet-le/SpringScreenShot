//! Compare recording filtering with nearest-pixel selection in performance Release.
//! --quality --output <directory> writes PPM previews and independent reference errors.
use snow_recording_export::resize::{NearestResizePlan, RecordingResizePlan};
use std::{
    fs,
    hint::black_box,
    io::{self, Write},
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

struct Options {
    quality: bool,
    pairs: usize,
    measurement: Duration,
    output: Option<PathBuf>,
}

fn options() -> Options {
    let mut options = Options {
        quality: false,
        pairs: 5,
        measurement: Duration::from_millis(500),
        output: None,
    };
    let mut args = std::env::args().skip(1);
    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--quality" => options.quality = true,
            "--pairs" => {
                options.pairs = args.next().expect("--pairs value").parse().expect("pairs");
                assert!(options.pairs > 0);
            }
            "--milliseconds" => {
                let milliseconds = args
                    .next()
                    .expect("--milliseconds value")
                    .parse()
                    .expect("milliseconds");
                assert!(milliseconds > 0);
                options.measurement = Duration::from_millis(milliseconds);
            }
            "--output" => options.output = Some(args.next().expect("--output value").into()),
            _ => panic!("unknown argument: {argument}"),
        }
    }
    options
}

fn main() {
    if cfg!(debug_assertions) {
        panic!("use the performance Release build");
    }
    let options = options();
    if let Some(output) = options.output.as_ref() {
        fs::create_dir_all(output).expect("report directory");
    }
    let mut report: Box<dyn Write> = options.output.as_ref().map_or_else(
        || Box::new(io::stdout()) as Box<dyn Write>,
        |output| {
            Box::new(
                fs::File::create(output.join(if options.quality {
                    "resize-quality.csv"
                } else {
                    "resize-performance.csv"
                }))
                .expect("CSV report"),
            )
        },
    );
    if options.quality {
        quality(&mut report, options.output.as_deref());
    } else {
        performance(&mut report, &options);
    }
}

fn performance(report: &mut dyn Write, options: &Options) {
    let pools: Vec<_> = [2, 4]
        .into_iter()
        .map(|threads| {
            rayon::ThreadPoolBuilder::new()
                .num_threads(threads)
                .build()
                .expect("bounded resize workers")
        })
        .collect();
    writeln!(
        report,
        "pair,variant,source_width,source_height,width,height,iterations,nanoseconds_per_resize"
    )
    .unwrap();
    for (source_size, output_size) in [
        ((3840, 2160), (1920, 1080)),
        ((2160, 3840), (1080, 1920)),
        ((2561, 1441), (1920, 1080)),
        ((3840, 2160), (1280, 720)),
        ((1919, 1079), (959, 539)),
        ((1920, 1080), (1920, 1080)),
    ] {
        let source = fixture(source_size);
        let nearest =
            NearestResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1);
        let filtered =
            RecordingResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1);
        let mut output = vec![0; output_size.0 as usize * output_size.1 as usize * 4];
        let mut serial = output.clone();
        filtered.resize_into(&source, &mut serial);
        for pool in &pools {
            filtered.resize_into_with_pool(&source, &mut output, pool);
            assert_eq!(serial, output, "worker count must preserve the image");
        }
        if source_size == output_size {
            assert_eq!(
                source, serial,
                "native recording must preserve exact pixels"
            );
        }
        for pair in 0..options.pairs {
            // Reverse alternate pairs and rotate the first variant to spread
            // cache/thermal drift across variants instead of favoring one.
            let mut variants = [0, 1, 2, 3];
            variants.rotate_left(pair % 4);
            if pair % 2 == 1 {
                variants.reverse();
            }
            for variant in variants {
                let resize = |output: &mut [u8]| match variant {
                    0 => nearest.resize_into(black_box(&source), output),
                    1 => filtered.resize_into(black_box(&source), output),
                    _ => filtered.resize_into_with_pool(
                        black_box(&source),
                        output,
                        &pools[variant - 2],
                    ),
                };
                let warmup = Instant::now();
                while warmup.elapsed() < Duration::from_millis(50) {
                    resize(black_box(&mut output));
                    black_box(&output);
                }
                let started = Instant::now();
                let mut iterations = 0;
                while started.elapsed() < options.measurement {
                    resize(black_box(&mut output));
                    black_box(&output);
                    iterations += 1;
                }
                writeln!(
                    report,
                    "{},{},{},{},{},{},{},{}",
                    pair + 1,
                    ["nearest", "area_serial", "area_parallel2", "area_parallel4"][variant],
                    source_size.0,
                    source_size.1,
                    output_size.0,
                    output_size.1,
                    iterations,
                    started.elapsed().as_nanos() / iterations,
                )
                .unwrap();
                report.flush().unwrap();
            }
        }
    }
}

fn fixture(size: (u32, u32)) -> Vec<u8> {
    (0..size.0 * size.1)
        .flat_map(|pixel| {
            let (x, y) = (pixel % size.0, pixel / size.0);
            // Thin grid lines, diagonal strokes and checkerboard patches are
            // screen details which nearest selection can discard.
            let value = if y < size.1 / 3 {
                if x % 7 == 1 || y % 11 == 1 { 24 } else { 232 }
            } else if y < size.1 * 2 / 3 {
                if (x + y) % 13 < 2 { 24 } else { 232 }
            } else if (x + y) % 2 == 0 {
                24
            } else {
                232
            };
            [value, value, value, 255]
        })
        .collect()
}

fn quality(report: &mut dyn Write, output: Option<&Path>) {
    writeln!(
        report,
        "variant,source_width,source_height,width,height,mean_absolute_rgb_error,max_rgb_error"
    )
    .unwrap();
    for (source_size, output_size) in [
        ((384, 216), (192, 108)),
        ((216, 384), (108, 192)),
        ((257, 145), (192, 108)),
        ((384, 216), (128, 72)),
        ((257, 108), (192, 108)),
        ((384, 54), (192, 108)),
        ((192, 108), (192, 108)),
        ((96, 54), (192, 108)),
    ] {
        let source = fixture(source_size);
        let reference = reference(&source, source_size, output_size);
        let nearest =
            NearestResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1);
        let filtered =
            RecordingResizePlan::new(source_size.0, source_size.1, output_size.0, output_size.1);
        let mut resized = reference.clone();
        let stem = format!(
            "{}x{}-{}x{}",
            source_size.0, source_size.1, output_size.0, output_size.1
        );
        if let Some(output) = output {
            ppm(
                &output.join(format!("{stem}-source.ppm")),
                source_size,
                &source,
            );
            ppm(
                &output.join(format!("{stem}-reference.ppm")),
                output_size,
                &reference,
            );
        }
        for variant in ["nearest", "area"] {
            match variant {
                "nearest" => nearest.resize_into(&source, &mut resized),
                _ => filtered.resize_into(&source, &mut resized),
            }
            let (sum, maximum) = resized
                .chunks_exact(4)
                .zip(reference.chunks_exact(4))
                .flat_map(|(actual, expected)| {
                    (0..3).map(move |channel| actual[channel].abs_diff(expected[channel]))
                })
                .fold((0u64, 0u8), |(sum, maximum), error| {
                    (sum + u64::from(error), maximum.max(error))
                });
            writeln!(
                report,
                "{variant},{},{},{},{},{:.6},{maximum}",
                source_size.0,
                source_size.1,
                output_size.0,
                output_size.1,
                sum as f64 / (output_size.0 as f64 * output_size.1 as f64 * 3.0),
            )
            .unwrap();
            if source_size.0 <= output_size.0 && source_size.1 <= output_size.1 {
                assert_eq!(resized, reference, "native/upscale parity");
            }
            if let Some(output) = output {
                ppm(
                    &output.join(format!("{stem}-{variant}.ppm")),
                    output_size,
                    &resized,
                );
            }
        }
    }
}

/// Independent continuous pixel-square intersections; no fixed-point weights,
/// cached plan, SIMD or row workers.
fn reference(source: &[u8], input: (u32, u32), output: (u32, u32)) -> Vec<u8> {
    let mut pixels = Vec::with_capacity(output.0 as usize * output.1 as usize * 4);
    if input.0 <= output.0 && input.1 <= output.1 {
        for y in 0..output.1 {
            for x in 0..output.0 {
                let sx = u64::from(x) * u64::from(input.0) / u64::from(output.0);
                let sy = u64::from(y) * u64::from(input.1) / u64::from(output.1);
                let offset = (sy * u64::from(input.0) + sx) as usize * 4;
                pixels.extend_from_slice(&source[offset..offset + 4]);
            }
        }
        return pixels;
    }
    let scale = (
        input.0 as f64 / output.0 as f64,
        input.1 as f64 / output.1 as f64,
    );
    let interval = |position: u32, source: u32, destination: u32, scale: f64| {
        if source > destination {
            (position as f64 * scale, (position + 1) as f64 * scale)
        } else {
            let selected = u64::from(position) * u64::from(source) / u64::from(destination);
            (selected as f64, (selected + 1) as f64)
        }
    };
    for y in 0..output.1 {
        let (top, bottom) = interval(y, input.1, output.1, scale.1);
        for x in 0..output.0 {
            let (left, right) = interval(x, input.0, output.0, scale.0);
            let mut sum = [0.0; 4];
            for sy in top.floor() as u32..(bottom.ceil() as u32).min(input.1) {
                let height = bottom.min((sy + 1) as f64) - top.max(sy as f64);
                for sx in left.floor() as u32..(right.ceil() as u32).min(input.0) {
                    let width = right.min((sx + 1) as f64) - left.max(sx as f64);
                    let offset = (sy as usize * input.0 as usize + sx as usize) * 4;
                    for channel in 0..4 {
                        sum[channel] += source[offset + channel] as f64 * width * height;
                    }
                }
            }
            pixels
                .extend(sum.map(|value| (value / ((right - left) * (bottom - top))).round() as u8));
        }
    }
    pixels
}

fn ppm(path: &Path, size: (u32, u32), pixels: &[u8]) {
    let mut file = fs::File::create(path).expect("PPM preview");
    writeln!(file, "P6\n{} {}\n255", size.0, size.1).unwrap();
    for pixel in pixels.chunks_exact(4) {
        file.write_all(&pixel[..3]).unwrap();
    }
}

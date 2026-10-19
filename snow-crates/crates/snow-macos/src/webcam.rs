//! AVFoundation video-only capture. Session control and conversion stay on its owner worker.
use block2::RcBlock;
use crossbeam_channel::{Receiver, Sender};
use dispatch2::{DispatchQueue, DispatchRetained};
use objc2::{
    AnyThread, DefinedClass, define_class, msg_send, rc::Retained, runtime::ProtocolObject,
};
use objc2_av_foundation::*;
use objc2_core_media::{
    CMSampleBuffer, CMTime, CMTimeFlags, CMVideoFormatDescriptionGetDimensions,
};
use objc2_foundation::{NSBundle, NSDictionary, NSNumber, NSObject, NSObjectProtocol, NSString};
use snow_media::{ColorDescription, PixelFormat, macos::PixelBuffer};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::{Duration, Instant},
};

struct State {
    frames: Sender<(PixelBuffer, Instant)>,
    closed: AtomicBool,
}
define_class!(
    #[unsafe(super(NSObject))]
    #[name = "SnowWebcamOutput"]
    #[ivars = Arc<State>]
    struct Output;
    unsafe impl NSObjectProtocol for Output {}
    unsafe impl AVCaptureVideoDataOutputSampleBufferDelegate for Output {
        #[unsafe(method(captureOutput:didOutputSampleBuffer:fromConnection:))]
        unsafe fn receive(&self, _output: &AVCaptureOutput, sample: &CMSampleBuffer, _connection: &AVCaptureConnection) {
            let state = self.ivars();
            if state.closed.load(Ordering::Acquire) || state.frames.is_full() { return; }
            let at = Instant::now();
            if let Some(image) = unsafe { sample.image_buffer() }
                && let Ok(image) = unsafe { PixelBuffer::from_retained(image, ColorDescription::SRGB) } {
                let _ = state.frames.try_send((image, at));
            }
        }
    }
);

/// None means that the OS has not yet asked the user.
pub fn authorization() -> Option<bool> {
    let status =
        unsafe { AVCaptureDevice::authorizationStatusForMediaType(AVMediaTypeVideo.unwrap()) };
    if status == AVAuthorizationStatus::NotDetermined {
        None
    } else {
        Some(status == AVAuthorizationStatus::Authorized)
    }
}
pub fn request_access() -> Receiver<bool> {
    let (sender, receiver) = crossbeam_channel::bounded(1);
    // Command-line hosts and tests may not carry an app privacy manifest. macOS would
    // terminate that process if we requested camera permission without this key.
    if NSBundle::mainBundle()
        .objectForInfoDictionaryKey(&NSString::from_str("NSCameraUsageDescription"))
        .is_none()
    {
        let _ = sender.try_send(false);
        return receiver;
    }
    let block = RcBlock::new(move |granted: objc2::runtime::Bool| {
        let _ = sender.try_send(granted.as_bool());
    });
    unsafe {
        AVCaptureDevice::requestAccessForMediaType_completionHandler(
            AVMediaTypeVideo.unwrap(),
            &block,
        );
    }
    receiver
}

pub struct Frame {
    pub width: u32,
    pub height: u32,
    pub rgba: Vec<u8>,
    pub stride: usize,
    pub at: Instant,
}
pub struct Stream {
    session: Retained<AVCaptureSession>,
    output: Retained<AVCaptureVideoDataOutput>,
    _delegate: Retained<Output>,
    _queue: DispatchRetained<DispatchQueue>,
    state: Arc<State>,
    frames: Receiver<(PixelBuffer, Instant)>,
}
impl Stream {
    pub fn open(size: u32, fps: u32) -> Result<Self, String> {
        objc2::rc::autoreleasepool(|_| unsafe {
            if authorization() != Some(true) {
                return Err(
                    "Camera access was denied. Enable camera access in System Settings.".into(),
                );
            }
            let media = AVMediaTypeVideo.unwrap();
            #[allow(deprecated)]
            let device = AVCaptureDevice::defaultDeviceWithMediaType(media)
                .or_else(|| AVCaptureDevice::devicesWithMediaType(media).firstObject())
                .ok_or("No camera is connected.")?;
            let session = AVCaptureSession::new();
            let input = AVCaptureDeviceInput::deviceInputWithDevice_error(&device)
                .map_err(|e| e.to_string())?;
            let output = AVCaptureVideoDataOutput::new();
            if !session.canAddInput(&input) || !session.canAddOutput(&output) {
                return Err("The camera is unavailable or in use.".into());
            }
            session.addInput(&input);
            session.addOutput(&output);
            let format = device
                .formats()
                .iter()
                .filter_map(|format| {
                    let dimensions =
                        CMVideoFormatDescriptionGetDimensions(&format.formatDescription());
                    if dimensions.width <= 0
                        || dimensions.height <= 0
                        || dimensions.width > 8192
                        || dimensions.height > 8192
                    {
                        return None;
                    }
                    let rate = format
                        .videoSupportedFrameRateRanges()
                        .iter()
                        .find(|range| {
                            range.minFrameRate() <= f64::from(fps)
                                && range.maxFrameRate() >= f64::from(fps)
                        })?;
                    let _ = rate;
                    let minimum = dimensions.width.min(dimensions.height) as u32;
                    Some((
                        (
                            minimum < size,
                            u64::from(dimensions.width as u32)
                                * u64::from(dimensions.height as u32),
                        ),
                        format,
                    ))
                })
                .min_by_key(|(score, _)| *score)
                .map(|(_, format)| format);
            if let Some(format) = format {
                device.lockForConfiguration().map_err(|e| e.to_string())?;
                device.setActiveFormat(&format);
                let interval = CMTime {
                    value: 1,
                    timescale: fps as i32,
                    flags: CMTimeFlags::Valid,
                    epoch: 0,
                };
                device.setActiveVideoMinFrameDuration(interval);
                device.setActiveVideoMaxFrameDuration(interval);
                device.unlockForConfiguration();
            }
            let settings = NSDictionary::from_slices(
                &[&*NSString::from_str("PixelFormatType")],
                &[
                    &*NSNumber::new_u32(objc2_core_video::kCVPixelFormatType_32BGRA)
                        as &objc2::runtime::AnyObject,
                ],
            );
            output.setVideoSettings(Some(&settings));
            output.setAlwaysDiscardsLateVideoFrames(true);
            if let Some(connection) = output.connectionWithMediaType(media)
                && connection.isVideoMirroringSupported()
            {
                connection.setAutomaticallyAdjustsVideoMirroring(false);
                connection.setVideoMirrored(false);
            }
            let (sender, frames) = crossbeam_channel::bounded(1);
            let state = Arc::new(State {
                frames: sender,
                closed: AtomicBool::new(false),
            });
            let allocated = Output::alloc().set_ivars(Arc::clone(&state));
            let delegate: Retained<Output> = msg_send![super(allocated), init];
            let queue = crate::qos::application_queue("app.snow.webcam.frames");
            output.setSampleBufferDelegate_queue(
                Some(ProtocolObject::from_ref(&*delegate)),
                Some(&queue),
            );
            let stream = Self {
                session,
                output,
                _delegate: delegate,
                _queue: queue,
                state,
                frames,
            };
            stream.session.startRunning();
            if !stream.session.isRunning() {
                return Err("The camera could not start.".into());
            }
            Ok(stream)
        })
    }
    pub fn poll(&self, timeout: Duration) -> Result<Option<Frame>, String> {
        match self.frames.recv_timeout(timeout) {
            Ok((image, at)) => {
                let size = image.size();
                let cpu = image
                    .to_cpu()
                    .map_err(|e| e.to_string())?
                    .into_format(PixelFormat::Rgba8)
                    .map_err(|e| e.to_string())?;
                Ok(Some(Frame {
                    width: size.width,
                    height: size.height,
                    rgba: cpu
                        .plane_bytes(0)
                        .ok_or("Invalid camera frame layout.")?
                        .to_vec(),
                    stride: cpu.planes[0].stride,
                    at,
                }))
            }
            Err(crossbeam_channel::RecvTimeoutError::Timeout) => Ok(None),
            Err(_) => Err("The camera disconnected.".into()),
        }
    }
}
impl Drop for Stream {
    fn drop(&mut self) {
        self.state.closed.store(true, Ordering::Release);
        unsafe {
            self.output.setSampleBufferDelegate_queue(None, None);
            self.session.stopRunning();
        }
        // The capture owner is never the callback queue. Drain already queued callbacks
        // before releasing the delegate; their closed flag prevents any further delivery.
        self._queue.exec_sync(|| {});
    }
}

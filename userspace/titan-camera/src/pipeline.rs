//! GStreamer pipelines on libcamera's element (the titan_camera driver's hardware-ISP YUYV):
//!
//! - preview, optionally recording: libcamerasrc -> tee: (1) the GPU uploads the YUYV once,
//!   converts and rotates it, then splits it: a screen-size RGBA copy for the view, and when
//!   recording an NV12 copy at the video size for the H.264 encoder (with Opus sound from
//!   PipeWire's Pulse server into MP4, or Matroska when this mp4mux takes no Opus); (2) the raw
//!   frames for autofocus and exposure; (3) a few frames a second for the QR decoder (qr.rs),
//!   in that branch's own thread, so a slow scan only drops QR frames.
//! - still: the camera at a photo size; frames after the settle ones are scored for sharpness
//!   and the sharpest of a short burst is kept (an unsteady hand blurs the others).
//!
//! The camera allows one stream at a time, so a photo or a recording replaces the preview.
//! Callbacks run on streaming threads and only send `Msg`s to the main loop.
use crate::af;
use crate::qr;
use gst::prelude::*;
use std::path::PathBuf;
use std::sync::atomic::{AtomicU32, Ordering};
use std::sync::{Arc, Mutex};

pub const VIEW_BOX: (u32, u32) = (720, 540);
const JPEG_QUALITY: i32 = 92;

pub struct Frame {
    pub w: u32,
    pub h: u32,
    pub stride: usize,
    pub data: Vec<u8>,
}

pub enum Msg {
    /// an RGBA preview frame
    View(u64, Frame),
    /// autofocus and exposure: running time, sharpness of the focus region, mean luma (0-255)
    Measure(u64, u64, f64, f64),
    /// the still burst is in: the sharpest frame (YUYV)
    Still(u64, Frame),
    Saved(Result<PathBuf, String>),
    /// a recording finished (EOS reached) or failed
    Recorded(u64, Result<PathBuf, String>),
    Error(u64, String),
    /// a QR code in view: its text
    Qr(u64, String),
}

pub type Tx = async_channel::Sender<Msg>;

fn has(name: &str) -> bool {
    gst::ElementFactory::find(name).is_some()
}

/// the best H.264 encoder here: the hardware one when it has a V4L2 driver, else software;
/// (element, the raw format it takes)
pub fn video_encoder(kbps: u32, fps: u32) -> Option<(String, &'static str)> {
    if has("v4l2h264enc") {
        Some((format!("v4l2h264enc extra-controls=\"encode,video_bitrate={}\"", kbps * 1000), "NV12"))
    } else if has("x264enc") {
        Some((
            format!("x264enc speed-preset=ultrafast tune=zerolatency bitrate={kbps} key-int-max={} threads=4", 2 * fps),
            "I420",
        ))
    } else if has("openh264enc") {
        Some((format!("openh264enc bitrate={} complexity=low gop-size={}", kbps * 1000, 2 * fps), "I420"))
    } else {
        None
    }
}

/// MP4 when its muxer takes Opus, else Matroska: (element, file extension)
fn muxer() -> (&'static str, &'static str) {
    let opus_in_mp4 = gst::ElementFactory::find("mp4mux")
        .map(|f| f.static_pad_templates().iter().any(|t| t.caps().to_string().contains("audio/x-opus")))
        .unwrap_or(false);
    if opus_in_mp4 {
        ("mp4mux", "mp4")
    } else {
        ("matroskamux", "mkv")
    }
}

pub struct Record {
    pub path: PathBuf,
    /// video size after rotation
    pub w: u32,
    pub h: u32,
    pub kbps: u32,
}

/// the next free name in `dir`: PREFIX_YYYYmmdd_HHMMSS.EXT
pub fn new_path(dir: &PathBuf, prefix: &str, ext: &str) -> PathBuf {
    let t = unsafe {
        let now = libc::time(std::ptr::null_mut());
        let mut tm: libc::tm = std::mem::zeroed();
        libc::localtime_r(&now, &mut tm);
        format!("{:04}{:02}{:02}_{:02}{:02}{:02}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec)
    };
    let mut p = dir.join(format!("{prefix}_{t}.{ext}"));
    let mut n = 1;
    while p.exists() {
        p = dir.join(format!("{prefix}_{t}-{n}.{ext}"));
        n += 1;
    }
    p
}

pub fn record_ext() -> &'static str {
    muxer().1
}

/// mean luma of a YUYV frame's centre-weighted subsample (every 8th pixel, every 8th line)
pub fn mean_luma(data: &[u8], w: usize, h: usize, stride: usize) -> f64 {
    let (mut s, mut n) = (0f64, 0f64);
    for y in (0..h).step_by(8) {
        let wy = if (h / 4..3 * h / 4).contains(&y) { 2.0 } else { 1.0 };
        let row = &data[y * stride..];
        for x in (0..w).step_by(8) {
            let wx = if (w / 4..3 * w / 4).contains(&x) { 2.0 } else { 1.0 };
            s += row[2 * x] as f64 * wx * wy;
            n += wx * wy;
        }
    }
    if n > 0.0 { s / n } else { 0.0 }
}

fn map_frame(sample: &gst::Sample) -> Option<Frame> {
    let caps = sample.caps()?;
    let info = gst_video::VideoInfo::from_caps(caps).ok()?;
    let buf = sample.buffer()?;
    let m = buf.map_readable().ok()?;
    Some(Frame { w: info.width(), h: info.height(), stride: info.stride()[0] as usize, data: m.as_slice().to_vec() })
}

/// the preview (and recording) pipeline
pub struct Preview {
    pub pipeline: gst::Pipeline,
    pub recording: Option<Record>,
    /// a view frame is on its way to the main loop: later ones are dropped meanwhile
    pending: Arc<AtomicU32>,
}

impl Preview {
    /// `size`: the camera mode; `dir`: video orientation; `af_region` shared with the main loop
    pub fn new(
        cam_id: &str,
        size: (u32, u32),
        dir: &str,
        rotated: bool,
        record: Option<Record>,
        gen: u64,
        tx: Tx,
        af_region: Arc<Mutex<(f64, f64)>>,
    ) -> Result<Preview, String> {
        let (w, h) = size;
        let (dw, dh) = if rotated { (h, w) } else { (w, h) };
        let k = (VIEW_BOX.0 as f64 / dw as f64).min(VIEW_BOX.1 as f64 / dh as f64);
        let (vw, vh) = (((dw as f64 * k) as u32) & !1, ((dh as f64 * k) as u32) & !1);
        let mut desc = format!(
            "libcamerasrc name=src camera-name=\"{cam_id}\" ! video/x-raw,format=YUY2,width={w},height={h} ! tee name=t \
             t. ! queue max-size-buffers=2 leaky=downstream ! glupload ! glcolorconvert ! glvideoflip video-direction={dir} ! tee name=g \
             g. ! queue max-size-buffers=1 leaky=downstream ! glcolorscale ! \
             video/x-raw(memory:GLMemory),format=RGBA,width={vw},height={vh} ! gldownload ! video/x-raw,format=RGBA ! \
             appsink name=view max-buffers=1 drop=true sync=false \
             t. ! queue max-size-buffers=1 leaky=downstream ! appsink name=af max-buffers=1 drop=true sync=false"
        );
        if qr::available() {
            desc += &format!(
                " t. ! videorate max-rate={} drop-only=true ! queue max-size-buffers=1 leaky=downstream ! \
                 appsink name=qr max-buffers=1 drop=true sync=false",
                qr::RATE
            );
        }
        if let Some(r) = &record {
            let (enc, fmt) = video_encoder(r.kbps, 30).ok_or("no H.264 encoder installed (gstreamer1.0-plugins-ugly)")?;
            let (mux, _) = muxer();
            // The GPU's YUV output pass (NV12, YUY2) runs at 12-15 fps at this size on Panfrost,
            // RGBA at 30: so RGBA comes down and the CPU converts it, 4 threads on the big cores
            // (measured 2026-11-09: 28-30 fps at 720x960 with x264, against 20 for GPU NV12)
            desc += &format!(
                " g. ! queue max-size-buffers=8 ! glcolorscale ! \
                 video/x-raw(memory:GLMemory),format=RGBA,width={},height={} ! gldownload ! video/x-raw,format=RGBA ! \
                 queue max-size-buffers=8 ! videoconvert n-threads=4 ! video/x-raw,format={fmt} ! \
                 queue max-size-buffers=8 ! {enc} ! h264parse ! queue ! mux. \
                 pulsesrc ! audio/x-raw,rate=48000,channels=1 ! queue ! audioconvert ! audioresample ! \
                 opusenc bitrate=64000 ! queue ! mux. \
                 {mux} name=mux ! filesink location=\"{}\"",
                r.w,
                r.h,
                r.path.display()
            );
        }
        let pipeline = gst::parse::launch(&desc)
            .map_err(|e| e.to_string())?
            .downcast::<gst::Pipeline>()
            .map_err(|_| "not a pipeline")?;

        let view = pipeline.by_name("view").unwrap().downcast::<gst_app::AppSink>().unwrap();
        let vtx = tx.clone();
        // one view frame in flight: frames that come meanwhile are dropped
        let pending = Arc::new(AtomicU32::new(0));
        let pend = pending.clone();
        view.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |s| {
                    let sample = s.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    if pend.load(Ordering::Acquire) == 0 {
                        if let Some(f) = map_frame(&sample) {
                            pend.store(1, Ordering::Release);
                            let _ = vtx.try_send(Msg::View(gen, f));
                        }
                    }
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );

        let afs = pipeline.by_name("af").unwrap().downcast::<gst_app::AppSink>().unwrap();
        let atx = tx.clone();
        let mut every = 0u32;
        afs.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |s| {
                    let sample = s.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    every += 1;
                    if every % 2 != 0 {
                        return Ok(gst::FlowSuccess::Ok); // every other frame is plenty
                    }
                    let pts = sample.buffer().and_then(|b| b.pts()).map(|t| t.nseconds()).unwrap_or(0);
                    let (Some(caps), Some(buf)) = (sample.caps(), sample.buffer()) else { return Ok(gst::FlowSuccess::Ok) };
                    let Ok(info) = gst_video::VideoInfo::from_caps(caps) else { return Ok(gst::FlowSuccess::Ok) };
                    let Ok(m) = buf.map_readable() else { return Ok(gst::FlowSuccess::Ok) };
                    let (w, h, st) = (info.width() as usize, info.height() as usize, info.stride()[0] as usize);
                    let region = *af_region.lock().unwrap();
                    let sharp = af::sharpness(m.as_slice(), w, h, st, region);
                    let luma = mean_luma(m.as_slice(), w, h, st);
                    let _ = atx.try_send(Msg::Measure(gen, pts, sharp, luma));
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );
        if let Some(qs) = pipeline.by_name("qr").and_then(|e| e.downcast::<gst_app::AppSink>().ok()) {
            let qtx = tx.clone();
            let mut scanner: Option<qr::Scanner> = None; // made in the decoding thread
            qs.set_callbacks(
                gst_app::AppSinkCallbacks::builder()
                    .new_sample(move |s| {
                        let sample = s.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                        if scanner.is_none() {
                            scanner = qr::Scanner::new();
                        }
                        let Some(sc) = scanner.as_mut() else { return Ok(gst::FlowSuccess::Ok) };
                        let (Some(caps), Some(buf)) = (sample.caps(), sample.buffer()) else { return Ok(gst::FlowSuccess::Ok) };
                        let Ok(info) = gst_video::VideoInfo::from_caps(caps) else { return Ok(gst::FlowSuccess::Ok) };
                        let Ok(m) = buf.map_readable() else { return Ok(gst::FlowSuccess::Ok) };
                        for text in sc.scan_yuyv(m.as_slice(), info.width() as usize, info.height() as usize, info.stride()[0] as usize) {
                            if !text.is_empty() {
                                let _ = qtx.try_send(Msg::Qr(gen, text));
                            }
                        }
                        Ok(gst::FlowSuccess::Ok)
                    })
                    .build(),
            );
        }
        watch_bus(&pipeline, gen, tx, record.as_ref().map(|r| r.path.clone()));
        Ok(Preview { pipeline, recording: record, pending })
    }

    pub fn start(&self) -> bool {
        self.pipeline.set_state(gst::State::Playing).is_ok()
    }

    /// the main loop has shown the last view frame: the next one may come
    pub fn view_shown(&self) {
        self.pending.store(0, Ordering::Release);
    }

    /// running time of the pipeline (ns): frame timestamps are on the same clock
    pub fn running_time(&self) -> u64 {
        self.pipeline.current_running_time().map(|t| t.nseconds()).unwrap_or(0)
    }

    /// recording: end the file properly (EOS through the muxer); `Msg::Recorded` follows
    pub fn finish(&self) {
        self.pipeline.send_event(gst::event::Eos::new());
    }

    pub fn stop(&self) {
        let _ = self.pipeline.set_state(gst::State::Null);
    }
}

fn watch_bus(pipeline: &gst::Pipeline, gen: u64, tx: Tx, rec: Option<PathBuf>) {
    let bus = pipeline.bus().unwrap();
    bus.set_sync_handler(move |_, msg| {
        use gst::MessageView;
        match msg.view() {
            MessageView::Error(e) => {
                let text = e.error().to_string();
                let _ = tx.try_send(match &rec {
                    Some(_) => Msg::Recorded(gen, Err(text)),
                    None => Msg::Error(gen, text),
                });
            }
            MessageView::Eos(_) => {
                if let Some(p) = &rec {
                    let _ = tx.try_send(Msg::Recorded(gen, Ok(p.clone())));
                }
            }
            _ => {}
        }
        gst::BusSyncReply::Drop
    });
}

/// still capture: settle frames dropped, then a burst scored for sharpness
pub struct Still {
    pub pipeline: gst::Pipeline,
}

pub struct StillPlan {
    pub settle: u32,
    pub burst: u32,
    /// called on the streaming thread with the first frame: manual exposure, flash pulse
    pub on_first: Option<Box<dyn FnOnce() + Send>>,
}

impl Still {
    pub fn new(cam_id: &str, size: (u32, u32), plan: StillPlan, region: (f64, f64), gen: u64, tx: Tx) -> Result<Still, String> {
        let (w, h) = size;
        let desc = format!(
            "libcamerasrc name=src camera-name=\"{cam_id}\" ! video/x-raw,format=YUY2,width={w},height={h} ! \
             identity name=gate ! appsink name=out max-buffers=2 drop=false sync=false"
        );
        let pipeline = gst::parse::launch(&desc)
            .map_err(|e| e.to_string())?
            .downcast::<gst::Pipeline>()
            .map_err(|_| "not a pipeline")?;
        let StillPlan { settle, burst, on_first } = plan;
        let first = Mutex::new(on_first);
        let count = AtomicU32::new(0);
        pipeline.by_name("gate").unwrap().static_pad("src").unwrap().add_probe(gst::PadProbeType::BUFFER, move |_, _| {
            if let Some(f) = first.lock().unwrap().take() {
                f();
            }
            let n = count.fetch_add(1, Ordering::AcqRel) + 1;
            if n <= settle || n > settle + burst {
                gst::PadProbeReturn::Drop
            } else {
                gst::PadProbeReturn::Ok
            }
        });
        let out = pipeline.by_name("out").unwrap().downcast::<gst_app::AppSink>().unwrap();
        let stx = tx.clone();
        let best: Mutex<(f64, Option<Frame>, u32)> = Mutex::new((-1.0, None, 0));
        out.set_callbacks(
            gst_app::AppSinkCallbacks::builder()
                .new_sample(move |s| {
                    let sample = s.pull_sample().map_err(|_| gst::FlowError::Eos)?;
                    let mut b = best.lock().unwrap();
                    if b.2 >= burst {
                        return Ok(gst::FlowSuccess::Ok);
                    }
                    if let Some(f) = map_frame(&sample) {
                        let score = af::sharpness(&f.data, f.w as usize, f.h as usize, f.stride, region);
                        if score > b.0 {
                            b.0 = score;
                            b.1 = Some(f);
                        }
                    }
                    b.2 += 1;
                    if b.2 >= burst {
                        if let Some(f) = b.1.take() {
                            let _ = stx.try_send(Msg::Still(gen, f));
                        }
                    }
                    Ok(gst::FlowSuccess::Ok)
                })
                .build(),
        );
        watch_bus(&pipeline, gen, tx, None);
        Ok(Still { pipeline })
    }

    pub fn start(&self) -> bool {
        self.pipeline.set_state(gst::State::Playing).is_ok()
    }

    pub fn stop(&self) {
        let _ = self.pipeline.set_state(gst::State::Null);
    }
}

/// rotate and JPEG-encode a YUYV frame, write it under `dir` (a worker thread)
pub fn save_jpeg(f: Frame, dir: &str, folder: PathBuf, tx: Tx) {
    let dir = dir.to_string();
    std::thread::spawn(move || {
        let r = encode_jpeg(&f, &dir).and_then(|jpeg| {
            std::fs::create_dir_all(&folder).map_err(|e| e.to_string())?;
            let path = new_path(&folder, "IMG", "jpg");
            std::fs::write(&path, jpeg).map_err(|e| e.to_string())?;
            Ok(path)
        });
        let _ = tx.send_blocking(Msg::Saved(r));
    });
}

fn encode_jpeg(f: &Frame, dir: &str) -> Result<Vec<u8>, String> {
    let p = gst::parse::launch(&format!(
        "appsrc name=src format=time ! videoflip video-direction={dir} ! videoconvert ! jpegenc quality={JPEG_QUALITY} ! \
         appsink name=out sync=false"
    ))
    .map_err(|e| e.to_string())?
    .downcast::<gst::Pipeline>()
    .map_err(|_| "not a pipeline")?;
    let src = p.by_name("src").unwrap().downcast::<gst_app::AppSrc>().unwrap();
    let info = gst_video::VideoInfo::builder(gst_video::VideoFormat::Yuy2, f.w, f.h).build().map_err(|e| e.to_string())?;
    src.set_caps(Some(&info.to_caps().map_err(|e| e.to_string())?));
    let out = p.by_name("out").unwrap().downcast::<gst_app::AppSink>().unwrap();
    p.set_state(gst::State::Playing).map_err(|e| e.to_string())?;
    // repack to the tight stride the caps describe
    let row = f.w as usize * 2;
    let mut data = Vec::with_capacity(row * f.h as usize);
    for y in 0..f.h as usize {
        data.extend_from_slice(&f.data[y * f.stride..y * f.stride + row]);
    }
    src.push_buffer(gst::Buffer::from_mut_slice(data)).map_err(|e| format!("{e:?}"))?;
    let _ = src.end_of_stream();
    let sample = out.try_pull_sample(gst::ClockTime::from_seconds(15));
    let _ = p.set_state(gst::State::Null);
    let sample = sample.ok_or("encode timed out")?;
    let buf = sample.buffer().ok_or("no buffer")?;
    let m = buf.map_readable().map_err(|e| e.to_string())?;
    Ok(m.as_slice().to_vec())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn luma_of_flat_frame() {
        let (w, h) = (64, 48);
        let mut f = vec![128u8; w * h * 2];
        for i in (0..f.len()).step_by(2) {
            f[i] = 100;
        }
        assert!((mean_luma(&f, w, h, w * 2) - 100.0).abs() < 1e-9);
    }
}

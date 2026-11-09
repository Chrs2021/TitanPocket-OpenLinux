//! A call's sound outside titan-ims: where it goes (the jacks), the ring, and titan-call-audio
//! for calls through ModemManager (titan-ims has the sound of its own calls on the CPU).
use std::process::{Child, Command, Stdio};

const RING: &str = "/run/user/0/titan-ring.wav";
const CALL_AUDIO: &str = "/usr/local/bin/titan-call-audio";
const VIBRATE: &str = "/usr/local/bin/titan-vibrate";

/// a jack's switch (the MT6358 ACCDET driver's controls)
fn jack(name: &str) -> bool {
    Command::new("amixer")
        .args(["-c", "titanpocket", "cget", &format!("iface=CARD,name={name}")])
        .output()
        .map(|o| String::from_utf8_lossy(&o.stdout).contains(": values=on"))
        .unwrap_or(false)
}

/// where a call's sound goes without the loudspeaker
pub fn wired_device() -> &'static str {
    if jack("Headset Mic Jack") {
        "headset"
    } else if jack("Headphones Jack") {
        "headphones"
    } else {
        "earpiece"
    }
}

/// a quiet two-tone ring (-30 dBFS, 0.4 s on, 0.2 s off, twice) as a WAV file
pub fn ring_wav() -> Vec<u8> {
    let rate = 16000u32;
    let amp = 1036.0;
    let mut pcm: Vec<u8> = Vec::new();
    for _ in 0..2 {
        for i in 0..(0.4 * rate as f64) as u32 {
            let t = i as f64 / rate as f64;
            let v = amp * ((2.0 * std::f64::consts::PI * 440.0 * t).sin() + (2.0 * std::f64::consts::PI * 480.0 * t).sin()) / 2.0;
            pcm.extend_from_slice(&(v as i16).to_le_bytes());
        }
        pcm.extend(std::iter::repeat(0u8).take(2 * (0.2 * rate as f64) as usize));
    }
    let mut w = Vec::with_capacity(44 + pcm.len());
    w.extend_from_slice(b"RIFF");
    w.extend_from_slice(&(36 + pcm.len() as u32).to_le_bytes());
    w.extend_from_slice(b"WAVEfmt ");
    w.extend_from_slice(&16u32.to_le_bytes());
    w.extend_from_slice(&1u16.to_le_bytes()); // PCM
    w.extend_from_slice(&1u16.to_le_bytes()); // mono
    w.extend_from_slice(&rate.to_le_bytes());
    w.extend_from_slice(&(rate * 2).to_le_bytes());
    w.extend_from_slice(&2u16.to_le_bytes());
    w.extend_from_slice(&16u16.to_le_bytes());
    w.extend_from_slice(b"data");
    w.extend_from_slice(&(pcm.len() as u32).to_le_bytes());
    w.extend_from_slice(&pcm);
    w
}

/// one ring: the tone (quietly) and a vibration pulse; the finished players are reaped
pub fn ring_once(players: &mut Vec<Child>) {
    let _ = Command::new(VIBRATE).arg("700").stdout(Stdio::null()).stderr(Stdio::null()).spawn().map(|mut c| {
        std::thread::spawn(move || c.wait());
    });
    if !std::path::Path::new(RING).exists() {
        let _ = std::fs::create_dir_all("/run/user/0");
        let _ = std::fs::write(RING, ring_wav());
    }
    players.retain_mut(|p| matches!(p.try_wait(), Ok(None)));
    if let Ok(c) = Command::new("pw-play").args(["--volume", "0.5", RING]).stdout(Stdio::null()).stderr(Stdio::null()).spawn() {
        players.push(c);
    }
}

/// titan-call-audio for a ModemManager call: runs while the call is active
pub fn call_audio(device: &str) -> Option<Child> {
    Command::new(CALL_AUDIO).args(["run", device]).stdout(Stdio::null()).stderr(Stdio::null()).spawn().ok()
}

pub fn call_audio_mute(on: bool) {
    let _ = Command::new(CALL_AUDIO).args(["mute", if on { "on" } else { "off" }]).stdout(Stdio::null()).stderr(Stdio::null()).spawn().map(|mut c| {
        std::thread::spawn(move || c.wait());
    });
}

pub fn stop(child: &mut Option<Child>) {
    if let Some(mut c) = child.take() {
        unsafe {
            libc::kill(c.id() as i32, libc::SIGTERM);
        }
        for _ in 0..50 {
            if matches!(c.try_wait(), Ok(Some(_))) {
                return;
            }
            std::thread::sleep(std::time::Duration::from_millis(100));
        }
        let _ = c.kill();
        let _ = c.wait();
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn ring_is_a_wav() {
        let w = super::ring_wav();
        assert_eq!(&w[..4], b"RIFF");
        assert_eq!(&w[8..12], b"WAVE");
        assert_eq!(w.len(), 44 + 2 * 2 * (6400 + 3200));
    }
}

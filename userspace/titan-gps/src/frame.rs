//! The /dev/stpgps framing (docs/gnss.md, "The /dev/stpgps protocol").
//!
//! ```text
//! AA F0 | len (2 bytes) | body (len bytes) | AA 0F
//! body = [type] FE [payload] [u16 sum]
//! ```
//!
//! - `len` is 12 bits; the chip puts a sequence number in the top 4 bits of
//!   its second byte (the host leaves them 0).
//! - `sum` is the 16-bit sum of the two length bytes as sent and every body
//!   byte before it.
//! - The length and the body are escaped: AA is sent as DE DF, DE as DE E0.
//! - The chip's payloads are XORed with 0xCA.

pub const START: [u8; 2] = [0xAA, 0xF0];
pub const END: [u8; 2] = [0xAA, 0x0F];
const ESC: u8 = 0xDE;
/// what the chip XORs its payloads with
pub const CHIP_XOR: u8 = 0xCA;

fn push_escaped(out: &mut Vec<u8>, bytes: &[u8]) {
    for &b in bytes {
        match b {
            0xAA => out.extend_from_slice(&[ESC, 0xDF]),
            0xDE => out.extend_from_slice(&[ESC, 0xE0]),
            _ => out.push(b),
        }
    }
}

/// how many bytes `bytes` take once escaped
pub fn escaped_len(bytes: &[u8]) -> usize {
    bytes.len() + bytes.iter().filter(|&&b| b == 0xAA || b == ESC).count()
}

/// a host frame of `kind` carrying `payload`, ready to write
pub fn encode(kind: u8, payload: &[u8]) -> Vec<u8> {
    let len = payload.len() + 4; // type, FE, the payload, the sum
    assert!(len < 0x1000, "a frame holds at most 4095 bytes");
    let head = [len as u8, (len >> 8) as u8];
    let mut body = Vec::with_capacity(len);
    body.push(kind);
    body.push(0xFE);
    body.extend_from_slice(payload);
    let sum = head.iter().chain(body.iter()).map(|&b| b as u32).sum::<u32>() as u16;
    body.extend_from_slice(&sum.to_le_bytes());
    let mut out = Vec::with_capacity(len + 16);
    out.extend_from_slice(&START);
    push_escaped(&mut out, &head);
    push_escaped(&mut out, &body);
    out.extend_from_slice(&END);
    out
}

/// a frame from the chip
#[derive(Debug, Clone, PartialEq)]
pub struct Frame {
    pub kind: u8,
    /// the sequence number in the length's top 4 bits
    pub seq: u8,
    /// after [type] FE, without the sum; XOR undone for the chip's frames
    pub payload: Vec<u8>,
}

/// splits the chip's byte stream into frames (they span reads)
#[derive(Default)]
pub struct Decoder {
    buf: Vec<u8>,
    /// bytes that weren't in a valid frame (the chip also sends unframed
    /// blocks, see docs/gnss.md)
    pub stray: usize,
    /// frames whose sum didn't match
    pub bad: usize,
}

enum Parse {
    Frame(Frame, usize),
    Junk(usize),
    More,
}

impl Decoder {
    pub fn new() -> Self {
        Self::default()
    }

    /// feed bytes as read; returns the frames they complete
    pub fn push(&mut self, data: &[u8], unxor: bool) -> Vec<Frame> {
        self.buf.extend_from_slice(data);
        let mut frames = Vec::new();
        let mut used = 0;
        loop {
            match self.parse(used, unxor) {
                Parse::Frame(f, n) => {
                    frames.push(f);
                    used += n;
                }
                Parse::Junk(n) => {
                    self.stray += n;
                    used += n;
                }
                Parse::More => break,
            }
        }
        self.buf.drain(..used);
        frames
    }

    fn parse(&mut self, from: usize, unxor: bool) -> Parse {
        let buf = &self.buf[from..];
        let start = match buf.windows(2).position(|w| w == START) {
            Some(p) => p,
            None => {
                // keep a trailing AA: it may start the next frame
                let keep = (buf.last() == Some(&0xAA)) as usize;
                return if buf.len() > keep { Parse::Junk(buf.len() - keep) } else { Parse::More };
            }
        };
        if start > 0 {
            return Parse::Junk(start);
        }
        // unescape the length, then exactly that many body bytes
        let mut out = Vec::new();
        let mut need = None;
        let mut k = 2;
        while need.map_or(true, |n: usize| out.len() < n + 2) {
            let Some(&b) = buf.get(k) else { return Parse::More };
            if b == ESC {
                let Some(&next) = buf.get(k + 1) else { return Parse::More };
                out.push(match next {
                    0xDF => 0xAA,
                    0xE0 => ESC,
                    other => other,
                });
                k += 2;
            } else {
                out.push(b);
                k += 1;
            }
            if need.is_none() && out.len() == 2 {
                need = Some(out[0] as usize | ((out[1] & 0x0F) as usize) << 8);
            }
        }
        let n = need.unwrap();
        if buf.len() < k + 2 {
            return Parse::More;
        }
        if buf[k..k + 2] != END || n < 4 {
            return Parse::Junk(2); // not a frame after all: resync
        }
        let (head, body) = out.split_at(2);
        let sum = head.iter().chain(&body[..n - 2]).map(|&b| b as u32).sum::<u32>() as u16;
        if sum != u16::from_le_bytes([body[n - 2], body[n - 1]]) {
            self.bad += 1;
            return Parse::Junk(k + 2);
        }
        let mut payload = body[2..n - 2].to_vec();
        if unxor {
            payload.iter_mut().for_each(|b| *b ^= CHIP_XOR);
        }
        Parse::Frame(Frame { kind: body[0], seq: head[1] >> 4, payload }, k + 2)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len()).step_by(2).map(|i| u8::from_str_radix(&s[i..i + 2], 16).unwrap()).collect()
    }

    #[test]
    fn encodes_as_libmnl_does() {
        // a setting frame as libmnl wrote it (gps2.trace)
        assert_eq!(encode(0x05, &hex("3200000100")), hex("aaf0090005fe32000001003f01aa0f"));
    }

    #[test]
    fn escapes_the_length_too() {
        // a 0x4DE-byte frame: its low length byte is DE
        let f = encode(0x0C, &vec![0x7B; 0x4DE - 4]);
        assert_eq!(&f[..5], &[0xAA, 0xF0, 0xDE, 0xE0, 0x04]);
    }

    #[test]
    fn decodes_the_chips_frames() {
        // a status frame from the chip: sequence 0xA, an escaped AA inside,
        // split across two reads
        let raw = hex("aaf033a02cfecaca113ecacacacacacacacec2cac2cacacacaca83ca47f2caca413e59bbebeacacacacadedf19cacacacad51ecace3bcf21aa0f");
        let mut d = Decoder::new();
        assert!(d.push(&raw[..20], true).is_empty());
        let frames = d.push(&raw[20..], true);
        assert_eq!(frames.len(), 1);
        assert_eq!((frames[0].kind, frames[0].seq, frames[0].payload.len()), (0x2C, 0xA, 0x33 - 4));
        assert_eq!(&frames[0].payload[..4], &[0, 0, 0xDB, 0xF4]);
        assert_eq!((d.stray, d.bad), (0, 0));
    }

    #[test]
    fn round_trips() {
        let payload: Vec<u8> = (0..=255u8).cycle().take(1400).collect();
        let mut d = Decoder::new();
        let frames = d.push(&encode(0x0C, &payload), false);
        assert_eq!(frames, vec![Frame { kind: 0x0C, seq: 0, payload }]);
    }
}

//! The receiver's DSP firmware patch: MediaTek's, loaded at the start of
//! every session (docs/gnss.md). It exists only inside libmnl.so, so it is
//! read from the phone's own copy and never stored here.
//!
//! In libmnl it is an 8-byte header and the image:
//!
//! ```text
//! [u16 size in 16-bit words] [u16 0x8000] [u16 0x8004] [u16 0x007B]
//! ```
//!
//! libmnl holds several images with the same header; the one this phone's
//! receiver takes is recognised by its CRC-32. It is sent as type 0x0C
//! frames, `[u16 number from 1][data]`: 1400-byte blocks, a block whose
//! escaped data would pass 1412 bytes going as two halves, and the rest in
//! the last.

use crate::frame;

/// the libmnl that ships with the phone (titan-mnld's chroot)
pub const LIBMNL: &str = "/opt/android-gps/vendor/lib/libmnl.so";
/// the image the MT6631 in this phone takes (61440 bytes)
pub const IMAGE_CRC32: u32 = 0x5A78_74A9;
/// where it was in the libmnl this was worked out on (searched if it moved)
pub const IMAGE_OFFSET: usize = 0x3E_0106;

pub const FRAME: u8 = 0x0C;
const BLOCK: usize = 1400;
const MAX_ESCAPED: usize = 1412;

/// the patch image and its header
pub struct Patch {
    pub header: [u8; 8],
    pub image: Vec<u8>,
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xEDB8_8320 & (crc & 1).wrapping_neg());
        }
    }
    !crc
}

fn image_at(lib: &[u8], at: usize) -> Option<Patch> {
    let header: [u8; 8] = lib.get(at.checked_sub(8)?..at)?.try_into().ok()?;
    if header[2..] != [0x00, 0x80, 0x04, 0x80, 0x7B, 0x00] {
        return None;
    }
    let size = u16::from_le_bytes([header[0], header[1]]) as usize * 2;
    let image = lib.get(at..at + size)?;
    (crc32(image) == IMAGE_CRC32).then(|| Patch { header, image: image.to_vec() })
}

impl Patch {
    /// the patch from a libmnl.so's bytes
    pub fn find(lib: &[u8]) -> Option<Patch> {
        image_at(lib, IMAGE_OFFSET).or_else(|| {
            let mark = [0x00u8, 0x80, 0x04, 0x80, 0x7B, 0x00];
            lib.windows(6)
                .enumerate()
                .filter(|(_, w)| *w == mark)
                .find_map(|(i, _)| image_at(lib, i + 6))
        })
    }

    pub fn load(path: &str) -> std::io::Result<Patch> {
        let lib = std::fs::read(path)?;
        Patch::find(&lib).ok_or_else(|| {
            std::io::Error::new(std::io::ErrorKind::NotFound,
                                format!("{path}: no MT6631 GNSS patch (CRC {IMAGE_CRC32:08x})"))
        })
    }

    /// the 0x0C frames' payloads, in order
    pub fn chunks(&self) -> Vec<Vec<u8>> {
        let mut out = Vec::new();
        let mut number = 1u16;
        let mut push = |data: &[u8]| {
            let mut p = number.to_le_bytes().to_vec();
            p.extend_from_slice(data);
            out.push(p);
            number += 1;
        };
        for block in self.image.chunks(BLOCK) {
            if frame::escaped_len(block) > MAX_ESCAPED {
                let (a, b) = block.split_at(block.len() / 2);
                push(a);
                push(b);
            } else {
                push(block);
            }
        }
        out
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crc32_is_ieee() {
        assert_eq!(crc32(b"123456789"), 0xCBF4_3926);
    }

    #[test]
    fn halves_blocks_that_escape_long() {
        let mut image = vec![0x11u8; 1400 * 3 + 40];
        image[1400..1400 + 13].fill(0xAA); // 1413 escaped: halved
        let p = Patch { header: [0; 8], image };
        let sizes: Vec<usize> = p.chunks().iter().map(|c| c.len() - 2).collect();
        assert_eq!(sizes, vec![1400, 700, 700, 1400, 40]);
        assert_eq!(&p.chunks()[2][..2], &[3, 0]);
    }
}

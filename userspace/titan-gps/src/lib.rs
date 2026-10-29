//! titan-gps: an open driver for the Titan Pocket's GNSS receiver (the
//! MT6631's, on /dev/stpgps), to replace MediaTek's closed libmnl step by
//! step (docs/gnss.md). The DSP firmware patch stays MediaTek's and is read
//! from the phone's own libmnl.so.

pub mod frame;
pub mod patch;

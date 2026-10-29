//! titan-gnss: GNSS for the Titan Pocket, through MediaTek's engine.
//!
//! The MT6631's GNSS receiver needs MediaTek's closed navigation library,
//! libmnl.so, hosted by titan-mnld (ours; 32-bit bionic in a small chroot,
//! see titan-mnld.service) in place of the stock mnld daemon. On Android, the
//! GNSS HAL (gps.default.so) drives mnld over two abstract Unix datagram
//! sockets; titan-mnld speaks the same protocol, and this program takes the
//! HAL's place and forwards the NMEA it gets to gpsd over UDP.
//!
//!   titan-gnss [daemon]   start the receiver, forward NMEA (titan-gnss.service)
//!   titan-gnss nmea       start the receiver, print NMEA (gpsd not needed)
//!
//! Protocol (recovered from gps.default.so; all ints little-endian):
//!   to mnld    "\0mtk_hal2mnl"  [i32 1][i32 cmd]...
//!              101 gps_init, 102 gps_start, 103 gps_stop,
//!              108 set_position_mode [mode][recurrence][interval ms]
//!                  [accuracy][time] [u8 low power]
//!   from mnld  "\0mtk_mnl2hal"  [i32 1][i32 type]...
//!              104 NMEA: [i64 timestamp][string][i32 length]
//!   string:    [u8 present][i32 length incl. NUL][bytes]
//! Abstract names are padded with NULs to the full 108-byte sun_path: the
//! HAL sends with addrlen = sizeof(sockaddr_un).

use std::io::{self, Write};
use std::net::UdpSocket;
use std::os::linux::net::SocketAddrExt;
use std::os::unix::net::{SocketAddr, UnixDatagram};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::time::{Duration, Instant};

const HAL2MNL: &str = "mtk_hal2mnl";
const MNL2HAL: &str = "mtk_mnl2hal";
const GPSD: &str = "127.0.0.1:10110"; // gpsd -n udp://127.0.0.1:10110

const VERSION: i32 = 1;
const CMD_INIT: i32 = 101;
const CMD_START: i32 = 102;
const CMD_STOP: i32 = 103;
const CMD_SET_POSITION_MODE: i32 = 108;
const MSG_NMEA: i32 = 104;

/// restart the session when mnld goes quiet this long (it may have restarted)
const QUIET: Duration = Duration::from_secs(30);

static STOP: AtomicBool = AtomicBool::new(false);

extern "C" {
    fn signal(sig: i32, handler: usize) -> usize;
}

extern "C" fn on_signal(_sig: i32) {
    STOP.store(true, Ordering::Relaxed);
}

/// "\0name" padded to the full sun_path, like the HAL's sockaddr_un
fn addr(name: &str) -> io::Result<SocketAddr> {
    let mut n = name.as_bytes().to_vec();
    n.resize(107, 0);
    SocketAddr::from_abstract_name(&n)
}

fn send(sock: &UnixDatagram, ints: &[i32], tail: &[u8]) -> io::Result<()> {
    let mut msg = Vec::with_capacity(4 * ints.len() + tail.len());
    for v in ints {
        msg.extend_from_slice(&v.to_le_bytes());
    }
    msg.extend_from_slice(tail);
    sock.send_to_addr(&msg, &addr(HAL2MNL)?)?;
    Ok(())
}

/// the NMEA sentence(s) of a type-104 message
fn nmea(msg: &[u8]) -> Option<String> {
    // [ver][type][i64 timestamp][u8 present][i32 len][bytes...]
    let present = *msg.get(16)?;
    if present == 0 {
        return None;
    }
    let len = i32::from_le_bytes(msg.get(17..21)?.try_into().ok()?) as usize;
    let text = msg.get(21..21 + len.min(msg.len().saturating_sub(21)))?;
    let text = String::from_utf8_lossy(text);
    let text = text.trim_end_matches('\0').trim();
    (!text.is_empty()).then(|| text.to_string())
}

fn start_session(sock: &UnixDatagram) -> io::Result<()> {
    send(sock, &[VERSION, CMD_INIT], &[])?;
    std::thread::sleep(Duration::from_millis(300));
    // standalone, periodic, 1 s, no accuracy/time preference, no low power
    send(sock, &[VERSION, CMD_SET_POSITION_MODE, 0, 0, 1000, 0, 0], &[0])?;
    send(sock, &[VERSION, CMD_START], &[])
}

fn run(print: bool) -> Result<(), String> {
    unsafe {
        signal(2, on_signal as extern "C" fn(i32) as usize); // SIGINT
        signal(15, on_signal as extern "C" fn(i32) as usize); // SIGTERM
    }
    let sock = UnixDatagram::unbound().map_err(|e| e.to_string())?;
    let rx = UnixDatagram::bind_addr(&addr(MNL2HAL).map_err(|e| e.to_string())?)
        .map_err(|e| format!("bind {MNL2HAL} (another titan-gnss running?): {e}"))?;
    rx.set_read_timeout(Some(Duration::from_secs(1))).map_err(|e| e.to_string())?;
    let udp = UdpSocket::bind("127.0.0.1:0").map_err(|e| e.to_string())?;

    let mut started = false;
    let mut last_msg = Instant::now();
    let mut buf = vec![0u8; 32768];
    let mut sentences: u64 = 0;
    let out = io::stdout();
    while !STOP.load(Ordering::Relaxed) {
        if !started || last_msg.elapsed() > QUIET {
            match start_session(&sock) {
                Ok(()) => {
                    if started {
                        eprintln!("titan-gnss: no data from mnld for {}s, restarting the session", QUIET.as_secs());
                    } else {
                        eprintln!("titan-gnss: receiver started");
                    }
                    started = true;
                }
                Err(e) => eprintln!("titan-gnss: mnld not reachable ({e}), retrying"),
            }
            last_msg = Instant::now();
        }
        let n = match rx.recv(&mut buf) {
            Ok(n) => n,
            Err(e) if matches!(e.kind(), io::ErrorKind::WouldBlock | io::ErrorKind::TimedOut | io::ErrorKind::Interrupted) => continue,
            Err(e) => return Err(format!("receive: {e}")),
        };
        last_msg = Instant::now();
        let msg = &buf[..n];
        if n < 8 || i32::from_le_bytes(msg[4..8].try_into().unwrap()) != MSG_NMEA {
            continue;
        }
        let Some(text) = nmea(msg) else { continue };
        for line in text.lines().map(str::trim).filter(|l| l.starts_with('$')) {
            sentences += 1;
            if sentences == 1 {
                eprintln!("titan-gnss: NMEA flowing");
            }
            if print {
                let _ = writeln!(out.lock(), "{line}");
            } else {
                let _ = udp.send_to(format!("{line}\r\n").as_bytes(), GPSD);
            }
        }
    }
    let _ = send(&sock, &[VERSION, CMD_STOP], &[]);
    eprintln!("titan-gnss: receiver stopped ({sentences} sentences)");
    Ok(())
}

fn main() -> ExitCode {
    let arg = std::env::args().nth(1).unwrap_or_else(|| "daemon".into());
    let result = match arg.as_str() {
        "daemon" => run(false),
        "nmea" => run(true),
        _ => Err("usage: titan-gnss [daemon|nmea]".into()),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("titan-gnss: {e}");
            ExitCode::FAILURE
        }
    }
}

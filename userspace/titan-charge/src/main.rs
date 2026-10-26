//! titan-charge: battery and charging for the Titan Pocket: a log, a charge
//! limit and the indicator LED.
//!
//! Every 10 s it reads the battery (the OZ8806 gauge), the charger and a few
//! temperatures.
//!
//! **Charge limit** (/var/lib/titan-charge/limit, 50-100, default 100: none):
//! at the limit charging stops and the phone runs from USB; it resumes at
//! the limit minus RESUME_GAP. The switch is MediaTek's charger manager's
//! `cmd_discharging` (/proc/mtk_battery_cmd/current_cmd: "0 1" stops, "0 0"
//! charges). Its own smart-charging limit (sc_tuisoc) would work off
//! MediaTek's battery percentage, which is -1 here: the gauge is the OZ8806.
//! Unplugging clears the hold, so the next plug-in starts charging. The hold
//! also needs the cell above HOLD_MIN_MV, so a wrong percentage can't keep
//! an empty battery from charging.
//!
//! **LED** (MT6370 RGB: led1 red, led2 green, led3 blue, as the stock lights
//! HAL maps them): red while charging, green when full or held at the limit,
//! a short red flash every 2 s below LOW % on battery (the chip's own PWM
//! blink: no wake-ups), off otherwise.
//!
//! **Log**: a CSV line to /var/log/titan-charge/YYYY-MM.csv
//!
//!   - when the battery percentage changes (event `pct`),
//!   - on plugging in and out (`plug`, `unplug`),
//!   - every minute while plugged in (`tick`), so the whole charge curve is
//!     there, the constant-voltage tail after 100 % included,
//!   - once at start (`start`).
//!
//! While asleep (suspend to RAM) nothing runs; the first reading after a wake
//! catches up. A charge or a discharge is a few hundred lines, so the flash
//! barely notices. Each line is written and flushed on its own: a crash
//! loses at most the line being written.
//!
//! Events also `hold` and `resume` (the limit).
//!
//! Columns: time (local), event, plugged (0/1), status (the gauge's), percent,
//! voltage_mV, current_mA (positive = charging), temp_C (battery),
//! charge_mAh (gauge), vbus_mV (charger input), input_limit_mA (USB
//! CURRENT_MAX: 500 on a computer's port), mtk_status, mtk_percent (MediaTek's
//! own battery view, which the charger framework uses), charger_C, soc_C,
//! screen (on/off).
//!
//!   titan-charge               the service
//!   titan-charge status        one reading, the limit and the hold state
//!   titan-charge limit [N]     show or set the charge limit (100: none)

use std::fs::{self, OpenOptions};
use std::io::Write;
use std::path::Path;
use std::thread::sleep;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const DIR: &str = "/var/log/titan-charge";
const PS: &str = "/sys/class/power_supply";
const GAUGE: &str = "oz8806-battery";
const VBUS: &str = "/sys/devices/platform/charger/ADC_Charger_Voltage";
const SCREEN_OFF: &str = "/run/titan-screen.off";
const POLL: Duration = Duration::from_secs(10);
const TICK: Duration = Duration::from_secs(60);
const LIMIT_FILE: &str = "/var/lib/titan-charge/limit";
const CHG_CMD: &str = "/proc/mtk_battery_cmd/current_cmd";
const RESUME_GAP: i64 = 5;
/// The limit holds only above this cell voltage (mV): ~75 % on this battery's
/// own log (80 % ~4.09 V, 90 % ~4.21 V at light load). A gauge that is wrong
/// (2026-10-25: it booted at "95 %" on an empty cell, seeded from a stale OCV)
/// must never keep an empty battery from charging.
const HOLD_MIN_MV: i64 = 4000;
const LOW: i64 = 15;
const LED: &str = "/sys/class/leds/mt6370_pmu_led";
const LED_LEVEL: &str = "2"; // of 6
const HEADER: &str = "time,event,plugged,status,percent,voltage_mV,current_mA,temp_C,\
charge_mAh,vbus_mV,input_limit_mA,mtk_status,mtk_percent,charger_C,soc_C,screen";

fn read(path: &str) -> Option<String> {
    fs::read_to_string(path).ok().map(|s| s.trim().to_string())
}

fn read_i64(path: &str) -> Option<i64> {
    read(path)?.parse().ok()
}

fn ps(supply: &str, attr: &str) -> Option<String> {
    read(&format!("{}/{}/{}", PS, supply, attr))
}

fn ps_i64(supply: &str, attr: &str) -> Option<i64> {
    ps(supply, attr)?.parse().ok()
}

/// A thermal zone's temp file, by its type name.
fn thermal_zone(kind: &str) -> Option<String> {
    let mut zones: Vec<_> = fs::read_dir("/sys/class/thermal").ok()?.flatten().collect();
    zones.sort_by_key(|e| e.file_name());
    for z in zones {
        let p = z.path();
        if read(&format!("{}/type", p.display())).as_deref() == Some(kind) {
            return Some(format!("{}/temp", p.display()));
        }
    }
    None
}

// ---- local time from musl (no crates) ----

#[repr(C)]
struct Tm {
    sec: i32,
    min: i32,
    hour: i32,
    mday: i32,
    mon: i32,
    year: i32,
    wday: i32,
    yday: i32,
    isdst: i32,
    gmtoff: i64,
    zone: *const u8,
}

extern "C" {
    fn localtime_r(t: *const i64, tm: *mut Tm) -> *mut Tm;
    fn tzset();
}

fn local_now() -> Tm {
    let t = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs() as i64).unwrap_or(0);
    let mut tm = Tm {
        sec: 0, min: 0, hour: 0, mday: 0, mon: 0, year: 0, wday: 0, yday: 0, isdst: 0,
        gmtoff: 0, zone: std::ptr::null(),
    };
    unsafe {
        tzset();
        localtime_r(&t, &mut tm);
    }
    tm
}

fn stamp(tm: &Tm) -> String {
    let off = tm.gmtoff / 60;
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}{}{:02}:{:02}",
        tm.year + 1900, tm.mon + 1, tm.mday, tm.hour, tm.min, tm.sec,
        if off < 0 { '-' } else { '+' }, off.abs() / 60, off.abs() % 60
    )
}

// ---- a reading ----

struct Reading {
    plugged: bool,
    percent: Option<i64>,
    status: String,
    mv: Option<i64>,
    fields: Vec<String>, // everything after time,event
}

fn opt(v: Option<i64>, div: i64) -> String {
    match v {
        Some(x) if div == 1 => x.to_string(),
        Some(x) => format!("{:.1}", x as f64 / div as f64),
        None => String::new(),
    }
}

fn opt_int(v: Option<i64>, div: i64) -> String {
    v.map(|x| (x / div).to_string()).unwrap_or_default()
}

struct Sources {
    charger_t: Option<String>,
    soc_t: Option<String>,
}

fn reading(src: &Sources) -> Reading {
    let online = |s: &str| ps_i64(s, "online") == Some(1);
    let plugged = online("usb") || online("ac");
    let percent = ps_i64(GAUGE, "capacity");
    let fields = vec![
        (plugged as i32).to_string(),
        ps(GAUGE, "status").unwrap_or_default().replace(',', " "),
        opt(percent, 1),
        opt_int(ps_i64(GAUGE, "voltage_now"), 1000),
        opt_int(ps_i64(GAUGE, "current_now"), 1000),
        opt(ps_i64(GAUGE, "temp"), 10),
        opt_int(ps_i64(GAUGE, "charge_now"), 1000),
        opt(read_i64(VBUS), 1),
        if online("usb") { opt_int(ps_i64("usb", "current_max"), 1000) } else { String::new() },
        ps("battery", "status").unwrap_or_default().replace(',', " "),
        opt(ps_i64("battery", "capacity"), 1),
        opt(src.charger_t.as_deref().and_then(read_i64), 1000),
        opt(src.soc_t.as_deref().and_then(read_i64), 1000),
        (if Path::new(SCREEN_OFF).exists() { "off" } else { "on" }).to_string(),
    ];
    let status = ps(GAUGE, "status").unwrap_or_default();
    let mv = ps_i64(GAUGE, "voltage_now").map(|v| v / 1000);
    Reading { plugged, percent, status, mv, fields }
}

fn append(event: &str, r: &Reading) {
    let tm = local_now();
    let path = format!("{}/{:04}-{:02}.csv", DIR, tm.year + 1900, tm.mon + 1);
    let _ = fs::create_dir_all(DIR);
    let new = !Path::new(&path).exists();
    match OpenOptions::new().create(true).append(true).open(&path) {
        Ok(mut f) => {
            let mut line = String::new();
            if new {
                line.push_str(HEADER);
                line.push('\n');
            }
            line.push_str(&format!("{},{},{}\n", stamp(&tm), event, r.fields.join(",")));
            if let Err(e) = f.write_all(line.as_bytes()).and_then(|_| f.flush()) {
                eprintln!("titan-charge: {}: {}", path, e);
            }
        }
        Err(e) => eprintln!("titan-charge: {}: {}", path, e),
    }
}

// ---- the charge limit ----

fn limit() -> i64 {
    read(LIMIT_FILE).and_then(|s| s.parse().ok()).unwrap_or(100).clamp(50, 100)
}

fn set_limit(n: i64) -> Result<(), String> {
    let n = n.clamp(50, 100);
    let _ = fs::create_dir_all("/var/lib/titan-charge");
    let tmp = format!("{}.tmp", LIMIT_FILE);
    fs::write(&tmp, format!("{}\n", n)).map_err(|e| format!("{}: {}", tmp, e))?;
    fs::rename(&tmp, LIMIT_FILE).map_err(|e| format!("{}: {}", LIMIT_FILE, e))?;
    println!("{}", n);
    Ok(())
}

/// enable or stop charging (MediaTek's cmd_discharging)
fn charging(on: bool) {
    if let Err(e) = fs::write(CHG_CMD, if on { "0 0" } else { "0 1" }) {
        eprintln!("titan-charge: {}: {}", CHG_CMD, e);
    }
}

fn charging_held() -> bool {
    // "usb_unlimited cmd_discharging"
    read(CHG_CMD).map(|s| s.split_whitespace().nth(1) == Some("1")).unwrap_or(false)
}

// ---- the LED ----

#[derive(Clone, Copy, PartialEq, Debug)]
enum Led {
    Off,
    Charging,
    Full,
    Low,
}

fn led_write(n: u32, attr: &str, v: &str) {
    let _ = fs::write(format!("{}{}/{}", LED, n, attr), v);
}

fn led(state: Led) {
    for n in 1..=3 {
        led_write(n, "brightness", "0");
        led_write(n, "trigger", "cc_mode");
    }
    match state {
        Led::Off => {}
        Led::Charging => led_write(1, "brightness", LED_LEVEL),
        Led::Full => led_write(2, "brightness", LED_LEVEL),
        Led::Low => {
            // the chip blinks it: a 2 s period, a short flash
            led_write(1, "trigger", "pwm_mode");
            led_write(1, "pwm_dim_freq", "2");   // 0.5Hz in the driver's list
            led_write(1, "pwm_duty", "3");       // of 31
            led_write(1, "brightness", LED_LEVEL);
        }
    }
}

fn led_for(r: &Reading, held: bool) -> Led {
    let pct = r.percent.unwrap_or(50);
    if r.plugged {
        if held || r.status == "Full" || pct >= 100 {
            Led::Full
        } else {
            Led::Charging
        }
    } else if pct <= LOW {
        Led::Low
    } else {
        Led::Off
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    match args.get(1).map(|s| s.as_str()) {
        Some("limit") => {
            match args.get(2) {
                None => println!("{}", limit()),
                Some(n) => match n.parse::<i64>() {
                    Ok(n) => {
                        if let Err(e) = set_limit(n) {
                            eprintln!("titan-charge: {}", e);
                            std::process::exit(1);
                        }
                    }
                    Err(_) => {
                        eprintln!("titan-charge: limit 50-100 (100: no limit)");
                        std::process::exit(2);
                    }
                },
            }
            return;
        }
        Some("status") | Some("--once") => {}
        Some(other) if other != "daemon" => {
            eprintln!("usage: titan-charge [status | limit [50-100]]");
            std::process::exit(2);
        }
        _ => {}
    }
    run(args.get(1).map(|s| s.as_str()));
}

fn run(cmd: Option<&str>) {
    let src = Sources {
        charger_t: thermal_zone("mtktscharger"),
        soc_t: thermal_zone("mtktsAP"),
    };
    if matches!(cmd, Some("status") | Some("--once")) {
        let r = reading(&src);
        println!("{}\n{},now,{}", HEADER, stamp(&local_now()), r.fields.join(","));
        println!("limit {}%  charging {}", limit(), if charging_held() { "held" } else { "allowed" });
        return;
    }
    let first = reading(&src);
    append("start", &first);
    let (mut plugged, mut percent) = (first.plugged, first.percent);
    let mut last = Instant::now();
    // start clean: charging allowed unless already at the limit
    let mut held = false;
    charging(true);
    let mut shown = None;
    let mut r = first;
    loop {
        // the limit
        let lim = limit();
        let pct = r.percent.unwrap_or(0);
        let high = r.mv.map_or(false, |v| v >= HOLD_MIN_MV);
        if r.plugged && lim < 100 && !held && pct >= lim && high {
            charging(false);
            held = true;
            append("hold", &r);
        } else if held && (!r.plugged || lim >= 100 || pct <= lim - RESUME_GAP
                           || r.mv.map_or(false, |v| v < HOLD_MIN_MV - 100)) {
            charging(true);
            held = false;
            append("resume", &r);
        }
        // the LED
        let want = led_for(&r, held);
        if shown != Some(want) {
            led(want);
            shown = Some(want);
        }
        sleep(POLL);
        r = reading(&src);
        let event = if r.plugged != plugged {
            Some(if r.plugged { "plug" } else { "unplug" })
        } else if r.percent != percent && r.percent.is_some() {
            Some("pct")
        } else if r.plugged && last.elapsed() >= TICK {
            Some("tick")
        } else {
            None
        };
        if let Some(e) = event {
            append(e, &r);
            last = Instant::now();
        }
        plugged = r.plugged;
        percent = r.percent;
    }
}

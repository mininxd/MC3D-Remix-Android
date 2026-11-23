use log::{Level, LevelFilter, Metadata, Record};
use std::ffi::{c_char, CString};
use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Mutex;
use std::time::SystemTime;

#[link(name = "log")]
extern "C" {
    fn __android_log_write(prio: i32, tag: *const c_char, text: *const c_char) -> i32;
}

pub struct AppLogger {
    paths: Mutex<Vec<PathBuf>>,
}

static LOGGER: AppLogger = AppLogger {
    paths: Mutex::new(Vec::new()),
};

impl AppLogger {
    pub fn add_path<P: AsRef<Path>>(&self, path: P) {
        let p = path.as_ref().to_path_buf();
        if let Ok(mut paths) = self.paths.lock() {
            if !paths.contains(&p) {
                paths.push(p);
            }
        }
    }
}

impl log::Log for AppLogger {
    fn enabled(&self, metadata: &Metadata) -> bool {
        metadata.level() <= Level::Info
    }

    fn log(&self, record: &Record) {
        if !self.enabled(record.metadata()) {
            return;
        }

        let prio = match record.level() {
            Level::Error => 6,
            Level::Warn => 5,
            Level::Info => 4,
            Level::Debug => 3,
            Level::Trace => 2,
        };

        let msg = format!("{}", record.args());
        let tag = "MC3DR_Rust";

        // 1. Write to Android logcat
        if let (Ok(c_tag), Ok(c_msg)) = (CString::new(tag), CString::new(msg.clone())) {
            unsafe {
                __android_log_write(prio, c_tag.as_ptr(), c_msg.as_ptr());
            }
        }

        // 2. Write to persistent app_logs.txt files
        let timestamp = match SystemTime::now().duration_since(SystemTime::UNIX_EPOCH) {
            Ok(d) => format!("{}.{:03}", d.as_secs(), d.subsec_millis()),
            Err(_) => "0.000".to_string(),
        };

        let file_line = format!("[{}] [{}] [{}] {}\n", timestamp, record.level(), record.target(), msg);

        if let Ok(paths) = self.paths.lock() {
            for p in paths.iter() {
                if let Some(parent) = p.parent() {
                    let _ = std::fs::create_dir_all(parent);
                }
                if let Ok(mut f) = OpenOptions::new().create(true).append(true).open(p) {
                    let _ = f.write_all(file_line.as_bytes());
                    let _ = f.flush();
                }
            }
        }
    }

    fn flush(&self) {
        if let Ok(paths) = self.paths.lock() {
            for p in paths.iter() {
                if let Ok(f) = OpenOptions::new().append(true).open(p) {
                    let _ = f.sync_all();
                }
            }
        }
    }
}

pub fn init(app: &android_activity::AndroidApp) {
    // Primary log target in game folder
    let primary_log = PathBuf::from("/sdcard/mc3dr/app_logs.txt");
    let fallback_log = PathBuf::from("/storage/emulated/0/mc3dr/app_logs.txt");

    if primary_log.exists() || Path::new("/sdcard").exists() {
        LOGGER.add_path(primary_log);
    } else {
        LOGGER.add_path(fallback_log);
    }

    if let Some(internal) = app.internal_data_path() {
        LOGGER.add_path(internal.join("app_logs.txt"));
    }
    if let Some(external) = app.external_data_path() {
        LOGGER.add_path(external.join("app_logs.txt"));
    }

    let _ = log::set_logger(&LOGGER);
    log::set_max_level(LevelFilter::Info);

    // Initial banner
    log::info!("================================================================================");
    log::info!(" Midnight Club 3: DUB Edition Remix (MC3DR) - Persistent Application Log");
    log::info!(" Package: io.github.mininxd.midnightclub3remixrecomp");
    log::info!(" Target Architecture: aarch64-linux-android");
    log::info!(" Log Targets: /sdcard/mc3dr/app_logs.txt and internal storage");
    log::info!("================================================================================");
}

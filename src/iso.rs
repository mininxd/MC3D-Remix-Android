use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

const XDVDFS_MAGIC: &[u8] = b"MICROSOFT*XBOX*MEDIA";
const SEARCH_DIRECTORIES: &[&str] = &[
    "/sdcard/mc3dr",
    "/storage/emulated/0/mc3dr",
    "/sdcard/xbox",
    "/storage/emulated/0/xbox",
    "/sdcard/Download",
    "/sdcard/ROMs",
    "/sdcard/games",
    "/sdcard",
    "/storage/emulated/0",
];

#[derive(Debug, Clone)]
pub struct DiscoveredIso {
    pub path: PathBuf,
    pub filename: String,
    pub is_valid_xdvdfs: bool,
}

pub struct IsoManager {
    pub items: Vec<DiscoveredIso>,
    pub selected_idx: usize,
}

impl IsoManager {
    pub fn new() -> Self {
        let mut mgr = Self {
            items: Vec::new(),
            selected_idx: 0,
        };
        mgr.scan();
        mgr
    }

    pub fn scan(&mut self) {
        self.items.clear();
        for dir_str in SEARCH_DIRECTORIES {
            let dir = Path::new(dir_str);
            if !dir.exists() || !dir.is_dir() {
                continue;
            }

            if let Ok(entries) = fs::read_dir(dir) {
                for entry in entries.flatten() {
                    let path = entry.path();
                    if path.is_file() {
                        let is_iso = path
                            .extension()
                            .and_then(|ext| ext.to_str())
                            .map(|ext| ext.eq_ignore_ascii_case("iso") || ext.eq_ignore_ascii_case("xbe"))
                            .unwrap_or(false);

                        if is_iso {
                            let filename = path
                                .file_name()
                                .and_then(|n| n.to_str())
                                .unwrap_or("Unknown")
                                .to_string();

                            let is_valid = Self::verify_xdvdfs(&path);
                            self.items.push(DiscoveredIso {
                                path,
                                filename,
                                is_valid_xdvdfs: is_valid,
                            });
                        }
                    }
                }
            }
        }
    }

    pub fn selected(&self) -> Option<&DiscoveredIso> {
        self.items.get(self.selected_idx)
    }

    pub fn find_mc3dr_iso(&self) -> Option<&DiscoveredIso> {
        // Prioritize default.xbe in /sdcard/mc3dr, then any ISO in /sdcard/mc3dr
        self.items
            .iter()
            .find(|item| {
                let p = item.path.to_string_lossy();
                (p.contains("/mc3dr/") || p.contains("/mc3dr")) && p.ends_with("default.xbe")
            })
            .or_else(|| {
                self.items.iter().find(|item| {
                    let p = item.path.to_string_lossy();
                    p.contains("/mc3dr/") || p.contains("/mc3dr")
                })
            })
    }

    pub fn select_next(&mut self) {
        if !self.items.is_empty() {
            self.selected_idx = (self.selected_idx + 1) % self.items.len();
        }
    }

    pub fn select_prev(&mut self) {
        if !self.items.is_empty() {
            if self.selected_idx == 0 {
                self.selected_idx = self.items.len() - 1;
            } else {
                self.selected_idx -= 1;
            }
        }
    }

    pub fn verify_xdvdfs(path: &Path) -> bool {
        let mut file = match File::open(path) {
            Ok(f) => f,
            Err(_) => return false,
        };

        // Check if XBE executable
        let mut xbe_magic = [0u8; 4];
        if file.read_exact(&mut xbe_magic).is_ok() && &xbe_magic == b"XBEH" {
            return true;
        }

        // Check XDVDFS partition candidates: 0 (bare/redump game partition), 0x18300000 (redump video offset), 0x0FD90000
        let partitions = [0u64, 0x18300000u64, 0x0FD90000u64];
        let mut buf = [0u8; 20];

        for &base in &partitions {
            let volume_offset = base + 32 * 2048;
            if file.seek(SeekFrom::Start(volume_offset)).is_ok() {
                if let Ok(n) = file.read(&mut buf) {
                    if n >= XDVDFS_MAGIC.len() && &buf[..XDVDFS_MAGIC.len()] == XDVDFS_MAGIC {
                        return true;
                    }
                }
            }
        }
        false
    }
}

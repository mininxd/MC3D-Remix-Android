use std::fs::File;
use std::io::{Read, Seek, SeekFrom};
use std::path::Path;

const XBE_MAGIC: &[u8] = b"XBEH";
const RETAIL_ENTRY_KEY: u32 = 0xA8FC57AB;
const DEBUG_ENTRY_KEY: u32 = 0x94859D4B;
const RETAIL_THUNK_KEY: u32 = 0x5B6D40B6;
const DEBUG_THUNK_KEY: u32 = 0xEFB1F152;

#[derive(Debug, Clone)]
pub struct XbeSection {
    pub name: String,
    pub flags: u32,
    pub virtual_addr: u32,
    pub virtual_size: u32,
    pub raw_addr: u32,
    pub raw_size: u32,
    pub data: Vec<u8>,
}

#[derive(Debug, Clone)]
pub struct XbeImage {
    pub title_id: u32,
    pub title_name: String,
    pub base_addr: u32,
    pub image_size: u32,
    pub entry_point: u32,
    pub stack_commit: u32,
    pub heap_reserve: u32,
    pub heap_commit: u32,
    pub sections: Vec<XbeSection>,
    pub raw_bytes: Vec<u8>,
}

impl XbeImage {
    pub fn parse(data: &[u8]) -> Result<Self, String> {
        if data.len() < 512 {
            return Err("XBE file is too small to contain a valid header".to_string());
        }

        if &data[0..4] != XBE_MAGIC {
            return Err(format!(
                "Invalid XBE magic: expected 'XBEH', found {:?}",
                &data[0..4]
            ));
        }

        let base_addr = read_u32(data, 0x104)?;
        let image_size = read_u32(data, 0x10C)?;
        let num_sections = read_u32(data, 0x11C)? as usize;
        let sec_headers_va = read_u32(data, 0x120)?;
        let raw_entry = read_u32(data, 0x128)?;
        let cert_va = read_u32(data, 0x118)?;

        let stack_commit = read_u32(data, 0x130).unwrap_or(65536);
        let heap_reserve = read_u32(data, 0x134).unwrap_or(1048576);
        let heap_commit = read_u32(data, 0x138).unwrap_or(65536);

        // Decode entry point
        let mut entry_point = raw_entry ^ RETAIL_ENTRY_KEY;
        if entry_point < base_addr || entry_point >= base_addr + image_size {
            let debug_entry = raw_entry ^ DEBUG_ENTRY_KEY;
            if debug_entry >= base_addr && debug_entry < base_addr + image_size {
                entry_point = debug_entry;
            }
        }

        // Parse certificate for Title ID and Title Name
        let mut title_id = 0u32;
        let mut title_name = "Xbox Game".to_string();
        if cert_va >= base_addr {
            let cert_offset = (cert_va - base_addr) as usize;
            if cert_offset + 84 <= data.len() {
                title_id = read_u32(data, cert_offset + 8).unwrap_or(0);
                let name_utf16: Vec<u16> = (0..40)
                    .map(|i| {
                        let off = cert_offset + 12 + i * 2;
                        if off + 1 < data.len() {
                            u16::from_le_bytes([data[off], data[off + 1]])
                        } else {
                            0
                        }
                    })
                    .take_while(|&c| c != 0)
                    .collect();
                if let Ok(s) = String::from_utf16(&name_utf16) {
                    if !s.trim().is_empty() {
                        title_name = s.trim().to_string();
                    }
                }
            }
        }

        // Parse sections
        let mut sections = Vec::with_capacity(num_sections);
        if sec_headers_va >= base_addr {
            let sec_start = (sec_headers_va - base_addr) as usize;
            for i in 0..num_sections {
                let off = sec_start + i * 56;
                if off + 56 > data.len() {
                    break;
                }

                let flags = read_u32(data, off)?;
                let virtual_addr = read_u32(data, off + 4)?;
                let virtual_size = read_u32(data, off + 8)?;
                let raw_addr = read_u32(data, off + 12)?;
                let raw_size = read_u32(data, off + 16)?;
                let name_va = read_u32(data, off + 20)?;

                let mut name = format!("section_{}", i);
                if name_va >= base_addr {
                    let name_off = (name_va - base_addr) as usize;
                    if name_off < data.len() {
                        let bytes: Vec<u8> = data[name_off..]
                            .iter()
                            .take(32)
                            .copied()
                            .take_while(|&b| b != 0)
                            .collect();
                        if let Ok(s) = String::from_utf8(bytes) {
                            name = s;
                        }
                    }
                }

                let raw_start = raw_addr as usize;
                let raw_end = (raw_addr + raw_size) as usize;
                let sec_data = if raw_start < data.len() && raw_end <= data.len() {
                    data[raw_start..raw_end].to_vec()
                } else {
                    Vec::new()
                };

                sections.push(XbeSection {
                    name,
                    flags,
                    virtual_addr,
                    virtual_size,
                    raw_addr,
                    raw_size,
                    data: sec_data,
                });
            }
        }

        Ok(Self {
            title_id,
            title_name,
            base_addr,
            image_size,
            entry_point,
            stack_commit,
            heap_reserve,
            heap_commit,
            sections,
            raw_bytes: data.to_vec(),
        })
    }

    pub fn from_file<P: AsRef<Path>>(path: P) -> Result<Self, String> {
        let mut file = File::open(path.as_ref())
            .map_err(|e| format!("Failed to open file {}: {}", path.as_ref().display(), e))?;
        let mut buffer = Vec::new();
        file.read_to_end(&mut buffer)
            .map_err(|e| format!("Failed to read XBE file: {}", e))?;
        Self::parse(&buffer)
    }

    pub fn from_iso_or_file<P: AsRef<Path>>(path: P) -> Result<Self, String> {
        let p = path.as_ref();
        let ext = p.extension().and_then(|e| e.to_str()).unwrap_or("");
        if ext.eq_ignore_ascii_case("xbe") {
            return Self::from_file(p);
        }

        // Try extracting from Xbox ISO via XDVDFS
        let mut file = File::open(p)
            .map_err(|e| format!("Failed to open ISO file {}: {}", p.display(), e))?;

        // Partitions: 0 (standard/XISO/redump game partition) and 0x18300000 (raw video partition dump)
        let partitions = [0u64, 0x18300000u64, 0x0FD90000u64];
        for &base in &partitions {
            let volume_offset = base + 32 * 2048;
            if file.seek(SeekFrom::Start(volume_offset)).is_ok() {
                let mut header = [0u8; 2048];
                if file.read_exact(&mut header).is_ok() {
                    if &header[0..20] == b"MICROSOFT*XBOX*MEDIA" {
                        let root_sector = u32::from_le_bytes([
                            header[0x14],
                            header[0x15],
                            header[0x16],
                            header[0x17],
                        ]) as u64;
                        let root_size = u32::from_le_bytes([
                            header[0x18],
                            header[0x19],
                            header[0x1A],
                            header[0x1B],
                        ]) as usize;

                        let root_offset = base + root_sector * 2048;
                        if file.seek(SeekFrom::Start(root_offset)).is_ok() {
                            let mut dir_buf = vec![0u8; root_size];
                            if file.read_exact(&mut dir_buf).is_ok() {
                                if let Some((file_sector, file_size)) =
                                    find_in_xdvdfs_dir(&dir_buf, "default.xbe")
                                {
                                    let xbe_offset = base + (file_sector as u64) * 2048;
                                    if file.seek(SeekFrom::Start(xbe_offset)).is_ok() {
                                        let mut xbe_data = vec![0u8; file_size as usize];
                                        if file.read_exact(&mut xbe_data).is_ok() {
                                            return Self::parse(&xbe_data);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // Fallback: Check if there's an extracted mc3/default.xbe nearby
        let fallback_candidates = [
            Path::new("/sdcard/mc3dr/default.xbe"),
            Path::new("/sdcard/xbox/mc3/default.xbe"),
            Path::new("/sdcard/xbox/default.xbe"),
            Path::new("/storage/emulated/0/mc3dr/default.xbe"),
        ];
        for candidate in &fallback_candidates {
            if candidate.exists() {
                return Self::from_file(candidate);
            }
        }

        Err("Could not find default.xbe in ISO or cache".to_string())
    }
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32, String> {
    if offset + 4 <= data.len() {
        Ok(u32::from_le_bytes([
            data[offset],
            data[offset + 1],
            data[offset + 2],
            data[offset + 3],
        ]))
    } else {
        Err(format!("Read out of bounds at offset {}", offset))
    }
}

fn find_in_xdvdfs_dir(table: &[u8], target: &str) -> Option<(u32, u32)> {
    if table.is_empty() {
        return None;
    }
    let mut todo = vec![0usize];
    let mut seen = std::collections::HashSet::new();

    while let Some(at) = todo.pop() {
        if seen.contains(&at) || at + 14 > table.len() {
            continue;
        }
        seen.insert(at);

        let left = u16::from_le_bytes([table[at], table[at + 1]]);
        let right = u16::from_le_bytes([table[at + 2], table[at + 3]]);
        if left == 0xFFFF {
            continue;
        }

        let sector = u32::from_le_bytes([
            table[at + 4],
            table[at + 5],
            table[at + 6],
            table[at + 7],
        ]);
        let size = u32::from_le_bytes([
            table[at + 8],
            table[at + 9],
            table[at + 10],
            table[at + 11],
        ]);
        let attr = table[at + 12];
        let length = table[at + 13] as usize;

        if at + 14 + length <= table.len() {
            let name_bytes = &table[at + 14..at + 14 + length];
            if let Ok(name_str) = std::str::from_utf8(name_bytes) {
                if name_str.eq_ignore_ascii_case(target) && (attr & 0x10) == 0 {
                    return Some((sector, size));
                }
            }
        }

        if left != 0 {
            todo.push((left as usize) * 4);
        }
        if right != 0 {
            todo.push((right as usize) * 4);
        }
    }
    None
}

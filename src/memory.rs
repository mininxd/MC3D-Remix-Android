use crate::xbe::XbeImage;
use log::info;
use std::ptr;

pub const XBOX_RAM_SIZE: usize = 64 * 1024 * 1024; // 64 MB
pub const XBOX_STACK_TOP: u32 = 0x00130000;
pub const XBOX_STACK_SIZE: u32 = 64 * 1024; // 64 KB

pub struct XboxMemory {
    pub base_ptr: *mut u8,
    pub size: usize,
}

impl XboxMemory {
    pub fn new() -> Result<Self, String> {
        unsafe {
            // Allocate 64 MB of guest physical RAM
            let ptr = libc::mmap(
                ptr::null_mut(),
                XBOX_RAM_SIZE,
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_PRIVATE | libc::MAP_ANONYMOUS,
                -1,
                0,
            );

            if ptr == libc::MAP_FAILED {
                return Err("Failed to allocate 64MB Xbox unified guest RAM via mmap".to_string());
            }

            // Zero out memory
            libc::memset(ptr, 0, XBOX_RAM_SIZE);

            info!("Allocated 64MB Xbox unified RAM at host ptr: {:p}", ptr);

            Ok(Self {
                base_ptr: ptr as *mut u8,
                size: XBOX_RAM_SIZE,
            })
        }
    }

    pub fn load_xbe(&mut self, xbe: &XbeImage) -> Result<(), String> {
        info!("Mapping XBE sections into Xbox guest memory...");
        for sec in &xbe.sections {
            let va = sec.virtual_addr as usize;
            let raw_size = sec.raw_size as usize;
            let virt_size = sec.virtual_size as usize;

            if va + virt_size > self.size {
                info!(
                    "Section {} VA {:#010X} exceeds 64MB boundary, clamping",
                    sec.name, sec.virtual_addr
                );
                continue;
            }

            unsafe {
                let dst = self.base_ptr.add(va);
                if !sec.data.is_empty() && raw_size > 0 {
                    let copy_len = raw_size.min(sec.data.len()).min(virt_size);
                    ptr::copy_nonoverlapping(sec.data.as_ptr(), dst, copy_len);
                }
                // Zero initialize remaining virtual size (BSS)
                if virt_size > raw_size {
                    let bss_len = virt_size - raw_size;
                    ptr::write_bytes(dst.add(raw_size), 0, bss_len);
                }
            }

            info!(
                "Mapped section {:<12} at VA {:#010X} (size {:#08X})",
                sec.name, sec.virtual_addr, sec.virtual_size
            );
        }

        // Setup KPCR (Kernel Processor Control Region) and TIB at address 0x0
        self.setup_kpcr();

        Ok(())
    }

    fn setup_kpcr(&mut self) {
        unsafe {
            // fs:[0x18] points to TIB self pointer
            // fs:[0x20] points to KPCR
            let kpcr_ptr = self.base_ptr as *mut u32;
            // 0x00000000: Exception list
            *kpcr_ptr.add(0) = 0xFFFFFFFF;
            // 0x00000004: Stack base
            *kpcr_ptr.add(1) = XBOX_STACK_TOP;
            // 0x00000008: Stack limit
            *kpcr_ptr.add(2) = XBOX_STACK_TOP - XBOX_STACK_SIZE;
            // 0x00000018: Self pointer to TIB
            *kpcr_ptr.add(6) = 0x00000000;
            // 0x00000020: Self pointer to KPCR
            *kpcr_ptr.add(8) = 0x00000000;
            // 0x00000028: Current thread pointer (dummy)
            *kpcr_ptr.add(10) = 0x00000200;
        }
    }

    pub fn read_u32(&self, va: u32) -> u32 {
        let offset = (va as usize) % self.size;
        unsafe {
            let p = self.base_ptr.add(offset) as *const u32;
            *p
        }
    }

    pub fn write_u32(&mut self, va: u32, val: u32) {
        let offset = (va as usize) % self.size;
        unsafe {
            let p = self.base_ptr.add(offset) as *mut u32;
            *p = val;
        }
    }
}

impl Drop for XboxMemory {
    fn drop(&mut self) {
        if !self.base_ptr.is_null() {
            unsafe {
                libc::munmap(self.base_ptr as *mut libc::c_void, self.size);
            }
        }
    }
}

#![allow(non_upper_case_globals)]

use crate::config::AppConfig;
use crate::memory::XboxMemory;
use log::info;
use std::collections::HashMap;

// Xbox Video Flags
pub const XC_VIDEO_FLAGS_WIDESCREEN: u32 = 0x00010000;
pub const XC_VIDEO_FLAGS_HDTV_480p: u32 = 0x00080000;
pub const XC_VIDEO_FLAGS_HDTV_720p: u32 = 0x00020000;
pub const XC_VIDEO_FLAGS_HDTV_1080i: u32 = 0x00040000;

pub struct KernelState {
    pub display_mode: u32,
    pub video_flags: u32,
    pub contiguous_alloc_ptr: u32,
    pub events: HashMap<u32, bool>,
    pub frame_counter: u64,
}

impl KernelState {
    pub fn new(config: &AppConfig) -> Self {
        let mut video_flags = XC_VIDEO_FLAGS_HDTV_480p;
        if config.widescreen_16_9 {
            video_flags |= XC_VIDEO_FLAGS_WIDESCREEN;
        }
        match config.render_scale {
            crate::config::RenderScale::Scale1_5x720p => {
                video_flags |= XC_VIDEO_FLAGS_HDTV_720p;
            }
            crate::config::RenderScale::Scale2x1080p | crate::config::RenderScale::Scale3x1440p => {
                video_flags |= XC_VIDEO_FLAGS_HDTV_1080i | XC_VIDEO_FLAGS_HDTV_720p;
            }
            _ => {}
        }

        Self {
            display_mode: 0,
            video_flags,
            // Reserve heap for MmAllocateContiguousMemory starting at 32MB mark
            contiguous_alloc_ptr: 0x02000000,
            events: HashMap::new(),
            frame_counter: 0,
        }
    }

    pub fn handle_ordinal(
        &mut self,
        ordinal: u32,
        args: &[u32],
        mem: &mut XboxMemory,
    ) -> u32 {
        match ordinal {
            1 => {
                // AvSetDisplayMode(StepHandle, Mode, Width, Height, RefreshRate)
                let mode = args.get(1).copied().unwrap_or(0);
                let w = args.get(2).copied().unwrap_or(640);
                let h = args.get(3).copied().unwrap_or(480);
                info!("AvSetDisplayMode called: mode={}, size={}x{}", mode, w, h);
                self.display_mode = mode;
                0 // STATUS_SUCCESS
            }
            12 => {
                // DbgPrint(format, ...)
                if let Some(&msg_va) = args.first() {
                    let mut s = String::new();
                    for i in 0..256 {
                        let b = (mem.read_u32(msg_va + (i & !3)) >> ((i % 4) * 8)) as u8;
                        if b == 0 {
                            break;
                        }
                        s.push(b as char);
                    }
                    info!("[Xbox DbgPrint] {}", s.trim());
                }
                0
            }
            49 => {
                // HalReturnToFirmware
                info!("HalReturnToFirmware requested - rebooting to dashboard");
                0
            }
            89 => {
                // KeInitializeEvent(Event, Type, State)
                if let Some(&event_va) = args.first() {
                    let state = args.get(2).copied().unwrap_or(0) != 0;
                    self.events.insert(event_va, state);
                }
                0
            }
            96 => {
                // KeSetEvent(Event, Increment, Wait)
                if let Some(&event_va) = args.first() {
                    self.events.insert(event_va, true);
                }
                0
            }
            165 => {
                // MmAllocateContiguousMemory(NumberOfBytes)
                let size = args.first().copied().unwrap_or(4096);
                let aligned_size = (size + 4095) & !4095;
                let addr = self.contiguous_alloc_ptr;
                self.contiguous_alloc_ptr += aligned_size;
                info!(
                    "MmAllocateContiguousMemory: size={:#X} -> allocated VA {:#010X}",
                    size, addr
                );
                addr
            }
            170 => {
                // MmFreeContiguousMemory(BaseAddress)
                0
            }
            184 => {
                // NtAllocateVirtualMemory(ProcessHandle, BaseAddress, ZeroBits, RegionSize, AllocationType, Protect)
                let size = args.get(3).copied().unwrap_or(4096);
                let addr = self.contiguous_alloc_ptr;
                self.contiguous_alloc_ptr += (size + 4095) & !4095;
                if let Some(&base_ptr_va) = args.get(1) {
                    if base_ptr_va != 0 {
                        mem.write_u32(base_ptr_va, addr);
                    }
                }
                0 // STATUS_SUCCESS
            }
            289 => {
                // RtlInitAnsiString(DestinationString, SourceString)
                0
            }
            353 => {
                // XMountUtilityDrive
                info!("XMountUtilityDrive: mounting utility partition Z:\\");
                0
            }
            362 => {
                // XapiThreadStartup
                0
            }
            _ => {
                info!("Kernel ordinal call: {} with args: {:?}", ordinal, args);
                0
            }
        }
    }
}

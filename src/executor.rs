use crate::config::AppConfig;
use crate::gles::GlesRenderer;
use crate::kernel::KernelState;
use crate::memory::{XboxMemory, XBOX_STACK_TOP};
use crate::nv2a::Nv2aBackend;
use crate::xbe::XbeImage;
use log::{error, info};
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ExecutionState {
    Uninitialized,
    Loading,
    Running,
    Paused,
    Failed,
}

pub struct GameExecutor {
    pub state: ExecutionState,
    pub xbe: Option<XbeImage>,
    pub memory: Option<XboxMemory>,
    pub kernel: Option<KernelState>,
    pub nv2a: Option<Nv2aBackend>,
    pub status: String,
    pub simulated_eax: u32,
    pub simulated_esp: u32,
    pub simulated_eip: u32,
}

impl GameExecutor {
    pub fn new() -> Self {
        Self {
            state: ExecutionState::Uninitialized,
            xbe: None,
            memory: None,
            kernel: None,
            nv2a: None,
            status: "No game loaded".to_string(),
            simulated_eax: 0,
            simulated_esp: XBOX_STACK_TOP,
            simulated_eip: 0,
        }
    }

    pub fn start_game<P: AsRef<Path>>(
        &mut self,
        iso_or_xbe_path: P,
        config: &AppConfig,
    ) -> Result<(), String> {
        let p = iso_or_xbe_path.as_ref();
        info!("Beginning game execution sequence for: {}", p.display());
        self.state = ExecutionState::Loading;
        self.status = format!("Loading {}...", p.display());

        // Step 1: Parse XBE image
        info!("Step 1: Parsing XBE image...");
        let xbe = match XbeImage::from_iso_or_file(p) {
            Ok(img) => img,
            Err(e) => {
                let err_msg = format!("Failed to load XBE: {}", e);
                error!("{}", err_msg);
                self.state = ExecutionState::Failed;
                self.status = err_msg.clone();
                return Err(err_msg);
            }
        };

        info!(
            "XBE parsed: Title '{}' (ID {:#010X}), Entry Point {:#010X}, Sections: {}",
            xbe.title_name,
            xbe.title_id,
            xbe.entry_point,
            xbe.sections.len()
        );

        // Step 2: Initialize 64MB Xbox unified RAM
        info!("Step 2: Initializing 64MB unified memory layout...");
        let mut memory = match XboxMemory::new() {
            Ok(mem) => mem,
            Err(e) => {
                let err_msg = format!("Memory allocation error: {}", e);
                error!("{}", err_msg);
                self.state = ExecutionState::Failed;
                self.status = err_msg.clone();
                return Err(err_msg);
            }
        };

        // Step 3: Map XBE sections into guest address space
        info!("Step 3: Mapping sections into guest address space...");
        if let Err(e) = memory.load_xbe(&xbe) {
            let err_msg = format!("Section mapping error: {}", e);
            error!("{}", err_msg);
            self.state = ExecutionState::Failed;
            self.status = err_msg.clone();
            return Err(err_msg);
        }

        // Step 4: Initialize Kernel HLE and NV2A backend
        info!("Step 4: Initializing Xbox Kernel HLE and NV2A engine...");
        let kernel = KernelState::new(config);
        let mut nv2a = Nv2aBackend::new(config);
        nv2a.set_render_scale(config.render_scale);

        // Step 5: Setup guest registers
        self.simulated_esp = XBOX_STACK_TOP - 16;
        self.simulated_eip = xbe.entry_point;
        self.simulated_eax = 0;

        self.xbe = Some(xbe);
        self.memory = Some(memory);
        self.kernel = Some(kernel);
        self.nv2a = Some(nv2a);
        self.state = ExecutionState::Running;
        self.status = "Game running".to_string();

        info!("Game initialized successfully! Simulated EIP={:#010X}, ESP={:#010X}", self.simulated_eip, self.simulated_esp);

        Ok(())
    }

    pub fn tick_frame(&mut self, _config: &AppConfig) {
        if self.state != ExecutionState::Running {
            return;
        }

        if let Some(ref mut nv2a) = self.nv2a {
            nv2a.begin_render();
            nv2a.process_push_buffer();
        }

        if let Some(ref mut kernel) = self.kernel {
            kernel.frame_counter += 1;
        }
    }

    pub fn render_viewport(
        &mut self,
        gles: &mut GlesRenderer,
        config: &AppConfig,
        sw: f32,
        sh: f32,
    ) {
        if let Some(ref mut nv2a) = self.nv2a {
            nv2a.draw_viewport(gles, config, sw, sh);
        }
    }

    pub fn stop(&mut self) {
        info!("Stopping game execution...");
        self.state = ExecutionState::Uninitialized;
        self.xbe = None;
        self.memory = None;
        self.kernel = None;
        self.nv2a = None;
        self.status = "Game stopped".to_string();
    }
}

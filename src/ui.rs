use crate::config::{AppConfig, RenderScale, TextureFilter};
use crate::iso::IsoManager;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Screen {
    MainLauncher,
    GraphicsSettings,
    GameRunning,
}

pub struct LauncherUi {
    pub current_screen: Screen,
    pub status_message: String,
}

impl LauncherUi {
    pub fn new() -> Self {
        Self {
            current_screen: Screen::MainLauncher,
            status_message: "Ready to launch".to_string(),
        }
    }

    pub fn handle_launch(&mut self, iso_mgr: &IsoManager, config: &AppConfig) -> bool {
        if let Some(iso) = iso_mgr.selected() {
            self.status_message = format!("Booting {}...", iso.filename);
            self.current_screen = Screen::GameRunning;
            true
        } else if !config.iso_path.is_empty() {
            self.status_message = format!("Booting configured ISO: {}...", config.iso_path);
            self.current_screen = Screen::GameRunning;
            true
        } else {
            self.status_message = "Error: No Xbox ISO selected!".to_string();
            false
        }
    }

    pub fn cycle_resolution(&mut self, config: &mut AppConfig) {
        config.render_scale = match config.render_scale {
            RenderScale::Scale1x480p => RenderScale::Scale1_5x720p,
            RenderScale::Scale1_5x720p => RenderScale::Scale2x1080p,
            RenderScale::Scale2x1080p => RenderScale::Scale3x1440p,
            RenderScale::Scale3x1440p => RenderScale::Scale1x480p,
        };
    }

    pub fn toggle_widescreen(&mut self, config: &mut AppConfig) {
        config.widescreen_16_9 = !config.widescreen_16_9;
    }

    pub fn cycle_fps_limit(&mut self, config: &mut AppConfig) {
        config.fps_limit = match config.fps_limit {
            30 => 60,
            60 => 0,
            _ => 30,
        };
    }

    pub fn cycle_texture_filter(&mut self, config: &mut AppConfig) {
        config.texture_filter = match config.texture_filter {
            TextureFilter::Bilinear => TextureFilter::Anisotropic8x,
            TextureFilter::Anisotropic8x => TextureFilter::Anisotropic16x,
            TextureFilter::Anisotropic16x => TextureFilter::Bilinear,
        };
    }

    pub fn toggle_touch_controls(&mut self, config: &mut AppConfig) {
        config.touch_controls = !config.touch_controls;
    }
}

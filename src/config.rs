use serde::{Deserialize, Serialize};
use std::fs;
use std::path::Path;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum RenderScale {
    Scale1x480p = 1,
    Scale1_5x720p = 2,
    Scale2x1080p = 3,
    Scale3x1440p = 4,
}

impl RenderScale {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Scale1x480p => "1.0x (480p Native)",
            Self::Scale1_5x720p => "1.5x (720p HD)",
            Self::Scale2x1080p => "2.0x (1080p Full HD)",
            Self::Scale3x1440p => "3.0x (1440p 2K)",
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
pub enum TextureFilter {
    Bilinear = 0,
    Anisotropic8x = 1,
    Anisotropic16x = 2,
}

impl TextureFilter {
    pub fn label(&self) -> &'static str {
        match self {
            Self::Bilinear => "Bilinear",
            Self::Anisotropic8x => "Anisotropic 8x",
            Self::Anisotropic16x => "Anisotropic 16x",
        }
    }
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    pub iso_path: String,
    pub save_dir: String,
    pub render_scale: RenderScale,
    pub widescreen_16_9: bool,
    pub fps_limit: u32,
    pub vsync: bool,
    pub texture_filter: TextureFilter,
    pub touch_controls: bool,
    pub touch_opacity: f32,
}

impl Default for AppConfig {
    fn default() -> Self {
        Self {
            iso_path: "/sdcard/mc3dr/Midnight Club 3 - DUB Edition Remix (Europe) (En,Fr,De,Es,It).iso".to_string(),
            save_dir: "/sdcard/mc3dr/saves".to_string(),
            render_scale: RenderScale::Scale2x1080p,
            widescreen_16_9: true,
            fps_limit: 60,
            vsync: true,
            texture_filter: TextureFilter::Anisotropic8x,
            touch_controls: false,
            touch_opacity: 0.75,
        }
    }
}

impl AppConfig {
    pub fn load(path: &Path) -> Self {
        if let Ok(data) = fs::read_to_string(path) {
            if let Ok(cfg) = serde_json::from_str(&data) {
                return cfg;
            }
        }
        Self::default()
    }

    pub fn save(&self, path: &Path) -> Result<(), std::io::Error> {
        if let Some(parent) = path.parent() {
            let _ = fs::create_dir_all(parent);
        }
        let data = serde_json::to_string_pretty(self)
            .map_err(|e| std::io::Error::new(std::io::ErrorKind::Other, e))?;
        fs::write(path, data)
    }
}

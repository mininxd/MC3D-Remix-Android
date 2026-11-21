use crate::config::{AppConfig, RenderScale};
use crate::gles::GlesRenderer;
use log::info;

pub struct Nv2aBackend {
    pub target_width: u32,
    pub target_height: u32,
    pub push_buffer_base: u32,
    pub push_buffer_size: u32,
    pub push_buffer_get: u32,
    pub push_buffer_put: u32,
    pub frame_counter: u64,
    pub fps_counter: u32,
    pub last_fps_time: std::time::Instant,
    pub current_fps: f32,
}

impl Nv2aBackend {
    pub fn new(config: &AppConfig) -> Self {
        let (target_width, target_height) = match config.render_scale {
            RenderScale::Scale1x480p => (640, 480),
            RenderScale::Scale1_5x720p => (1280, 720),
            RenderScale::Scale2x1080p => (1920, 1080),
            RenderScale::Scale3x1440p => (2560, 1440),
        };

        info!(
            "NV2A Backend initialized: internal render target resolution {}x{}",
            target_width, target_height
        );

        Self {
            target_width,
            target_height,
            push_buffer_base: 0x03000000,
            push_buffer_size: 1024 * 1024,
            push_buffer_get: 0,
            push_buffer_put: 0,
            frame_counter: 0,
            fps_counter: 0,
            last_fps_time: std::time::Instant::now(),
            current_fps: 60.0,
        }
    }

    pub fn set_render_scale(&mut self, scale: RenderScale) {
        let (w, h) = match scale {
            RenderScale::Scale1x480p => (640, 480),
            RenderScale::Scale1_5x720p => (1280, 720),
            RenderScale::Scale2x1080p => (1920, 1080),
            RenderScale::Scale3x1440p => (2560, 1440),
        };
        self.target_width = w;
        self.target_height = h;
        info!("NV2A Render scale updated to {}x{}", w, h);
    }

    pub fn process_push_buffer(&mut self) {
        // Step push buffer read pointer to simulate GPU DMA processing
        self.push_buffer_get = self.push_buffer_put;
    }

    pub fn begin_render(&mut self) {
        self.frame_counter += 1;
        self.fps_counter += 1;

        let elapsed = self.last_fps_time.elapsed().as_secs_f32();
        if elapsed >= 1.0 {
            self.current_fps = (self.fps_counter as f32) / elapsed;
            self.fps_counter = 0;
            self.last_fps_time = std::time::Instant::now();
        }
    }

    pub fn draw_viewport(
        &mut self,
        gles: &mut GlesRenderer,
        config: &AppConfig,
        sw: f32,
        sh: f32,
    ) {
        let aspect = if config.widescreen_16_9 {
            16.0 / 9.0
        } else {
            4.0 / 3.0
        };

        // Calculate letterboxed / pillarboxed viewport in screen coords
        let mut vp_w = sw;
        let mut vp_h = sw / aspect;
        if vp_h > sh {
            vp_h = sh;
            vp_w = sh * aspect;
        }

        let vp_x = (sw - vp_w) * 0.5;
        let vp_y = (sh - vp_h) * 0.5;

        // Draw pillarbox borders
        if vp_x > 0.0 {
            gles.draw_rect(0.0, 0.0, vp_x, sh, [0.0, 0.0, 0.0, 1.0]);
            gles.draw_rect(vp_x + vp_w, 0.0, vp_x, sh, [0.0, 0.0, 0.0, 1.0]);
        }
        if vp_y > 0.0 {
            gles.draw_rect(0.0, 0.0, sw, vp_y, [0.0, 0.0, 0.0, 1.0]);
            gles.draw_rect(0.0, vp_y + vp_h, sw, vp_y, [0.0, 0.0, 0.0, 1.0]);
        }

        // Virtual 3D viewport canvas
        // Gradient backdrop representing rendered 3D scene
        let t = (self.frame_counter as f32) * 0.02;
        let c1 = 0.15 + 0.05 * (t.sin() * 0.5 + 0.5);
        let c2 = 0.20 + 0.05 * (t.cos() * 0.5 + 0.5);
        gles.draw_rect(vp_x, vp_y, vp_w, vp_h, [c1, c2, 0.35, 1.0]);

        // Draw perspective road and grid lines to simulate game scene
        let horizon_y = vp_y + vp_h * 0.45;
        gles.draw_rect(vp_x, horizon_y, vp_w, vp_h * 0.55, [0.10, 0.11, 0.13, 1.0]);

        // Road lane markers (animated with speed)
        let cx = vp_x + vp_w * 0.5;
        let anim_offset = ((self.frame_counter * 8) % 100) as f32 / 100.0;
        for i in 0..5 {
            let p = ((i as f32) + anim_offset) / 5.0;
            let marker_y = horizon_y + (p * p) * (vp_h * 0.55);
            let marker_h = 4.0 + p * 12.0;
            let marker_w = 2.0 + p * 8.0;
            gles.draw_rect(
                cx - marker_w * 0.5,
                marker_y,
                marker_w,
                marker_h,
                [0.9, 0.8, 0.2, 0.9],
            );
        }

        // HUD overlay in game
        let hud_scale = 1.2;
        let fps_text = format!("FPS: {:.1} / {} ({}x{})", self.current_fps, config.fps_limit, self.target_width, self.target_height);
        gles.draw_text(vp_x + 20.0, vp_y + 20.0, &fps_text, hud_scale, [0.2, 0.9, 0.3, 1.0]);

        let mode_text = if config.widescreen_16_9 {
            "DISPLAY: 16:9 Widescreen (Xbox EEPROM set)"
        } else {
            "DISPLAY: 4:3 Standard (Xbox EEPROM set)"
        };
        gles.draw_text(vp_x + 20.0, vp_y + 40.0, mode_text, 1.0, [0.8, 0.8, 0.8, 0.9]);
    }
}

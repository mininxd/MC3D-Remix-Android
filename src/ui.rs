use crate::config::{AppConfig, RenderScale, TextureFilter};
use crate::executor::GameExecutor;
use crate::gles::GlesRenderer;
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
    pub frame_count: u64,
}

struct Button {
    x: f32,
    y: f32,
    w: f32,
    h: f32,
}

impl Button {
    fn contains(&self, px: f32, py: f32) -> bool {
        px >= self.x && px <= self.x + self.w && py >= self.y && py <= self.y + self.h
    }
}

impl LauncherUi {
    pub fn new() -> Self {
        Self {
            current_screen: Screen::MainLauncher,
            status_message: "Select ISO and adjust graphics settings".to_string(),
            frame_count: 0,
        }
    }

    pub fn render(
        &mut self,
        gles: &mut GlesRenderer,
        config: &mut AppConfig,
        iso_mgr: &mut IsoManager,
        executor: &mut GameExecutor,
        touch: Option<(f32, f32)>,
        touch_pressed: bool,
    ) {
        self.frame_count += 1;
        let sw = gles.width as f32;
        let sh = gles.height as f32;

        match self.current_screen {
            Screen::MainLauncher | Screen::GraphicsSettings => {
                self.render_main_and_settings(gles, config, iso_mgr, executor, sw, sh, touch, touch_pressed);
            }
            Screen::GameRunning => {
                self.render_game_running(gles, config, executor, sw, sh, touch, touch_pressed);
            }
        }
    }

    fn render_main_and_settings(
        &mut self,
        gles: &mut GlesRenderer,
        config: &mut AppConfig,
        iso_mgr: &mut IsoManager,
        executor: &mut GameExecutor,
        sw: f32,
        sh: f32,
        touch: Option<(f32, f32)>,
        touch_pressed: bool,
    ) {
        // Top Header Banner
        gles.draw_rect(0.0, 0.0, sw, 80.0, [0.12, 0.14, 0.18, 1.0]);
        gles.draw_rect(0.0, 78.0, sw, 2.0, [0.20, 0.60, 0.90, 1.0]);

        gles.draw_text(30.0, 20.0, "MIDNIGHT CLUB 3: DUB EDITION REMIX", 2.2, [1.0, 1.0, 1.0, 1.0]);
        gles.draw_text(30.0, 50.0, "Android Native Static Recompilation Launcher", 1.4, [0.6, 0.7, 0.8, 1.0]);

        // Panel 1: ISO Selector Card
        let card1_x = 30.0;
        let card1_y = 100.0;
        let card1_w = sw - 60.0;
        let card1_h = 130.0;

        gles.draw_rect(card1_x, card1_y, card1_w, card1_h, [0.15, 0.17, 0.22, 1.0]);
        gles.draw_rect_outline(card1_x, card1_y, card1_w, card1_h, [0.25, 0.30, 0.38, 1.0], 1.5);

        gles.draw_text(card1_x + 20.0, card1_y + 15.0, "GAME ISO FILE:", 1.5, [0.2, 0.8, 1.0, 1.0]);

        let iso_display = if let Some(selected) = iso_mgr.selected() {
            format!("[XBOX ISO] {}", selected.filename)
        } else if !config.iso_path.is_empty() {
            format!("[PATH] {}", config.iso_path)
        } else {
            "NO ISO FOUND - Place in /sdcard/xbox/".to_string()
        };
        gles.draw_text(card1_x + 20.0, card1_y + 45.0, &iso_display, 1.6, [1.0, 1.0, 0.9, 1.0]);

        // Prev & Next ISO Buttons
        let prev_btn = Button { x: card1_x + 20.0, y: card1_y + 80.0, w: 140.0, h: 36.0 };
        let next_btn = Button { x: card1_x + 180.0, y: card1_y + 80.0, w: 140.0, h: 36.0 };
        let scan_btn = Button { x: card1_x + 340.0, y: card1_y + 80.0, w: 140.0, h: 36.0 };

        let prev_pressed = touch.map(|(tx, ty)| prev_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        let next_pressed = touch.map(|(tx, ty)| next_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        let scan_pressed = touch.map(|(tx, ty)| scan_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);

        if prev_pressed {
            iso_mgr.select_prev();
            if let Some(s) = iso_mgr.selected() {
                config.iso_path = s.path.to_string_lossy().to_string();
            }
        }
        if next_pressed {
            iso_mgr.select_next();
            if let Some(s) = iso_mgr.selected() {
                config.iso_path = s.path.to_string_lossy().to_string();
            }
        }
        if scan_pressed {
            iso_mgr.scan();
        }

        draw_ui_button(gles, &prev_btn, "< PREV ISO", prev_pressed, [0.2, 0.4, 0.6, 1.0]);
        draw_ui_button(gles, &next_btn, "NEXT ISO >", next_pressed, [0.2, 0.4, 0.6, 1.0]);
        draw_ui_button(gles, &scan_btn, "RESCAN", scan_pressed, [0.3, 0.3, 0.4, 1.0]);

        // Panel 2: Graphics & Display Settings Card
        let card2_x = 30.0;
        let card2_y = 250.0;
        let card2_w = sw - 60.0;
        let card2_h = 240.0;

        gles.draw_rect(card2_x, card2_y, card2_w, card2_h, [0.15, 0.17, 0.22, 1.0]);
        gles.draw_rect_outline(card2_x, card2_y, card2_w, card2_h, [0.25, 0.30, 0.38, 1.0], 1.5);

        gles.draw_text(card2_x + 20.0, card2_y + 15.0, "GRAPHICS & DISPLAY SETTINGS:", 1.5, [0.2, 0.8, 1.0, 1.0]);

        // Option 1: Resolution Scale
        let res_btn = Button { x: card2_x + 20.0, y: card2_y + 50.0, w: 340.0, h: 42.0 };
        let res_pressed = touch.map(|(tx, ty)| res_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if res_pressed {
            self.cycle_resolution(config);
        }
        let res_label = format!("RESOLUTION: {}", config.render_scale.label());
        draw_ui_button(gles, &res_btn, &res_label, res_pressed, [0.22, 0.28, 0.36, 1.0]);

        // Option 2: Widescreen 16:9
        let ws_btn = Button { x: card2_x + 380.0, y: card2_y + 50.0, w: 320.0, h: 42.0 };
        let ws_pressed = touch.map(|(tx, ty)| ws_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if ws_pressed {
            config.widescreen_16_9 = !config.widescreen_16_9;
        }
        let ws_label = format!("ASPECT: {}", if config.widescreen_16_9 { "16:9 Widescreen" } else { "4:3 Standard" });
        draw_ui_button(gles, &ws_btn, &ws_label, ws_pressed, if config.widescreen_16_9 { [0.2, 0.5, 0.3, 1.0] } else { [0.3, 0.3, 0.3, 1.0] });

        // Option 3: FPS Limit
        let fps_btn = Button { x: card2_x + 20.0, y: card2_y + 110.0, w: 340.0, h: 42.0 };
        let fps_pressed = touch.map(|(tx, ty)| fps_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if fps_pressed {
            config.fps_limit = match config.fps_limit {
                30 => 60,
                60 => 0,
                _ => 30,
            };
        }
        let fps_label = format!("FRAME RATE: {}", if config.fps_limit == 0 { "Uncapped".to_string() } else { format!("{} FPS", config.fps_limit) });
        draw_ui_button(gles, &fps_btn, &fps_label, fps_pressed, [0.22, 0.28, 0.36, 1.0]);

        // Option 4: VSync
        let vsync_btn = Button { x: card2_x + 380.0, y: card2_y + 110.0, w: 320.0, h: 42.0 };
        let vsync_pressed = touch.map(|(tx, ty)| vsync_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if vsync_pressed {
            config.vsync = !config.vsync;
        }
        let vsync_label = format!("VSYNC: {}", if config.vsync { "ENABLED" } else { "DISABLED" });
        draw_ui_button(gles, &vsync_btn, &vsync_label, vsync_pressed, if config.vsync { [0.2, 0.5, 0.3, 1.0] } else { [0.5, 0.2, 0.2, 1.0] });

        // Option 5: Texture Filtering
        let tex_btn = Button { x: card2_x + 20.0, y: card2_y + 170.0, w: 340.0, h: 42.0 };
        let tex_pressed = touch.map(|(tx, ty)| tex_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if tex_pressed {
            config.texture_filter = match config.texture_filter {
                TextureFilter::Bilinear => TextureFilter::Anisotropic8x,
                TextureFilter::Anisotropic8x => TextureFilter::Anisotropic16x,
                TextureFilter::Anisotropic16x => TextureFilter::Bilinear,
            };
        }
        let tex_label = format!("FILTERING: {}", config.texture_filter.label());
        draw_ui_button(gles, &tex_btn, &tex_label, tex_pressed, [0.22, 0.28, 0.36, 1.0]);

        // Option 6: Virtual Touch Controls
        let touch_btn = Button { x: card2_x + 380.0, y: card2_y + 170.0, w: 320.0, h: 42.0 };
        let touch_ctrl_pressed = touch.map(|(tx, ty)| touch_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if touch_ctrl_pressed {
            config.touch_controls = !config.touch_controls;
        }
        let touch_label = format!("TOUCH OVERLAY: {}", if config.touch_controls { "ON" } else { "OFF" });
        draw_ui_button(gles, &touch_btn, &touch_label, touch_ctrl_pressed, if config.touch_controls { [0.2, 0.5, 0.3, 1.0] } else { [0.3, 0.3, 0.3, 1.0] });

        // Big LAUNCH Button
        let launch_btn = Button {
            x: 30.0,
            y: card2_y + card2_h + 20.0,
            w: sw - 60.0,
            h: 70.0,
        };
        let launch_pressed = touch.map(|(tx, ty)| launch_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if launch_pressed {
            self.handle_launch(iso_mgr, config, executor);
        }

        draw_ui_button(
            gles,
            &launch_btn,
            ">>> LAUNCH MIDNIGHT CLUB 3 <<<",
            launch_pressed,
            [0.10, 0.70, 0.30, 1.0],
        );

        // Status bar at the bottom
        gles.draw_rect(0.0, sh - 30.0, sw, 30.0, [0.1, 0.1, 0.1, 1.0]);
        gles.draw_text(15.0, sh - 22.0, &format!("Status: {}", self.status_message), 1.2, [0.7, 0.7, 0.7, 1.0]);
    }

    fn render_game_running(
        &mut self,
        gles: &mut GlesRenderer,
        config: &AppConfig,
        executor: &mut GameExecutor,
        sw: f32,
        sh: f32,
        touch: Option<(f32, f32)>,
        touch_pressed: bool,
    ) {
        // Step execution and render game 3D viewport
        executor.tick_frame(config);
        executor.render_viewport(gles, config, sw, sh);

        // Draw In-Game Info Header
        gles.draw_rect(0.0, 0.0, sw, 40.0, [0.1, 0.1, 0.1, 0.8]);
        gles.draw_text(20.0, 12.0, "Midnight Club 3: DUB Edition Remix (Game Running)", 1.4, [0.2, 0.8, 1.0, 1.0]);

        let res_info = format!("Internal Render: {}", config.render_scale.label());
        gles.draw_text(sw - 360.0, 12.0, &res_info, 1.2, [0.7, 0.7, 0.7, 1.0]);

        // In-game Settings / Pause floating button
        let pause_btn = Button { x: sw - 120.0, y: 8.0, w: 100.0, h: 26.0 };
        let pause_pressed = touch.map(|(tx, ty)| pause_btn.contains(tx, ty) && touch_pressed).unwrap_or(false);
        if pause_pressed {
            self.current_screen = Screen::MainLauncher;
            self.status_message = "Returned to Launcher Settings".to_string();
        }
        draw_ui_button(gles, &pause_btn, "OPTIONS", pause_pressed, [0.3, 0.4, 0.5, 1.0]);

        // Virtual Touch Overlay (if enabled)
        if config.touch_controls {
            self.draw_virtual_gamepad(gles, sw, sh, config.touch_opacity);
        }
    }

    fn draw_virtual_gamepad(&self, gles: &mut GlesRenderer, sw: f32, sh: f32, opacity: f32) {
        // Left Analog Stick / D-Pad area
        let stick_x = 100.0;
        let stick_y = sh - 100.0;
        gles.draw_rect_outline(stick_x - 50.0, stick_y - 50.0, 100.0, 100.0, [1.0, 1.0, 1.0, opacity * 0.5], 2.0);
        gles.draw_rect(stick_x - 20.0, stick_y - 20.0, 40.0, 40.0, [0.3, 0.6, 0.9, opacity * 0.7]);
        gles.draw_text(stick_x - 16.0, stick_y - 4.0, "STEER", 1.0, [1.0, 1.0, 1.0, opacity]);

        // Right Action Buttons (A = Gas, X = Brake, B = Handbrake, Y = Nitro)
        let right_x = sw - 100.0;
        let right_y = sh - 100.0;

        // Button A (Gas)
        draw_gamepad_btn(gles, right_x, right_y + 35.0, "A (GAS)", [0.2, 0.8, 0.2, opacity]);
        // Button X (Brake)
        draw_gamepad_btn(gles, right_x - 45.0, right_y, "X (BRK)", [0.2, 0.4, 0.9, opacity]);
        // Button B (Handbrake)
        draw_gamepad_btn(gles, right_x + 45.0, right_y, "B (E-B)", [0.9, 0.2, 0.2, opacity]);
        // Button Y (Nitrous)
        draw_gamepad_btn(gles, right_x, right_y - 35.0, "Y (NOS)", [0.9, 0.9, 0.2, opacity]);
    }

    pub fn handle_launch(
        &mut self,
        iso_mgr: &IsoManager,
        config: &AppConfig,
        executor: &mut GameExecutor,
    ) -> bool {
        let target_path = if let Some(iso) = iso_mgr.selected() {
            Some(iso.path.clone())
        } else if !config.iso_path.is_empty() {
            Some(std::path::PathBuf::from(&config.iso_path))
        } else {
            None
        };

        if let Some(path) = target_path {
            self.status_message = format!("Initializing {}...", path.display());
            match executor.start_game(&path, config) {
                Ok(()) => {
                    self.status_message = format!(
                        "Running {}!",
                        path.file_name().unwrap_or_default().to_string_lossy()
                    );
                    self.current_screen = Screen::GameRunning;
                    true
                }
                Err(e) => {
                    self.status_message = format!("Launch Error: {}", e);
                    false
                }
            }
        } else {
            self.status_message = "Error: Please place ISO in /sdcard/xbox/".to_string();
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
}

fn draw_ui_button(gles: &mut GlesRenderer, btn: &Button, label: &str, pressed: bool, base_color: [f32; 4]) {
    let color = if pressed {
        [
            (base_color[0] + 0.2).min(1.0),
            (base_color[1] + 0.2).min(1.0),
            (base_color[2] + 0.2).min(1.0),
            1.0,
        ]
    } else {
        base_color
    };

    gles.draw_rect(btn.x, btn.y, btn.w, btn.h, color);
    gles.draw_rect_outline(btn.x, btn.y, btn.w, btn.h, [1.0, 1.0, 1.0, 0.4], 1.5);

    let text_scale = 1.3;
    let text_w = (label.len() as f32) * 9.0 * text_scale;
    let text_x = btn.x + (btn.w - text_w) * 0.5;
    let text_y = btn.y + (btn.h - (8.0 * text_scale)) * 0.5;

    gles.draw_text(text_x.max(btn.x + 8.0), text_y, label, text_scale, [1.0, 1.0, 1.0, 1.0]);
}

fn draw_gamepad_btn(gles: &mut GlesRenderer, cx: f32, cy: f32, label: &str, color: [f32; 4]) {
    let size = 32.0;
    gles.draw_rect(cx - size * 0.5, cy - size * 0.5, size, size, color);
    gles.draw_rect_outline(cx - size * 0.5, cy - size * 0.5, size, size, [1.0, 1.0, 1.0, 0.6], 1.5);
    gles.draw_text(cx - 16.0, cy - 4.0, label, 0.8, [1.0, 1.0, 1.0, 1.0]);
}

pub mod android_permissions;
pub mod config;
pub mod executor;
pub mod gles;
pub mod iso;
pub mod kernel;
pub mod logger;
pub mod memory;
pub mod nv2a;
pub mod ui;
pub mod xbe;

use android_activity::input::{InputEvent, KeyAction, Keycode, MotionAction};
use android_activity::{AndroidApp, InputStatus, MainEvent, PollEvent};
use config::AppConfig;
use executor::GameExecutor;
use gles::GlesRenderer;
use iso::IsoManager;
use log::{error, info};
use std::ffi::c_void;
use std::path::PathBuf;
use std::time::Duration;
use ui::{LauncherUi, Screen};

#[no_mangle]
fn android_main(app: AndroidApp) {
    // Initialize unified persistent file + logcat logger
    logger::init(&app);

    info!("============================================================");
    info!(" Midnight Club 3: DUB Edition Remix - Pure Rust Launcher");
    info!(" Built with android-activity, 100% Rust, zero Java / C");
    info!(" Persistent log target: /sdcard/mc3dr/app_logs.txt");
    info!("============================================================");

    let internal_dir = app
        .internal_data_path()
        .unwrap_or_else(|| PathBuf::from("/data/data/io.github.mininxd.midnightclub3remixrecomp/files"));
    info!("App internal data path: {}", internal_dir.display());
    if let Some(ext) = app.external_data_path() {
        info!("App external data path: {}", ext.display());
    }

    let config_path = internal_dir.join("mc3dr_config.json");
    let mut config = AppConfig::load(&config_path);
    info!(
        "Loaded config: render_scale={:?}, widescreen={}, fps_limit={}, touch_controls={}",
        config.render_scale, config.widescreen_16_9, config.fps_limit, config.touch_controls
    );

    let mut iso_mgr = IsoManager::new();
    info!("Discovered {} Xbox game target(s)", iso_mgr.items.len());
    for (i, item) in iso_mgr.items.iter().enumerate() {
        info!("  [{}] {} (valid_xdvdfs={})", i, item.filename, item.is_valid_xdvdfs);
    }

    let mut ui = LauncherUi::new();
    let mut executor = GameExecutor::new();
    let mut renderer: Option<GlesRenderer> = None;

    // AUTO-DETECT in /sdcard/mc3dr (default.xbe or ISO): If found, automatically launch it!
    if let Some(auto_target) = iso_mgr.find_mc3dr_iso() {
        info!("Auto-detected Midnight Club 3 target in /sdcard/mc3dr: {}", auto_target.path.display());
        config.iso_path = auto_target.path.to_string_lossy().to_string();
        match executor.start_game(&auto_target.path, &config) {
            Ok(()) => {
                info!("Auto-launch initiated successfully for: {}", auto_target.filename);
                ui.current_screen = Screen::GameRunning;
                ui.status_message = format!("Running {} from /sdcard/mc3dr", auto_target.filename);
            }
            Err(e) => {
                error!("Auto-launch error: {}", e);
                ui.status_message = format!("Auto-launch failed: {}", e);
            }
        }
    } else {
        info!("No game files found in /sdcard/mc3dr on initial boot scan. Showing launcher UI.");
        ui.status_message = "Place game files in /sdcard/mc3dr/ or tap STORAGE PERM".to_string();
    }

    let mut touch_pos: Option<(f32, f32)> = None;
    let mut touch_pressed = false;

    let mut running = true;
    while running {
        app.poll_events(Some(Duration::from_millis(16)), |event| {
            if let PollEvent::Main(main_event) = event {
                match main_event {
                    MainEvent::InitWindow { .. } => {
                        info!("Native Window initialized. Setting up display viewport.");
                        if let Some(win) = app.native_window() {
                            let width = win.width();
                            let height = win.height();
                            let ptr = win.ptr().as_ptr() as *mut c_void;
                            info!("Creating GLES renderer for window: {}x{}", width, height);
                            renderer = GlesRenderer::new(ptr, width, height);
                            if renderer.is_some() {
                                info!("GLES renderer initialized successfully!");
                            } else {
                                error!("Failed to initialize GLES renderer");
                            }
                        }
                    }
                    MainEvent::WindowResized { .. } => {
                        if let (Some(ref mut r), Some(win)) = (&mut renderer, app.native_window()) {
                            let w = win.width();
                            let h = win.height();
                            info!("Window resized to {}x{}", w, h);
                            r.resize(w, h);
                        }
                    }
                    MainEvent::RedrawNeeded { .. } => {}
                    MainEvent::TerminateWindow { .. } => {
                        info!("Native Window terminated. Dropping renderer.");
                        renderer = None;
                    }
                    MainEvent::Pause => {
                        info!("App paused. Saving config.");
                        if let Err(e) = config.save(&config_path) {
                            error!("Failed to save config: {}", e);
                        }
                    }
                    MainEvent::Resume { .. } => {
                        info!("App resumed. Rescanning storage for ISOs.");
                        iso_mgr.scan();
                        if ui.current_screen != Screen::GameRunning {
                            if let Some(auto_iso) = iso_mgr.find_mc3dr_iso() {
                                info!("Found ISO after resume: {}", auto_iso.filename);
                                config.iso_path = auto_iso.path.to_string_lossy().to_string();
                                if executor.start_game(&auto_iso.path, &config).is_ok() {
                                    ui.current_screen = Screen::GameRunning;
                                    ui.status_message = format!("Running {} from /sdcard/mc3dr", auto_iso.filename);
                                }
                            }
                        }
                    }
                    MainEvent::Destroy => {
                        info!("App exiting.");
                        let _ = config.save(&config_path);
                        running = false;
                    }
                    MainEvent::InputAvailable => {
                        if let Ok(mut iter) = app.input_events_iter() {
                            while iter.next(|event| {
                                match event {
                                    InputEvent::MotionEvent(motion) => {
                                        let action = motion.action();
                                        let pointer = motion.pointer_at_index(0);
                                        touch_pos = Some((pointer.x(), pointer.y()));
                                        match action {
                                            MotionAction::Down => {
                                                touch_pressed = true;
                                            }
                                            MotionAction::Up | MotionAction::Cancel => {
                                                touch_pressed = false;
                                            }
                                            _ => {}
                                        }
                                        InputStatus::Handled
                                    }
                                    InputEvent::KeyEvent(key) => {
                                        let action = key.action();
                                        let code = key.key_code();
                                        if action == KeyAction::Down {
                                            match code {
                                                Keycode::ButtonA | Keycode::ButtonStart => {
                                                    if ui.current_screen != Screen::GameRunning {
                                                        if ui.handle_launch(&iso_mgr, &config, &mut executor) {
                                                            ui.current_screen = Screen::GameRunning;
                                                            ui.status_message = "Running Midnight Club 3 Remix".to_string();
                                                        }
                                                    }
                                                }
                                                Keycode::ButtonB => {
                                                    if ui.current_screen == Screen::GameRunning {
                                                        ui.current_screen = Screen::MainLauncher;
                                                        ui.status_message = "Paused - Options".to_string();
                                                    }
                                                }
                                                Keycode::DpadLeft | Keycode::ButtonL1 => {
                                                    if ui.current_screen != Screen::GameRunning {
                                                        iso_mgr.select_prev();
                                                        if let Some(s) = iso_mgr.selected() {
                                                            config.iso_path = s.path.to_string_lossy().to_string();
                                                        }
                                                    }
                                                }
                                                Keycode::DpadRight | Keycode::ButtonR1 => {
                                                    if ui.current_screen != Screen::GameRunning {
                                                        iso_mgr.select_next();
                                                        if let Some(s) = iso_mgr.selected() {
                                                            config.iso_path = s.path.to_string_lossy().to_string();
                                                        }
                                                    }
                                                }
                                                Keycode::ButtonY => {
                                                    if ui.current_screen != Screen::GameRunning {
                                                        ui.cycle_resolution(&mut config);
                                                    }
                                                }
                                                Keycode::ButtonX => {
                                                    if ui.current_screen != Screen::GameRunning {
                                                        config.widescreen_16_9 = !config.widescreen_16_9;
                                                    }
                                                }
                                                _ => {}
                                            }
                                        }
                                        InputStatus::Handled
                                    }
                                    _ => InputStatus::Unhandled,
                                }
                            }) {}
                        }
                    }
                    _ => {}
                }
            }
        });

        // Ensure renderer exists if native window is attached
        if renderer.is_none() {
            if let Some(win) = app.native_window() {
                let width = win.width();
                let height = win.height();
                let ptr = win.ptr().as_ptr() as *mut c_void;
                renderer = GlesRenderer::new(ptr, width, height);
            }
        }

        // Handle user requests from UI (File picker, permissions)
        if ui.request_file_picker {
            ui.request_file_picker = false;
            android_permissions::open_system_file_picker(&app);
        }
        if ui.request_permission {
            ui.request_permission = false;
            android_permissions::request_storage_permissions(&app);
        }

        // Render UI frame if renderer is active
        if let Some(ref mut r) = renderer {
            r.begin_frame(0.08, 0.09, 0.12);
            ui.render(r, &mut config, &mut iso_mgr, &mut executor, touch_pos, touch_pressed);
            r.end_frame();
            if touch_pressed {
                touch_pressed = false;
            }
        }
    }

    info!("Clean shutdown complete.");
}

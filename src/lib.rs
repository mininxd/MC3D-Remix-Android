pub mod config;
pub mod executor;
pub mod gles;
pub mod iso;
pub mod kernel;
pub mod memory;
pub mod nv2a;
pub mod ui;
pub mod xbe;

use android_activity::input::{InputEvent, MotionAction};
use android_activity::{AndroidApp, InputStatus, MainEvent, PollEvent};
use config::AppConfig;
use executor::GameExecutor;
use gles::GlesRenderer;
use iso::IsoManager;
use log::{error, info};
use std::ffi::c_void;
use std::path::PathBuf;
use std::time::Duration;
use ui::LauncherUi;

#[no_mangle]
fn android_main(app: AndroidApp) {
    android_logger::init_once(
        android_logger::Config::default()
            .with_max_level(log::LevelFilter::Info)
            .with_tag("MC3DR_Rust"),
    );

    info!("============================================================");
    info!(" Midnight Club 3: DUB Edition Remix - Pure Rust Launcher");
    info!(" Built with android-activity, 100% Rust, zero Java / C");
    info!("============================================================");

    let internal_dir = app
        .internal_data_path()
        .unwrap_or_else(|| PathBuf::from("/data/data/io.github.mininxd.midnightclub3remixrecomp/files"));
    let config_path = internal_dir.join("mc3dr_config.json");

    let mut config = AppConfig::load(&config_path);
    info!(
        "Loaded config: render_scale={:?}, widescreen={}, fps_limit={}",
        config.render_scale, config.widescreen_16_9, config.fps_limit
    );

    let mut iso_mgr = IsoManager::new();
    info!("Discovered {} Xbox game ISO(s)", iso_mgr.items.len());
    for (i, item) in iso_mgr.items.iter().enumerate() {
        info!("  [{}] {} (valid_xdvdfs={})", i, item.filename, item.is_valid_xdvdfs);
    }

    let mut ui = LauncherUi::new();
    let mut executor = GameExecutor::new();
    let mut renderer: Option<GlesRenderer> = None;

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
                            info!("Creating GLES3 renderer for window: {}x{}", width, height);
                            renderer = GlesRenderer::new(ptr, width, height);
                            if renderer.is_some() {
                                info!("GLES3 renderer initialized successfully!");
                            } else {
                                error!("Failed to initialize GLES3 renderer");
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
                        info!("App resumed.");
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
                                    _ => InputStatus::Unhandled,
                                }
                            }) {}
                        }
                    }
                    _ => {}
                }
            }
        });

        // Render UI frame if renderer is active
        if let Some(ref mut r) = renderer {
            r.begin_frame(0.08, 0.09, 0.12);
            ui.render(r, &mut config, &mut iso_mgr, &mut executor, touch_pos, touch_pressed);
            r.end_frame();
            // Reset touch pressed after consumption
            if touch_pressed {
                touch_pressed = false;
            }
        }
    }

    info!("Clean shutdown complete.");
}

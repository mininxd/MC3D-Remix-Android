use std::ffi::{c_char, c_void, CString};
use std::ptr;

// EGL constants
const EGL_DEFAULT_DISPLAY: *mut c_void = ptr::null_mut();
const EGL_NO_CONTEXT: *mut c_void = ptr::null_mut();
const EGL_NO_SURFACE: *mut c_void = ptr::null_mut();
const EGL_NO_DISPLAY: *mut c_void = ptr::null_mut();

const EGL_RED_SIZE: i32 = 0x3024;
const EGL_GREEN_SIZE: i32 = 0x3023;
const EGL_BLUE_SIZE: i32 = 0x3022;
const EGL_ALPHA_SIZE: i32 = 0x3021;
const EGL_DEPTH_SIZE: i32 = 0x3025;
const EGL_RENDERABLE_TYPE: i32 = 0x3040;
const EGL_OPENGL_ES3_BIT: i32 = 0x00000040;
const EGL_NONE: i32 = 0x3038;
const EGL_CONTEXT_CLIENT_VERSION: i32 = 0x3098;

// GL constants
const GL_COLOR_BUFFER_BIT: u32 = 0x00004000;
const GL_TRIANGLES: u32 = 0x0004;
const GL_ARRAY_BUFFER: u32 = 0x8892;
const GL_DYNAMIC_DRAW: u32 = 0x88E8;
const GL_FLOAT: u32 = 0x1406;
const GL_FALSE: u8 = 0;
const GL_BLEND: u32 = 0x0BE2;
const GL_SRC_ALPHA: u32 = 0x0302;
const GL_ONE_MINUS_SRC_ALPHA: u32 = 0x0303;
const GL_VERTEX_SHADER: u32 = 0x8B31;
const GL_FRAGMENT_SHADER: u32 = 0x8B30;

#[link(name = "EGL")]
extern "C" {
    fn eglGetDisplay(display_id: *mut c_void) -> *mut c_void;
    fn eglInitialize(dpy: *mut c_void, major: *mut i32, minor: *mut i32) -> u32;
    fn eglChooseConfig(
        dpy: *mut c_void,
        attrib_list: *const i32,
        configs: *mut *mut c_void,
        config_size: i32,
        num_config: *mut i32,
    ) -> u32;
    fn eglCreateWindowSurface(
        dpy: *mut c_void,
        config: *mut c_void,
        win: *mut c_void,
        attrib_list: *const i32,
    ) -> *mut c_void;
    fn eglCreateContext(
        dpy: *mut c_void,
        config: *mut c_void,
        share_context: *mut c_void,
        attrib_list: *const i32,
    ) -> *mut c_void;
    fn eglMakeCurrent(
        dpy: *mut c_void,
        draw: *mut c_void,
        read: *mut c_void,
        ctx: *mut c_void,
    ) -> u32;
    fn eglSwapBuffers(dpy: *mut c_void, surface: *mut c_void) -> u32;
    fn eglDestroySurface(dpy: *mut c_void, surface: *mut c_void) -> u32;
    fn eglDestroyContext(dpy: *mut c_void, ctx: *mut c_void) -> u32;
    fn eglTerminate(dpy: *mut c_void) -> u32;
}

#[link(name = "GLESv3")]
extern "C" {
    fn glViewport(x: i32, y: i32, width: i32, height: i32);
    fn glClearColor(red: f32, green: f32, blue: f32, alpha: f32);
    fn glClear(mask: u32);
    fn glEnable(cap: u32);
    fn glBlendFunc(sfactor: u32, dfactor: u32);
    fn glCreateShader(type_: u32) -> u32;
    fn glShaderSource(shader: u32, count: i32, string: *const *const c_char, length: *const i32);
    fn glCompileShader(shader: u32);
    fn glCreateProgram() -> u32;
    fn glAttachShader(program: u32, shader: u32);
    fn glLinkProgram(program: u32);
    fn glUseProgram(program: u32);
    fn glDeleteShader(shader: u32);
    fn glGenBuffers(n: i32, buffers: *mut u32);
    fn glBindBuffer(target: u32, buffer: u32);
    fn glBufferData(target: u32, size: isize, data: *const c_void, usage: u32);
    fn glBufferSubData(target: u32, offset: isize, size: isize, data: *const c_void);
    fn glEnableVertexAttribArray(index: u32);
    fn glVertexAttribPointer(
        index: u32,
        size: i32,
        type_: u32,
        normalized: u8,
        stride: i32,
        pointer: *const c_void,
    );
    fn glDrawArrays(mode: u32, first: i32, count: i32);
    fn glGetUniformLocation(program: u32, name: *const c_char) -> i32;
    fn glUniform2f(location: i32, v0: f32, v1: f32);
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Vertex {
    pub x: f32,
    pub y: f32,
    pub r: f32,
    pub g: f32,
    pub b: f32,
    pub a: f32,
}

const MAX_VERTICES: usize = 16384;

pub struct GlesRenderer {
    display: *mut c_void,
    surface: *mut c_void,
    context: *mut c_void,
    pub width: i32,
    pub height: i32,
    program: u32,
    vbo: u32,
    u_screen_size: i32,
    vertices: Vec<Vertex>,
}

impl GlesRenderer {
    pub fn new(native_window: *mut c_void, width: i32, height: i32) -> Option<Self> {
        unsafe {
            let display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
            if display == EGL_NO_DISPLAY {
                return None;
            }

            let mut major = 0;
            let mut minor = 0;
            if eglInitialize(display, &mut major, &mut minor) == 0 {
                return None;
            }

            let attribs = [
                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                EGL_RED_SIZE, 8,
                EGL_GREEN_SIZE, 8,
                EGL_BLUE_SIZE, 8,
                EGL_ALPHA_SIZE, 8,
                EGL_DEPTH_SIZE, 16,
                EGL_NONE,
            ];

            let mut config: *mut c_void = ptr::null_mut();
            let mut num_configs = 0;
            if eglChooseConfig(display, attribs.as_ptr(), &mut config, 1, &mut num_configs) == 0 || num_configs < 1 {
                return None;
            }

            let surface = eglCreateWindowSurface(display, config, native_window, ptr::null());
            if surface == EGL_NO_SURFACE {
                return None;
            }

            let ctx_attribs = [EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE];
            let context = eglCreateContext(display, config, EGL_NO_CONTEXT, ctx_attribs.as_ptr());
            if context == EGL_NO_CONTEXT {
                return None;
            }

            if eglMakeCurrent(display, surface, surface, context) == 0 {
                return None;
            }

            // Setup shaders
            let vs_source = CString::new(r#"#version 300 es
                layout(location = 0) in vec2 a_pos;
                layout(location = 1) in vec4 a_color;
                uniform vec2 u_screen_size;
                out vec4 v_color;
                void main() {
                    vec2 norm = (a_pos / u_screen_size) * 2.0 - 1.0;
                    gl_Position = vec4(norm.x, -norm.y, 0.0, 1.0);
                    v_color = a_color;
                }
            "#).unwrap();

            let fs_source = CString::new(r#"#version 300 es
                precision mediump float;
                in vec4 v_color;
                out vec4 fragColor;
                void main() {
                    fragColor = v_color;
                }
            "#).unwrap();

            let vs = glCreateShader(GL_VERTEX_SHADER);
            glShaderSource(vs, 1, [vs_source.as_ptr()].as_ptr(), ptr::null());
            glCompileShader(vs);

            let fs = glCreateShader(GL_FRAGMENT_SHADER);
            glShaderSource(fs, 1, [fs_source.as_ptr()].as_ptr(), ptr::null());
            glCompileShader(fs);

            let program = glCreateProgram();
            glAttachShader(program, vs);
            glAttachShader(program, fs);
            glLinkProgram(program);
            glDeleteShader(vs);
            glDeleteShader(fs);

            let u_name = CString::new("u_screen_size").unwrap();
            let u_screen_size = glGetUniformLocation(program, u_name.as_ptr());

            let mut vbo = 0;
            glGenBuffers(1, &mut vbo);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(
                GL_ARRAY_BUFFER,
                (MAX_VERTICES * std::mem::size_of::<Vertex>()) as isize,
                ptr::null(),
                GL_DYNAMIC_DRAW,
            );

            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

            Some(Self {
                display,
                surface,
                context,
                width,
                height,
                program,
                vbo,
                u_screen_size,
                vertices: Vec::with_capacity(4096),
            })
        }
    }

    pub fn resize(&mut self, w: i32, h: i32) {
        self.width = w;
        self.height = h;
    }

    pub fn begin_frame(&mut self, r: f32, g: f32, b: f32) {
        unsafe {
            glViewport(0, 0, self.width, self.height);
            glClearColor(r, g, b, 1.0);
            glClear(GL_COLOR_BUFFER_BIT);
            glUseProgram(self.program);
            glUniform2f(self.u_screen_size, self.width as f32, self.height as f32);
        }
        self.vertices.clear();
    }

    pub fn draw_rect(&mut self, x: f32, y: f32, w: f32, h: f32, color: [f32; 4]) {
        let [r, g, b, a] = color;
        let v0 = Vertex { x, y, r, g, b, a };
        let v1 = Vertex { x: x + w, y, r, g, b, a };
        let v2 = Vertex { x, y: y + h, r, g, b, a };
        let v3 = Vertex { x: x + w, y: y + h, r, g, b, a };

        self.vertices.push(v0);
        self.vertices.push(v2);
        self.vertices.push(v1);

        self.vertices.push(v1);
        self.vertices.push(v2);
        self.vertices.push(v3);
    }

    pub fn draw_rect_outline(&mut self, x: f32, y: f32, w: f32, h: f32, color: [f32; 4], thick: f32) {
        self.draw_rect(x, y, w, thick, color); // top
        self.draw_rect(x, y + h - thick, w, thick, color); // bottom
        self.draw_rect(x, y, thick, h, color); // left
        self.draw_rect(x + w - thick, y, thick, h, color); // right
    }

    pub fn draw_text(&mut self, mut x: f32, y: f32, text: &str, scale: f32, color: [f32; 4]) {
        let char_w = 8.0 * scale;

        for ch in text.chars() {
            if ch == '\n' {
                continue;
            }
            if let Some(bitmap) = get_char_bitmap(ch) {
                for row in 0..8 {
                    let byte = bitmap[row];
                    for col in 0..8 {
                        if (byte & (1 << (7 - col))) != 0 {
                            let px = x + (col as f32) * scale;
                            let py = y + (row as f32) * scale;
                            self.draw_rect(px, py, scale, scale, color);
                        }
                    }
                }
            }
            x += char_w + (1.0 * scale);
        }
    }

    pub fn end_frame(&mut self) {
        if !self.vertices.is_empty() {
            unsafe {
                glBindBuffer(GL_ARRAY_BUFFER, self.vbo);
                glBufferSubData(
                    GL_ARRAY_BUFFER,
                    0,
                    (self.vertices.len() * std::mem::size_of::<Vertex>()) as isize,
                    self.vertices.as_ptr() as *const c_void,
                );

                let stride = std::mem::size_of::<Vertex>() as i32;
                glEnableVertexAttribArray(0);
                glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, stride, ptr::null());

                glEnableVertexAttribArray(1);
                glVertexAttribPointer(
                    1,
                    4,
                    GL_FLOAT,
                    GL_FALSE,
                    stride,
                    (2 * std::mem::size_of::<f32>()) as *const c_void,
                );

                glDrawArrays(GL_TRIANGLES, 0, self.vertices.len() as i32);
            }
        }

        unsafe {
            eglSwapBuffers(self.display, self.surface);
        }
    }
}

impl Drop for GlesRenderer {
    fn drop(&mut self) {
        unsafe {
            if self.display != EGL_NO_DISPLAY {
                eglMakeCurrent(self.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                if self.context != EGL_NO_CONTEXT {
                    eglDestroyContext(self.display, self.context);
                }
                if self.surface != EGL_NO_SURFACE {
                    eglDestroySurface(self.display, self.surface);
                }
                eglTerminate(self.display);
            }
        }
    }
}

// Compact 8x8 font rendering table for alphanumeric & symbols
fn get_char_bitmap(c: char) -> Option<[u8; 8]> {
    match c {
        ' ' => Some([0, 0, 0, 0, 0, 0, 0, 0]),
        '!' => Some([0x18, 0x18, 0x18, 0x18, 0x00, 0x00, 0x18, 0x00]),
        ':' => Some([0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00]),
        '-' => Some([0x00, 0x00, 0x00, 0x7E, 0x00, 0x00, 0x00, 0x00]),
        '+' => Some([0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x00, 0x00]),
        '<' => Some([0x0C, 0x18, 0x30, 0x60, 0x30, 0x18, 0x0C, 0x00]),
        '>' => Some([0x30, 0x18, 0x0C, 0x06, 0x0C, 0x18, 0x30, 0x00]),
        '[' => Some([0x3C, 0x30, 0x30, 0x30, 0x30, 0x30, 0x3C, 0x00]),
        ']' => Some([0x3C, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00]),
        '(' => Some([0x0C, 0x18, 0x30, 0x30, 0x30, 0x18, 0x0C, 0x00]),
        ')' => Some([0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x18, 0x30, 0x00]),
        '.' => Some([0x00, 0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00]),
        '/' => Some([0x06, 0x0C, 0x18, 0x30, 0x60, 0xC0, 0x80, 0x00]),
        '0' => Some([0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00]),
        '1' => Some([0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00]),
        '2' => Some([0x3C, 0x66, 0x06, 0x0C, 0x18, 0x30, 0x7E, 0x00]),
        '3' => Some([0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00]),
        '4' => Some([0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00]),
        '5' => Some([0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00]),
        '6' => Some([0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00]),
        '7' => Some([0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00]),
        '8' => Some([0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00]),
        '9' => Some([0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00]),
        'A' | 'a' => Some([0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00]),
        'B' | 'b' => Some([0x7C, 0x66, 0x66, 0x7C, 0x66, 0x66, 0x7C, 0x00]),
        'C' | 'c' => Some([0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00]),
        'D' | 'd' => Some([0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00]),
        'E' | 'e' => Some([0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00]),
        'F' | 'f' => Some([0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00]),
        'G' | 'g' => Some([0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3A, 0x00]),
        'H' | 'h' => Some([0x66, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x66, 0x00]),
        'I' | 'i' => Some([0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00]),
        'J' | 'j' => Some([0x0E, 0x06, 0x06, 0x06, 0x06, 0x66, 0x3C, 0x00]),
        'K' | 'k' => Some([0x66, 0x6C, 0x78, 0x70, 0x78, 0x6C, 0x66, 0x00]),
        'L' | 'l' => Some([0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00]),
        'M' | 'm' => Some([0x63, 0x77, 0x7F, 0x6B, 0x63, 0x63, 0x63, 0x00]),
        'N' | 'n' => Some([0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00]),
        'O' | 'o' => Some([0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00]),
        'P' | 'p' => Some([0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00]),
        'Q' | 'q' => Some([0x3C, 0x66, 0x66, 0x66, 0x6A, 0x6C, 0x36, 0x00]),
        'R' | 'r' => Some([0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00]),
        'S' | 's' => Some([0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00]),
        'T' | 't' => Some([0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00]),
        'U' | 'u' => Some([0x66, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00]),
        'V' | 'v' => Some([0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00]),
        'W' | 'w' => Some([0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00]),
        'X' | 'x' => Some([0x66, 0x66, 0x3C, 0x18, 0x3C, 0x66, 0x66, 0x00]),
        'Y' | 'y' => Some([0x66, 0x66, 0x66, 0x3C, 0x18, 0x18, 0x18, 0x00]),
        'Z' | 'z' => Some([0x7E, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x7E, 0x00]),
        _ => Some([0x7E, 0x81, 0x81, 0x81, 0x81, 0x81, 0x7E, 0x00]), // square placeholder
    }
}

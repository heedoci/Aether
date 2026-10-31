//! iOS Artemis runtime adapter for the Aether engine_runtime_provider_v1 ABI.
//! Owns the Art3m1s runtime and PFS resource mount without inventing game assets.
//! Audio/video host event consumption is NOT implemented in this first integration.
use art3m1s_core::ffi_api::{Art3m1sApiV1, art3m1s_get_api_v1};
use art3m1s_core::host_files::HostResources;
use art3m1s_core::runtime::CoreRuntime;
use std::ffi::{CStr, CString, c_char};
use std::path::{Path, PathBuf};
use std::ptr;

const API_MAGIC: u64 = 0x4152_5433_4D31_4150;

struct Bridge {
    api: &'static Art3m1sApiV1,
    runtime: *mut CoreRuntime,
    resources: HostResources,
    frame: Vec<u8>,
    error: CString,
    opened: bool,
}

impl Drop for Bridge {
    fn drop(&mut self) {
        if !self.runtime.is_null() {
            if let Some(notify) = self.api.runtime_notify_lifecycle {
                unsafe { notify(self.runtime, 0) };
            }
            if let Some(destroy) = self.api.runtime_destroy {
                unsafe { destroy(self.runtime) };
            }
            self.runtime = ptr::null_mut();
        }
    }
}

impl Bridge {
    fn fail(&mut self, message: &str) -> i32 {
        let msg = message.replace('\0', " ");
        self.error = CString::new(msg).unwrap_or_default();
        -1
    }

    fn mount_and_load(&mut self, root: &Path) -> Result<(), String> {
        let root_dir = if root.is_file() {
            root.parent().ok_or("missing archive directory")?
        } else {
            root
        };
        if !root_dir.is_dir() {
            return Err(format!("game folder missing: {}", root_dir.display()));
        }
        let archive = if root.is_file() && root.extension().is_some_and(|x| x.eq_ignore_ascii_case("pfs")) {
            Some(root.to_path_buf())
        } else {
            std::fs::read_dir(root_dir).ok().and_then(|entries| {
                let files = entries.filter_map(Result::ok).map(|e| e.path()).collect::<Vec<_>>();
                files.iter().find(|p| p.file_name().is_some_and(|n| n.eq_ignore_ascii_case("root.pfs")))
                    .cloned()
                    .or_else(|| files.iter().find(|p| p.extension().is_some_and(|e| e.eq_ignore_ascii_case("pfs"))).cloned())
            })
        };
        if let Some(pfs) = archive {
            self.resources.mount_pfs(&pfs, "Shift_JIS")?;
        } else {
            self.resources.mount_directory(root_dir)?;
        }
        let ini = self.resources.read_file("system.ini")
            .map_err(|e| format!("cannot read system.ini from Artemis game: {e}"))?;
        let set_resources = self.api.runtime_set_resources.ok_or("runtime_set_resources missing")?;
        let load = self.api.runtime_load_project_bytes.ok_or("runtime_load_project_bytes missing")?;
        if unsafe { set_resources(self.runtime, &mut self.resources) } != 1 {
            return Err("runtime_set_resources failed".into());
        }
        // Original Artemis scripts typically have a [WINDOWS] platform section.
        if unsafe { load(self.runtime, ini.as_ptr(), ini.len(), c"WINDOWS".as_ptr()) } != 0 {
            return Err("Art3m1s runtime_load_project_bytes rejected system.ini".into());
        }
        self.frame.clear();
        self.opened = true;
        Ok(())
    }
}

fn bridge_mut<'a>(opaque: *mut Bridge) -> Option<&'a mut Bridge> {
    unsafe { opaque.as_mut() }
}

#[unsafe(no_mangle)]
pub extern "C" fn aether_artemis_create(width: u32, height: u32, save_dir: *const c_char) -> *mut Bridge {
    let mut size = 0usize;
    let raw = unsafe { art3m1s_get_api_v1(&mut size) };
    let Some(api) = (unsafe { raw.as_ref() }) else { return ptr::null_mut() };
    if api.magic != API_MAGIC || api.abi_version != 1 || api.struct_size as usize != size {
        return ptr::null_mut();
    }
    let Some(create) = api.runtime_create else { return ptr::null_mut() };
    // backend=0: platform default, native Metal on iOS.
    let runtime = unsafe { create(width.max(1), height.max(1), 0) };
    if runtime.is_null() { return ptr::null_mut() }
    let resources = HostResources::new();
    if !save_dir.is_null() {
        let save_path = unsafe { CStr::from_ptr(save_dir) }.to_string_lossy();
        if !save_path.is_empty() {
            if resources.set_save_dir(Some(Path::new(save_path.as_ref()))).is_err() {
                if let Some(destroy) = api.runtime_destroy {
                    unsafe { destroy(runtime) };
                }
                return ptr::null_mut();
            }
        }
    }
    Box::into_raw(Box::new(Bridge {
        api, runtime, resources, frame: Vec::new(),
        error: CString::default(), opened: false
    }))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_destroy(opaque: *mut Bridge) {
    if !opaque.is_null() {
        drop(unsafe { Box::from_raw(opaque) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_open(opaque: *mut Bridge, path: *const c_char) -> i32 {
    let Some(b) = bridge_mut(opaque) else { return -1 };
    if path.is_null() { return b.fail("missing Artemis game path"); }
    let path = unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned();
    match b.mount_and_load(Path::new(&path)) {
        Ok(()) => 0,
        Err(message) => b.fail(&message)
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_tick(opaque: *mut Bridge, delta_ms: u32) -> i32 {
    let Some(b) = bridge_mut(opaque) else { return -1 };
    if !b.opened { return b.fail("Artemis game is not opened"); }
    if b.api.runtime_is_exit_requested.is_some_and(|f| unsafe { f(b.runtime) != 0 }) {
        b.error = CString::new("runtime requested termination").unwrap_or_default();
        return -2;
    }
    let Some(size_fn) = b.api.runtime_pixel_buffer_size else { return b.fail("pixel buffer API unavailable") };
    let size = unsafe { size_fn(b.runtime) } as usize;
    if size == 0 || size > 100 * 1024 * 1024 {
        return b.fail("invalid Artemis frame size");
    }
    b.frame.resize(size, 0);
    let Some(advance) = b.api.runtime_advance_and_render else { return b.fail("render API unavailable") };
    let produced = unsafe { advance(b.runtime, delta_ms, b.frame.as_mut_ptr(), size as u32) } as usize;
    if produced == 0 || produced > size {
        return b.fail("Art3m1s did not produce a CPU-readable RGBA frame");
    }
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_width(opaque: *mut Bridge) -> u32 {
    let Some(b) = bridge_mut(opaque) else { return 0 };
    b.api.runtime_stage_width.map_or(0, |f| unsafe { f(b.runtime) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_height(opaque: *mut Bridge) -> u32 {
    let Some(b) = bridge_mut(opaque) else { return 0 };
    b.api.runtime_stage_height.map_or(0, |f| unsafe { f(b.runtime) })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_frame_bytes(opaque: *mut Bridge, out: *mut u8, cap: usize) -> usize {
    let Some(b) = bridge_mut(opaque) else { return 0 };
    if out.is_null() || b.frame.is_empty() || cap < b.frame.len() { return 0 };
    unsafe { ptr::copy_nonoverlapping(b.frame.as_ptr(), out, b.frame.len()) };
    b.frame.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_has_frame(opaque: *mut Bridge) -> i32 {
    bridge_mut(opaque).map_or(0, |b| i32::from(!b.frame.is_empty()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_mouse(opaque: *mut Bridge, x: i32, y: i32) {
    if let Some(b) = bridge_mut(opaque) {
        if let Some(f) = b.api.runtime_feed_mouse { unsafe { f(b.runtime, x, y) } }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_button(opaque: *mut Bridge, button: u32, pressed: i32) {
    if let Some(b) = bridge_mut(opaque) {
        if let Some(f) = b.api.runtime_feed_mouse_button {
            unsafe { f(b.runtime, button, pressed) }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_key(opaque: *mut Bridge, vk: u32, pressed: i32) {
    if let Some(b) = bridge_mut(opaque) {
        if let Some(f) = b.api.runtime_feed_key { unsafe { f(b.runtime, vk, pressed) } }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_lifecycle(opaque: *mut Bridge, state: i32) {
    if let Some(b) = bridge_mut(opaque) {
        if let Some(f) = b.api.runtime_notify_lifecycle {
            unsafe { f(b.runtime, state) }
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn aether_artemis_error(opaque: *mut Bridge) -> *const c_char {
    bridge_mut(opaque).map_or(c"missing Artemis bridge".as_ptr(), |b| b.error.as_ptr())
}

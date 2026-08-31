//! WebKitGTK workarounds that must run before the webview is created.

use std::env;
use std::path::{Path, PathBuf};

const DMABUF: &str = "WEBKIT_DISABLE_DMABUF_RENDERER";
const COMPOSITING: &str = "WEBKIT_DISABLE_COMPOSITING_MODE";
const NV_SYNC: &str = "__NV_DISABLE_EXPLICIT_SYNC";
const GDK_BACKEND: &str = "GDK_BACKEND";
const GIO_MODULE_DIR: &str = "GIO_MODULE_DIR";
const GTK_IM_MODULE: &str = "GTK_IM_MODULE";
const XMODIFIERS: &str = "XMODIFIERS";
const NO_AT_BRIDGE: &str = "NO_AT_BRIDGE";
const GTK_MODULES: &str = "GTK_MODULES";
/// Opt out of X11/XWayland: `GETNATIVE_WEBKIT_WAYLAND=1`.
const NATIVE_WAYLAND: &str = "GETNATIVE_WEBKIT_WAYLAND";
/// Keep the session IME (fcitx5/ibus): `GETNATIVE_WEBKIT_IME=1`.
const KEEP_IME: &str = "GETNATIVE_WEBKIT_IME";

/// GNOME/Wayland WebKitGTK can stop presenting after caret or selection
/// redraws: the UI looks frozen until the window is resized. DMA-BUF off
/// alone is not enough on some hosts. Fall back to software compositing
/// and, when XWayland is available, force GDK onto X11.
///
/// AppImages built on Ubuntu 22.04 also load the *host* GIO and GTK IM
/// modules (gvfs, IBus). Ubuntu 26.04's modules need a newer GLib than
/// the bundle (`g_task_set_static_name`), and focusing an input attaches
/// IBus through that mismatched .so — that freeze is independent of GPU
/// flags. Isolate GIO and default the IM module to GTK's built-in
/// `simple` backend inside AppImages.
pub fn apply_workarounds() {
    if cfg!(not(target_os = "linux")) {
        return;
    }
    // Thin AppImages, .deb, and `tauri dev` use the host WebKitGTK. Do not
    // force X11 or disable IME/AT-SPI in that case — that path is what works
    // on Ubuntu 26.04 GNOME. Fat AppImages that still bundle WebKit keep the
    // isolation workarounds below.
    if !bundled_webkit() {
        return;
    }
    isolate_host_gio_modules();
    isolate_gtk_search_path();
    isolate_host_im_module();
    disable_atk_bridge();
    set_if_unset(DMABUF, "1");
    set_if_unset(COMPOSITING, "1");
    if nvidia_present() {
        set_if_unset(NV_SYNC, "1");
    }
    force_x11_backend();
}

fn set_if_unset(key: &str, value: &str) {
    if env::var_os(key).is_none() {
        env::set_var(key, value);
        eprintln!("getnative: {key}={value} (WebKitGTK freeze workaround)");
    }
}

fn bundled_webkit() -> bool {
    let Some(appdir) = env::var_os("APPDIR") else {
        return false;
    };
    let appdir = PathBuf::from(appdir);
    [
        appdir.join("usr/lib/libwebkit2gtk-4.1.so.0"),
        appdir.join("usr/lib/x86_64-linux-gnu/libwebkit2gtk-4.1.so.0"),
        appdir.join("usr/lib/aarch64-linux-gnu/libwebkit2gtk-4.1.so.0"),
        appdir.join("usr/lib64/libwebkit2gtk-4.1.so.0"),
    ]
    .iter()
    .any(|path| path.exists())
}

fn isolate_host_gio_modules() {
    if env::var_os(GIO_MODULE_DIR).is_some() {
        return;
    }
    let Some(appdir) = env::var_os("APPDIR") else {
        return;
    };
    let appdir = PathBuf::from(appdir);
    let candidates = [
        appdir.join("usr/lib/x86_64-linux-gnu/gio/modules"),
        appdir.join("usr/lib/aarch64-linux-gnu/gio/modules"),
        appdir.join("usr/lib/gio/modules"),
    ];
    let dir = candidates
        .into_iter()
        .find(|path| path.is_dir())
        .unwrap_or_else(|| PathBuf::from("/dev/null"));
    env::set_var(GIO_MODULE_DIR, &dir);
    eprintln!(
        "getnative: {GIO_MODULE_DIR}={} (block host gvfs against bundled GLib)",
        dir.display()
    );
}

fn isolate_gtk_search_path() {
    let Ok(appdir) = env::var("APPDIR") else {
        return;
    };
    let Ok(path) = env::var("GTK_PATH") else {
        return;
    };
    let filtered: Vec<&str> = path
        .split(':')
        .filter(|entry| !entry.is_empty() && entry.starts_with(&appdir))
        .collect();
    env::set_var("GTK_PATH", filtered.join(":"));
    eprintln!("getnative: GTK_PATH={} (drop host GTK modules from AppImage)", filtered.join(":"));
}

fn isolate_host_im_module() {
    if env::var_os(KEEP_IME).is_some() {
        return;
    }
    // Session-wide GTK_IM_MODULE=fcitx / XMODIFIERS=@im=fcitx still attach
    // fcitx5 on focus even when the GTK module is `simple`. The AppImage
    // hook already forces GDK_BACKEND=x11, so Wayland text-input is not the
    // only path — XIM is. Disable both unless the user opts back in.
    env::set_var(GTK_IM_MODULE, "simple");
    env::set_var(XMODIFIERS, "@im=none");
    eprintln!("getnative: {GTK_IM_MODULE}=simple {XMODIFIERS}=@im=none (block fcitx5/ibus on focus)");
}

fn disable_atk_bridge() {
    // Ubuntu 26.04 at-spi2 replies with a D-Bus signature that the
    // AppImage's Ubuntu 22.04 atk-bridge does not understand. Focusing a
    // text field queries device events over that bridge and the UI freezes
    // until a resize. NO_AT_BRIDGE is the documented opt-out.
    set_if_unset(NO_AT_BRIDGE, "1");
    if let Ok(modules) = env::var(GTK_MODULES) {
        let filtered: Vec<&str> = modules
            .split(':')
            .filter(|module| {
                let name = module.trim();
                !name.is_empty() && name != "atk-bridge" && name != "gail"
            })
            .collect();
        env::set_var(GTK_MODULES, filtered.join(":"));
    }
}

fn force_x11_backend() {
    if env::var_os(NATIVE_WAYLAND).is_some() {
        return;
    }
    if env::var_os("DISPLAY").is_none() {
        return;
    }
    env::remove_var("WAYLAND_DISPLAY");
    env::remove_var("WAYLAND_SOCKET");
    env::set_var(GDK_BACKEND, "x11");
    eprintln!("getnative: {GDK_BACKEND}=x11, WAYLAND_DISPLAY unset (force X11, not XWayland IM)");
}

fn nvidia_present() -> bool {
    Path::new("/proc/driver/nvidia/version").is_file() || Path::new("/dev/nvidia0").exists()
}

#[cfg(test)]
mod tests {
    use super::{
        COMPOSITING, DMABUF, GDK_BACKEND, GIO_MODULE_DIR, GTK_IM_MODULE, KEEP_IME, NATIVE_WAYLAND,
        NO_AT_BRIDGE, NV_SYNC, XMODIFIERS,
    };

    #[test]
    fn workaround_names_match_webkit_and_nvidia_docs() {
        assert_eq!(DMABUF, "WEBKIT_DISABLE_DMABUF_RENDERER");
        assert_eq!(COMPOSITING, "WEBKIT_DISABLE_COMPOSITING_MODE");
        assert_eq!(NV_SYNC, "__NV_DISABLE_EXPLICIT_SYNC");
        assert_eq!(GDK_BACKEND, "GDK_BACKEND");
        assert_eq!(GIO_MODULE_DIR, "GIO_MODULE_DIR");
        assert_eq!(GTK_IM_MODULE, "GTK_IM_MODULE");
        assert_eq!(XMODIFIERS, "XMODIFIERS");
        assert_eq!(NO_AT_BRIDGE, "NO_AT_BRIDGE");
        assert_eq!(NATIVE_WAYLAND, "GETNATIVE_WEBKIT_WAYLAND");
        assert_eq!(KEEP_IME, "GETNATIVE_WEBKIT_IME");
    }
}

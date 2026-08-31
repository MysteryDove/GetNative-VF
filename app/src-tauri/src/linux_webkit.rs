//! WebKitGTK workarounds that must run before the webview is created.

use std::env;
use std::path::Path;

const DMABUF: &str = "WEBKIT_DISABLE_DMABUF_RENDERER";
const NV_SYNC: &str = "__NV_DISABLE_EXPLICIT_SYNC";

/// GNOME/Wayland WebKitGTK can stop presenting after caret or selection
/// redraws: the UI looks frozen until the window is resized. Native number
/// spinners were one trigger; typing into a text field is another. Disable
/// the DMA-BUF renderer unless the user already set the variable. NVIDIA
/// hosts also get the cheaper explicit-sync workaround from Tauri's docs.
pub fn apply_workarounds() {
    if cfg!(not(target_os = "linux")) {
        return;
    }
    if env::var_os(DMABUF).is_none() {
        env::set_var(DMABUF, "1");
        eprintln!(
            "getnative: {DMABUF}=1 (WebKitGTK input-redraw freeze workaround)"
        );
    }
    if nvidia_present() && env::var_os(NV_SYNC).is_none() {
        env::set_var(NV_SYNC, "1");
    }
}

fn nvidia_present() -> bool {
    Path::new("/proc/driver/nvidia/version").is_file() || Path::new("/dev/nvidia0").exists()
}

#[cfg(test)]
mod tests {
    use super::{DMABUF, NV_SYNC};

    #[test]
    fn workaround_names_match_webkit_and_nvidia_docs() {
        assert_eq!(DMABUF, "WEBKIT_DISABLE_DMABUF_RENDERER");
        assert_eq!(NV_SYNC, "__NV_DISABLE_EXPLICIT_SYNC");
    }
}

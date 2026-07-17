#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

fn main() {
    // WebKitGTK's DMA-BUF renderer can produce a completely blank window on
    // some Linux ARM64 graphics stacks. Disable only that renderer; WebKit
    // continues to use its fallback rendering path.
    #[cfg(all(target_os = "linux", target_arch = "aarch64"))]
    std::env::set_var("WEBKIT_DISABLE_DMABUF_RENDERER", "1");

    vpx_scoretracker_viewer_lib::run();
}

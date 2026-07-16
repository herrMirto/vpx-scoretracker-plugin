use std::env;
use std::fs::File;
use std::path::PathBuf;

use flate2::write::GzEncoder;
use flate2::Compression;

fn main() {
    println!("cargo:rerun-if-env-changed=SCORETRACKER_PAYLOAD_DIR");

    let output = PathBuf::from(env::var_os("OUT_DIR").expect("OUT_DIR is set"))
        .join("scoretracker-payload.tar.gz");
    let archive = File::create(&output).expect("create embedded payload archive");
    let encoder = GzEncoder::new(archive, Compression::best());
    let mut tar = tar::Builder::new(encoder);
    tar.follow_symlinks(false);

    if let Some(payload) = env::var_os("SCORETRACKER_PAYLOAD_DIR") {
        let payload = PathBuf::from(payload);
        if !payload.is_dir() {
            panic!(
                "SCORETRACKER_PAYLOAD_DIR is not a directory: {}",
                payload.display()
            );
        }
        println!("cargo:rerun-if-changed={}", payload.display());
        tar.append_dir_all(".", &payload)
            .expect("archive installer payload");
    } else if env::var("PROFILE").as_deref() == Ok("release") {
        panic!("SCORETRACKER_PAYLOAD_DIR must be set for release installer builds");
    }

    let encoder = tar.into_inner().expect("finish payload tar archive");
    encoder.finish().expect("finish payload compression");
}

use atomicwrites::{AllowOverwrite, AtomicFile};
use std::fs;
use std::io::{self, Write};
use std::path::Path;

pub fn write_bytes(path: &Path, bytes: &[u8]) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }

    AtomicFile::new(path, AllowOverwrite)
        .write(|file| file.write_all(bytes))
        .map_err(io::Error::from)
}

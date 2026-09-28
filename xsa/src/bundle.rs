//! Cargo-installed stages: immutable embedded bytes, verified on every use.
use sha2::{Digest, Sha256};
use std::{fs, path::{Path, PathBuf}, os::unix::fs::PermissionsExt};
include!(concat!(env!("OUT_DIR"), "/bundled_tools.rs"));

pub fn hash(bytes: &[u8]) -> String { format!("{:x}", Sha256::digest(bytes)) }

fn verify(root: &Path) -> Result<(), String> {
    for (name, _, expected) in FILES {
        let path = root.join(name);
        let md = fs::symlink_metadata(&path).map_err(|e| format!("bundled tool {}: {e}", path.display()))?;
        if !md.is_file() || md.file_type().is_symlink() {
            return Err(format!("bundled tool must be a regular file: {}", path.display()));
        }
        let bytes = fs::read(&path).map_err(|e| e.to_string())?;
        if hash(&bytes) != *expected {
            return Err(format!("tool manifest verification failed: SHA256 mismatch: {} (expected {expected})", path.display()));
        }
        if !name.contains('/') && *name != "MANIFEST.sha256" && md.permissions().mode() & 0o111 == 0 {
            return Err(format!("missing executable: {}", path.display()));
        }
    }
    Ok(())
}

pub fn resolve() -> Result<PathBuf, String> {
    let base = std::env::var_os("XDG_CACHE_HOME").map(PathBuf::from)
        .or_else(|| std::env::var_os("HOME").map(|p| PathBuf::from(p).join(".cache")))
        .ok_or("xsa needs HOME or XDG_CACHE_HOME for its verified tool cache")?.join("xsa");
    fs::create_dir_all(&base).map_err(|e| format!("create {}: {e}", base.display()))?;
    let root = base.join(ID);
    if !root.exists() {
        let temp = tempfile::Builder::new().prefix(".extract-").tempdir_in(&base).map_err(|e| e.to_string())?;
        fs::set_permissions(temp.path(), fs::Permissions::from_mode(0o700)).map_err(|e| e.to_string())?;
        for (name, bytes, _) in FILES {
            let path = temp.path().join(name);
            fs::create_dir_all(path.parent().unwrap()).map_err(|e| e.to_string())?;
            fs::write(&path, bytes).map_err(|e| e.to_string())?;
            let mode = if !name.contains('/') && *name != "MANIFEST.sha256" { 0o700 } else { 0o600 };
            fs::set_permissions(path, fs::Permissions::from_mode(mode)).map_err(|e| e.to_string())?;
        }
        verify(temp.path())?;
        if let Err(e) = fs::rename(temp.path(), &root) {
            if !root.is_dir() { return Err(format!("publish tool cache: {e}")); }
            // Another invocation may have atomically installed the same bundle.
        }
    }
    verify(&root)?;
    Ok(root)
}

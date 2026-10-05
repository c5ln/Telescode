//! Opening a public GitHub repository from its URL.
//!
//! The repository's default branch is downloaded as a tarball, unpacked into
//! the app's cache, and scanned into a database there with the same
//! `TelescodeHeadless scan` and `algo` a local repository goes through. The
//! frontend then opens that database like any other, so both kinds of
//! repository share one analysis path.
//!
//! Only public repositories are supported: no token is ever sent, and GitHub
//! answers a private repository exactly as it does a missing one.

use serde::Deserialize;
use std::collections::HashMap;
use std::ffi::OsStr;
use std::fs;
use std::io::{self, Read};
use std::path::{Component, Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::Duration;

use tauri::Manager;

use crate::bridge::{self, BridgeError};

const API: &str = "https://api.github.com";
const USER_AGENT: &str = concat!("Telescode/", env!("CARGO_PKG_VERSION"));

/// Largest repository GitHub may report, in KB. This counts history too, so it
/// is generous: the limits on the download itself are what really apply.
const MAX_REPOSITORY_KB: u64 = 2_000_000;
/// Largest tarball accepted, compressed.
const MAX_DOWNLOAD_BYTES: u64 = 500 * 1024 * 1024;
/// Largest total size of the unpacked files.
const MAX_SOURCE_BYTES: u64 = 2 * 1024 * 1024 * 1024;
/// Most entries an archive may hold.
const MAX_ENTRIES: u64 = 200_000;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
const DOWNLOAD_TIMEOUT: Duration = Duration::from_secs(15 * 60);

/// A repository named by a URL: `owner/name` on github.com.
#[derive(Debug, PartialEq)]
pub struct RepositoryRef {
    pub owner: String,
    pub name: String,
}

/// Reads `https://github.com/owner/repo`, with or without `www.`, `.git`, a
/// trailing slash, a query or a fragment. Anything that points inside a
/// repository (a branch, file, pull request, ...) is refused, as are other hosts.
pub fn parse_url(raw: &str) -> Result<RepositoryRef, BridgeError> {
    let invalid = |why: &str| BridgeError::InvalidRepositoryUrl {
        message: format!("{why} Enter a public GitHub repository URL, such as https://github.com/owner/repo."),
    };

    let trimmed = raw.trim();
    let rest = trimmed
        .strip_prefix("https://")
        .or_else(|| trimmed.strip_prefix("http://"))
        .ok_or_else(|| invalid("Not a web URL."))?;
    let rest = rest.split(['?', '#']).next().unwrap_or_default();
    let (host, path) = rest.split_once('/').unwrap_or((rest, ""));
    let host = host.to_ascii_lowercase();
    if host != "github.com" && host != "www.github.com" {
        return Err(invalid("Only GitHub repositories are supported."));
    }

    let parts: Vec<&str> = path.split('/').filter(|p| !p.is_empty()).collect();
    match parts.as_slice() {
        [owner, name] => {
            let name = name.strip_suffix(".git").unwrap_or(name);
            let owner_ok = !owner.is_empty() && owner.chars().all(|c| c.is_ascii_alphanumeric() || c == '-');
            let name_ok = !name.is_empty()
                && name != "."
                && name != ".."
                && name.chars().all(|c| c.is_ascii_alphanumeric() || matches!(c, '-' | '_' | '.'));
            if owner_ok && name_ok {
                Ok(RepositoryRef { owner: owner.to_string(), name: name.to_string() })
            } else {
                Err(invalid("That is not a valid repository name."))
            }
        }
        [_, _, ..] => Err(invalid("That URL points inside a repository.")),
        _ => Err(invalid("The URL needs both an owner and a repository name.")),
    }
}

/// What GitHub says about a repository; only the fields used here.
#[derive(Debug, Deserialize)]
struct RepositoryInfo {
    name: String,
    full_name: String,
    #[serde(default)]
    private: bool,
    #[serde(default)]
    size: u64,
}

fn agent(timeout: Duration) -> ureq::Agent {
    ureq::Agent::new_with_config(
        ureq::Agent::config_builder()
            .user_agent(USER_AGENT)
            .http_status_as_error(false)
            .timeout_connect(Some(CONNECT_TIMEOUT))
            .timeout_global(Some(timeout))
            .build(),
    )
}

fn unreachable(e: impl std::fmt::Display) -> BridgeError {
    BridgeError::RepositoryUnreachable { message: format!("Could not reach GitHub: {e}") }
}

/// The error for a response GitHub refused, by status.
fn status_error(status: u16, repo: &RepositoryRef) -> BridgeError {
    match status {
        404 | 451 => BridgeError::RepositoryNotFound {
            message: format!(
                "{}/{} does not exist or is private. Only public repositories can be opened.",
                repo.owner, repo.name
            ),
        },
        403 | 429 => BridgeError::RepositoryUnreachable {
            message: "GitHub is limiting requests right now. Try again in a few minutes.".into(),
        },
        _ => BridgeError::RepositoryUnreachable { message: format!("GitHub answered with HTTP {status}.") },
    }
}

fn fetch_info(repo: &RepositoryRef) -> Result<RepositoryInfo, BridgeError> {
    let url = format!("{API}/repos/{}/{}", repo.owner, repo.name);
    let mut res = agent(CONNECT_TIMEOUT * 2)
        .get(&url)
        .header("Accept", "application/vnd.github+json")
        .call()
        .map_err(unreachable)?;
    let status = res.status().as_u16();
    if status != 200 {
        return Err(status_error(status, repo));
    }
    let body = res.body_mut().read_to_string().map_err(unreachable)?;
    let info: RepositoryInfo = serde_json::from_str(&body).map_err(|e| BridgeError::RepositoryFetchFailed {
        message: format!("GitHub sent a repository description Telescode could not read: {e}"),
    })?;
    if info.private {
        return Err(status_error(404, repo));
    }
    if info.size > MAX_REPOSITORY_KB {
        return Err(too_large(&info.full_name));
    }
    Ok(info)
}

fn too_large(name: &str) -> BridgeError {
    BridgeError::RepositoryTooLarge { message: format!("{name} is too large for Telescode to process.") }
}

/// Fails reads once more than `limit` bytes have passed through.
struct Capped<R> {
    inner: R,
    remaining: u64,
}

impl<R: Read> Read for Capped<R> {
    fn read(&mut self, buf: &mut [u8]) -> io::Result<usize> {
        let n = self.inner.read(buf)?;
        self.remaining = self
            .remaining
            .checked_sub(n as u64)
            .ok_or_else(|| io::Error::new(io::ErrorKind::FileTooLarge, "download limit exceeded"))?;
        Ok(n)
    }
}

/// Where an archive entry goes under the destination: the path without
/// GitHub's top-level `owner-repo-sha/` directory. `None` for that directory
/// itself and for anything that could escape the destination.
fn entry_destination(path: &Path) -> Option<PathBuf> {
    let mut components = path.components();
    let Some(Component::Normal(_)) = components.next() else { return None };
    let mut out = PathBuf::new();
    for c in components {
        match c {
            Component::Normal(part) => out.push(part),
            Component::CurDir => {}
            _ => return None,
        }
    }
    (!out.as_os_str().is_empty()).then_some(out)
}

fn fetch_failed(e: impl std::fmt::Display) -> BridgeError {
    BridgeError::RepositoryFetchFailed { message: format!("Could not download the repository: {e}") }
}

/// Unpacks a gzipped GitHub tarball into `dest`, keeping only directories and
/// regular files. Links are skipped, so nothing unpacked can point outside.
fn unpack(reader: impl Read, dest: &Path, name: &str) -> Result<(), BridgeError> {
    let capped = Capped { inner: reader, remaining: MAX_DOWNLOAD_BYTES };
    let mut archive = tar::Archive::new(flate2::read::GzDecoder::new(capped));
    let mut entries_seen = 0u64;
    let mut unpacked = 0u64;

    // A read that hit the download cap surfaces as an io error somewhere in
    // the archive or gzip layers; recognise it wherever it comes from.
    let read_error = |e: io::Error| {
        if e.kind() == io::ErrorKind::FileTooLarge {
            too_large(name)
        } else {
            fetch_failed(e)
        }
    };

    for entry in archive.entries().map_err(read_error)? {
        let mut entry = entry.map_err(read_error)?;
        entries_seen += 1;
        if entries_seen > MAX_ENTRIES {
            return Err(too_large(name));
        }
        let path = entry.path().map_err(read_error)?.into_owned();
        let Some(relative) = entry_destination(&path) else { continue };
        let target = dest.join(relative);

        match entry.header().entry_type() {
            tar::EntryType::Directory => fs::create_dir_all(&target).map_err(fetch_failed)?,
            tar::EntryType::Regular | tar::EntryType::Continuous => {
                unpacked += entry.size();
                if unpacked > MAX_SOURCE_BYTES {
                    return Err(too_large(name));
                }
                if let Some(parent) = target.parent() {
                    fs::create_dir_all(parent).map_err(fetch_failed)?;
                }
                let mut file = fs::File::create(&target).map_err(fetch_failed)?;
                io::copy(&mut entry, &mut file).map_err(read_error)?;
            }
            _ => {}
        }
    }
    Ok(())
}

/// Downloads the default branch of `repo` into `dest`, which must not exist.
fn download(repo: &RepositoryRef, info: &RepositoryInfo, dest: &Path) -> Result<(), BridgeError> {
    // Without a ref, GitHub serves the default branch.
    let url = format!("{API}/repos/{}/{}/tarball", repo.owner, repo.name);
    let mut res = agent(DOWNLOAD_TIMEOUT).get(&url).call().map_err(unreachable)?;
    let status = res.status().as_u16();
    if status != 200 {
        return Err(status_error(status, repo));
    }
    fs::create_dir_all(dest).map_err(fetch_failed)?;
    unpack(res.body_mut().as_reader(), dest, &info.full_name)
}

/// Number of files `scan` reports parsing, from its `[scan] Parsed N files.` line.
fn parsed_file_count(stdout: &str) -> Option<u64> {
    stdout.lines().find_map(|line| {
        line.trim().strip_prefix("[scan] Parsed ")?.strip_suffix(" files.")?.parse().ok()
    })
}

fn remove_if_present(path: &Path) -> Result<(), BridgeError> {
    let result = if path.is_dir() { fs::remove_dir_all(path) } else { fs::remove_file(path) };
    match result {
        Err(e) if e.kind() != io::ErrorKind::NotFound => Err(BridgeError::Internal {
            message: format!("Could not clear {}: {e}", path.display()),
        }),
        _ => Ok(()),
    }
}

/// Scans `source` into a fresh database at `db` and computes its reading order.
fn scan(sidecar: &Path, source: &Path, db: &Path, name: &str) -> Result<(), BridgeError> {
    for suffix in ["", "-wal", "-shm"] {
        let mut path = db.as_os_str().to_owned();
        path.push(suffix);
        remove_if_present(Path::new(&path))?;
    }

    let out = bridge::run_sidecar(sidecar, &[OsStr::new("scan"), source.as_os_str(), db.as_os_str()])?;
    if parsed_file_count(&out) == Some(0) {
        return Err(BridgeError::RepositoryEmpty {
            message: format!("{name} has no source files Telescode can analyze."),
        });
    }
    bridge::run_sidecar(sidecar, &[OsStr::new("algo"), db.as_os_str()])?;
    Ok(())
}

/// One lock per cache directory, so two opens of the same repository never
/// unpack or scan over each other. Different repositories proceed in parallel.
fn lock_for(dir: &Path) -> Arc<Mutex<()>> {
    static LOCKS: OnceLock<Mutex<HashMap<PathBuf, Arc<Mutex<()>>>>> = OnceLock::new();
    let mut locks = LOCKS.get_or_init(Default::default).lock().unwrap_or_else(|e| e.into_inner());
    locks.entry(dir.to_path_buf()).or_default().clone()
}

/// Downloads and scans the repository at `url` under `cache`, returning the
/// database to open. Each open fetches the current default branch again.
fn open(cache: &Path, url: &str) -> Result<PathBuf, BridgeError> {
    let repo = parse_url(url)?;
    let sidecar = bridge::locate_sidecar()?;
    let info = fetch_info(&repo)?;

    // GitHub names are case-insensitive, so one directory per repository
    // whatever the URL's spelling.
    let dir = cache
        .join("repositories")
        .join(repo.owner.to_ascii_lowercase())
        .join(repo.name.to_ascii_lowercase());
    let lock = lock_for(&dir);
    let _guard = lock.lock().unwrap_or_else(|e| e.into_inner());

    let source = dir.join("source");
    let partial = dir.join("source.partial");
    remove_if_present(&partial)?;
    let downloaded = download(&repo, &info, &partial);
    if let Err(e) = downloaded {
        let _ = fs::remove_dir_all(&partial);
        return Err(e);
    }
    remove_if_present(&source)?;
    fs::rename(&partial, &source).map_err(|e| BridgeError::Internal {
        message: format!("Could not move the downloaded repository into place: {e}"),
    })?;

    // Named after the repository, which is how the workspace titles it.
    let db = dir.join(format!("{}.db", info.name));
    scan(&sidecar, &source, &db, &info.full_name)?;
    Ok(db)
}

/// IPC entry point: `invoke("open_repository", { url })`. Resolves with the
/// path of the scanned database, ready for `run_headless`.
#[tauri::command]
pub async fn open_repository(app: tauri::AppHandle, url: String) -> Result<String, BridgeError> {
    let cache = app.path().app_cache_dir().map_err(|e| BridgeError::Internal {
        message: format!("Cannot locate the app's cache directory: {e}"),
    })?;
    tauri::async_runtime::spawn_blocking(move || open(&cache, &url).map(|db| db.display().to_string()))
        .await
        .map_err(|e| BridgeError::Internal {
            message: format!("Repository worker failed: {e}"),
        })?
}

#[cfg(test)]
mod tests {
    use super::*;

    fn repo(owner: &str, name: &str) -> RepositoryRef {
        RepositoryRef { owner: owner.into(), name: name.into() }
    }

    #[test]
    fn repository_urls_are_read() {
        for url in [
            "https://github.com/c5ln/Telescode",
            "https://github.com/c5ln/Telescode/",
            "https://github.com/c5ln/Telescode.git",
            "http://www.github.com/c5ln/Telescode",
            "  https://GitHub.com/c5ln/Telescode?tab=readme#top ",
        ] {
            assert_eq!(parse_url(url).unwrap(), repo("c5ln", "Telescode"), "{url}");
        }
        assert_eq!(parse_url("https://github.com/a-b/x.y_z").unwrap(), repo("a-b", "x.y_z"));
    }

    #[test]
    fn other_urls_are_refused() {
        for url in [
            "",
            "github.com/c5ln/Telescode",
            "git@github.com:c5ln/Telescode.git",
            "https://gitlab.com/c5ln/Telescode",
            "https://github.com.evil.example/c5ln/Telescode",
            "https://github.com/c5ln",
            "https://github.com/c5ln/Telescode/blob/main/README.md",
            "https://github.com/c5ln/Telescode/pull/1",
            "https://github.com/c5ln/..",
            "https://github.com/c5ln/re po",
        ] {
            assert!(matches!(parse_url(url), Err(BridgeError::InvalidRepositoryUrl { .. })), "{url}");
        }
    }

    #[test]
    fn missing_and_private_repositories_read_the_same() {
        let e = status_error(404, &repo("a", "b"));
        assert!(matches!(e, BridgeError::RepositoryNotFound { .. }));
        assert!(matches!(status_error(403, &repo("a", "b")), BridgeError::RepositoryUnreachable { .. }));
    }

    #[test]
    fn archive_paths_lose_their_top_directory_and_cannot_escape() {
        assert_eq!(entry_destination(Path::new("o-r-abc/src/a.py")), Some(PathBuf::from("src/a.py")));
        assert_eq!(entry_destination(Path::new("o-r-abc/")), None);
        assert_eq!(entry_destination(Path::new("o-r-abc/../x")), None);
        assert_eq!(entry_destination(Path::new("/etc/passwd")), None);
    }

    fn tarball(entries: &[(&str, &[u8])]) -> Vec<u8> {
        let gz = flate2::write::GzEncoder::new(Vec::new(), flate2::Compression::fast());
        let mut builder = tar::Builder::new(gz);
        for (path, data) in entries {
            let mut header = tar::Header::new_gnu();
            header.set_size(data.len() as u64);
            header.set_mode(0o644);
            header.set_cksum();
            builder.append_data(&mut header, path, *data).unwrap();
        }
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(tar::EntryType::Symlink);
        header.set_size(0);
        builder.append_link(&mut header, "o-r-abc/link", "/etc/passwd").unwrap();
        builder.into_inner().unwrap().finish().unwrap()
    }

    #[test]
    fn tarballs_unpack_files_but_not_links() {
        let dest = std::env::temp_dir().join(format!("telescode-unpack-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dest);
        fs::create_dir_all(&dest).unwrap();
        let bytes = tarball(&[("o-r-abc/pkg/a.py", b"print(1)\n"), ("o-r-abc/README.md", b"hi")]);
        unpack(&bytes[..], &dest, "o/r").unwrap();
        assert_eq!(fs::read_to_string(dest.join("pkg/a.py")).unwrap(), "print(1)\n");
        assert!(dest.join("README.md").is_file());
        assert!(fs::symlink_metadata(dest.join("link")).is_err());
        let _ = fs::remove_dir_all(&dest);
    }

    #[test]
    fn oversized_downloads_are_refused() {
        let mut capped = Capped { inner: &[0u8; 10][..], remaining: 4 };
        let err = io::copy(&mut capped, &mut io::sink()).unwrap_err();
        assert_eq!(err.kind(), io::ErrorKind::FileTooLarge);
    }

    #[test]
    fn scan_output_gives_the_file_count() {
        assert_eq!(parsed_file_count("[scan] Parsed 0 files.\n[scan] Inserted"), Some(0));
        assert_eq!(parsed_file_count("[scan] Parsed 42 files.\n"), Some(42));
        assert_eq!(parsed_file_count("something else"), None);
    }

    // Downloads and scans a real repository when both variables are set, e.g.
    //   TELESCODE_HEADLESS=<build>/Release/TelescodeHeadless.exe
    //   TELESCODE_TEST_REPOSITORY=https://github.com/sherlock-project/sherlock
    // and is a no-op otherwise, so `cargo test` needs no network.
    #[test]
    fn real_repository_round_trip() {
        let (Some(_), Some(url)) = (
            std::env::var_os("TELESCODE_HEADLESS"),
            std::env::var("TELESCODE_TEST_REPOSITORY").ok(),
        ) else {
            return;
        };
        let cache = std::env::temp_dir().join(format!("telescode-repository-{}", std::process::id()));
        let db = open(&cache, &url).unwrap();
        let out = bridge::run(&bridge::locate_sidecar().unwrap(), bridge::Operation::Analyze, db.to_str().unwrap()).unwrap();
        let v: serde_json::Value = serde_json::from_str(&out).unwrap();
        assert!(v["totals"]["fileCount"].as_u64().unwrap() > 0);

        // Opening again replaces the previous download rather than failing on it.
        assert_eq!(open(&cache, &url).unwrap(), db);
        let _ = fs::remove_dir_all(&cache);
    }
}

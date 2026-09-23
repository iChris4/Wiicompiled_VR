//! Mod archives for the launcher's Patches page (`org.wiicompiled.quest.launcher.ModArchive`).
//!
//! WheelWizard unpacks every mod it installs with SharpCompress, which reads .zip, .7z and .rar,
//! and GameBanana serves all three. Android's java.util.zip covers .zip; this module covers the
//! other two in pure Rust: sevenz-rust2 for 7z and rars for RAR (1.5 to 4.x, and RAR5 onwards).
//! Both decoders check each file's stored CRC or hash, so a damaged download fails instead of
//! installing broken files.
//!
//! Only regular files are written, and only below the destination: a name that climbs out with
//! `..` fails the whole archive, as in the launcher's own zip reader. Directories and links write
//! nothing.

use std::{
    cell::Cell,
    fs::{self, File},
    io::{self, Read, Write},
    path::{Path, PathBuf},
    rc::Rc,
};

use sevenz_rust2::{ArchiveEntry, ArchiveReader, Password};

pub enum Failure {
    Error(String),
    Cancelled,
}

impl<E: std::fmt::Display> From<E> for Failure {
    fn from(error: E) -> Self { Failure::Error(error.to_string()) }
}

type Outcome<T> = Result<T, Failure>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    SevenZip,
    Rar,
}

const SEVEN_ZIP_SIGNATURE: &[u8] = &[b'7', b'z', 0xBC, 0xAF, 0x27, 0x1C];
/// Followed by 0x00 for RAR 1.5 to 4.x and by 0x01 0x00 for RAR5.
const RAR_SIGNATURE: &[u8] = b"Rar!\x1a\x07";

/// Unix file type bits 7-Zip keeps in the high half of the attributes when 0x8000 is set.
const SEVEN_ZIP_UNIX_EXTENSION: u32 = 0x8000;
const UNIX_FILE_TYPE: u32 = 0o170000;
const UNIX_SYMLINK: u32 = 0o120000;

const COPY_CHUNK: usize = 256 * 1024;

/// The archive format a file starts with, by its signature rather than its name.
pub fn kind(header: &[u8]) -> Option<Kind> {
    if header.starts_with(SEVEN_ZIP_SIGNATURE) {
        Some(Kind::SevenZip)
    } else if header.starts_with(RAR_SIGNATURE) {
        Some(Kind::Rar)
    } else {
        None
    }
}

/// Unpacks `archive` into `destination`, which must exist, and returns how many files it wrote.
/// `progress` gets the bytes written so far and the archive's unpacked total, and returns false
/// to cancel.
pub fn extract(archive: &Path, destination: &Path, progress: &mut dyn FnMut(u64, u64) -> bool) -> Outcome<u64> {
    let mut header = [0u8; 8];
    let read = File::open(archive)?.read(&mut header)?;
    match kind(&header[..read]) {
        Some(Kind::SevenZip) => extract_seven_zip(archive, destination, progress),
        Some(Kind::Rar) => extract_rar(archive, destination, progress),
        None => Err(Failure::Error("The file is neither a 7z nor a RAR archive.".into())),
    }
}

/// Where an archive member named `name` goes below `destination`: the launcher's zip rule, where
/// either slash separates folders (Windows archivers store backslashes), leading and empty parts
/// are dropped, and `..` anywhere is refused.
pub fn member_path(destination: &Path, name: &str) -> Outcome<PathBuf> {
    let mut path = destination.to_path_buf();
    let mut parts = 0;
    for part in name.split(['/', '\\']) {
        match part {
            "" | "." => {}
            ".." => return Err(Failure::Error(format!("The archive has a file outside its own folder ({name})."))),
            _ => {
                path.push(part);
                parts += 1;
            }
        }
    }
    if parts == 0 {
        return Err(Failure::Error(format!("The archive has a file with no usable name ({name}).")));
    }
    Ok(path)
}

fn create(path: &Path) -> Outcome<File> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    Ok(File::create(path)?)
}

fn extract_seven_zip(archive: &Path, destination: &Path, progress: &mut dyn FnMut(u64, u64) -> bool) -> Outcome<u64> {
    let mut reader = ArchiveReader::open(archive, Password::empty()).map_err(seven_zip_error)?;
    let total: u64 = reader.archive().files.iter().filter(|entry| wanted_seven_zip(entry)).map(|entry| entry.size).sum();
    if !progress(0, total) {
        return Err(Failure::Cancelled);
    }
    let mut done = 0u64;
    let mut files = 0u64;
    // The callback can only return the decoder's error type, so ours waits here while the walk stops.
    let mut failure: Option<Failure> = None;
    let walked = reader.for_each_entries(|entry, data| {
        if !wanted_seven_zip(entry) {
            // A solid block decodes in order: whatever this entry holds must still be read past.
            io::copy(data, &mut io::sink())?;
            return Ok(true);
        }
        let step = (|| -> Outcome<()> {
            let mut file = create(&member_path(destination, &entry.name)?)?;
            let mut buffer = vec![0u8; COPY_CHUNK];
            loop {
                let read = data.read(&mut buffer).map_err(seven_zip_read_error)?;
                if read == 0 {
                    break;
                }
                file.write_all(&buffer[..read])?;
                done += read as u64;
                if !progress(done, total) {
                    return Err(Failure::Cancelled);
                }
            }
            files += 1;
            Ok(())
        })();
        match step {
            Ok(()) => Ok(true),
            Err(error) => {
                failure = Some(error);
                Ok(false)
            }
        }
    });
    if let Some(failure) = failure {
        return Err(failure);
    }
    walked.map_err(seven_zip_error)?;
    Ok(files)
}

/// A file with contents to write: not a directory, a deletion marker or a symbolic link.
fn wanted_seven_zip(entry: &ArchiveEntry) -> bool {
    let link = entry.has_windows_attributes
        && entry.windows_attributes & SEVEN_ZIP_UNIX_EXTENSION != 0
        && (entry.windows_attributes >> 16) & UNIX_FILE_TYPE == UNIX_SYMLINK;
    !entry.is_directory && !entry.is_anti_item && !link
}

fn seven_zip_error(error: sevenz_rust2::Error) -> Failure {
    use sevenz_rust2::Error;
    match error {
        // AES is left out of the build: nothing here could ask for the password anyway.
        Error::PasswordRequired | Error::MaybeBadPassword(_) => Failure::Error("The archive is password-protected.".into()),
        Error::UnsupportedCompressionMethod(method) if method.starts_with("AES") => {
            Failure::Error("The archive is password-protected.".into())
        }
        Error::UnsupportedCompressionMethod(method) => {
            Failure::Error(format!("The 7z archive uses a compression method this headset cannot unpack ({method})."))
        }
        Error::ChecksumVerificationFailed => damaged(),
        other => Failure::Error(format!("The 7z archive could not be unpacked: {other}")),
    }
}

/// A decoder error met while reading a file's contents, which the decoder wraps in io::Error.
fn seven_zip_read_error(error: io::Error) -> Failure {
    match error.get_ref().and_then(|inner| inner.downcast_ref::<sevenz_rust2::Error>()) {
        Some(sevenz_rust2::Error::ChecksumVerificationFailed) => damaged(),
        _ => Failure::Error(format!("The 7z archive could not be unpacked: {error}")),
    }
}

fn damaged() -> Failure {
    Failure::Error("The archive is damaged: a file in it does not match its checksum.".into())
}

fn extract_rar(archive: &Path, destination: &Path, progress: &mut dyn FnMut(u64, u64) -> bool) -> Outcome<u64> {
    let parsed = rars::ArchiveReader::read_path(archive).map_err(rar_error)?;
    let members: Vec<_> = parsed.members().map(|member| member.meta).collect();
    if members.iter().any(|member| member.is_split_before || member.is_split_after) {
        return Err(Failure::Error("The archive is one part of a split RAR archive, which cannot be installed on its own.".into()));
    }
    if members.iter().any(|member| member.is_encrypted) {
        return Err(Failure::Error("The archive is password-protected.".into()));
    }
    let total: u64 = members.iter().filter(|member| !member.is_directory).map(|member| member.unpacked_size).sum();
    if !progress(0, total) {
        return Err(Failure::Cancelled);
    }
    // The writers rars asks for must own what they use, so they count into a shared cell, and
    // progress is reported as each file starts. Links never reach the callback.
    let written = Rc::new(Cell::new(0u64));
    let mut files = 0u64;
    let mut failure: Option<Failure> = None;
    let extracted = parsed.extract_to(None, |meta| {
        if meta.is_directory {
            return Ok(Box::new(io::sink()) as Box<dyn Write>);
        }
        let opened = (|| -> Outcome<File> {
            if !progress(written.get(), total) {
                return Err(Failure::Cancelled);
            }
            create(&member_path(destination, &String::from_utf8_lossy(&meta.name))?)
        })();
        match opened {
            Ok(file) => {
                files += 1;
                Ok(Box::new(CountingWriter { inner: file, written: Rc::clone(&written) }) as Box<dyn Write>)
            }
            Err(error) => {
                let message = match &error {
                    Failure::Error(message) => message.clone(),
                    Failure::Cancelled => "cancelled".into(),
                };
                failure = Some(error);
                Err(rars::Error::from(io::Error::other(message)))
            }
        }
    });
    if let Some(failure) = failure {
        return Err(failure);
    }
    extracted.map_err(rar_error)?;
    progress(written.get(), total);
    Ok(files)
}

struct CountingWriter {
    inner: File,
    written: Rc<Cell<u64>>,
}

impl Write for CountingWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        let count = self.inner.write(data)?;
        self.written.set(self.written.get() + count as u64);
        Ok(count)
    }

    fn flush(&mut self) -> io::Result<()> { self.inner.flush() }
}

fn rar_error(error: rars::Error) -> Failure {
    match error {
        rars::Error::NeedPassword => Failure::Error("The archive is password-protected.".into()),
        other => Failure::Error(format!("The RAR archive could not be unpacked: {other}")),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn message(outcome: Outcome<PathBuf>) -> String {
        match outcome {
            Ok(path) => panic!("accepted {}", path.display()),
            Err(Failure::Error(message)) => message,
            Err(Failure::Cancelled) => panic!("cancelled"),
        }
    }

    #[test]
    fn formats_are_told_by_their_signature() {
        assert_eq!(kind(b"7z\xBC\xAF\x27\x1C\x00\x04"), Some(Kind::SevenZip));
        assert_eq!(kind(b"Rar!\x1a\x07\x00\xcf"), Some(Kind::Rar));
        assert_eq!(kind(b"Rar!\x1a\x07\x01\x00"), Some(Kind::Rar));
        assert_eq!(kind(b"PK\x03\x04"), None);
        assert_eq!(kind(b"7z"), None);
    }

    #[test]
    fn members_stay_below_the_destination() {
        let root = Path::new("staging");
        assert_eq!(member_path(root, "Patches\\Menu.szs").ok(), Some(root.join("Patches").join("Menu.szs")));
        assert_eq!(member_path(root, "/Common.szs").ok(), Some(root.join("Common.szs")));
        assert_eq!(member_path(root, "a/./b//c.szs").ok(), Some(root.join("a").join("b").join("c.szs")));
        for name in ["../escape.szs", "a/../../escape.szs", "a\\..\\b.szs", ".."] {
            assert!(message(member_path(root, name)).contains("outside"), "{name}");
        }
        for name in ["", "/", "./."] {
            assert!(message(member_path(root, name)).contains("no usable name"), "{name}");
        }
    }
}

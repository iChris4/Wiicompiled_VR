//! JNI bridge from the Quest launcher (`org.wiicompiled.quest.launcher.NodDisc`) to nod.
//!
//! The PC installer validates and extracts the player's disc with nodtool. This library gives
//! the Quest launcher the same three operations on a file descriptor from Android's document
//! picker: read the disc header, read one file of the data partition (so the launcher can hash
//! main.dol and StaticR.rel before a long extraction), and extract the data partition.
//!
//! The extraction is nodtool's `extract` command (nodtool/src/cmd/extract.rs at
//! v2.0.0-alpha.10, MIT OR Apache-2.0) with progress reporting and cancellation added, so both
//! installers write the same `sys/`, `files/`, `disc/` and `*.bin` layout that the runtime's DVD
//! layer reads.
//!
//! It also unpacks the .7z and .rar mods the Patches page installs
//! (`org.wiicompiled.quest.launcher.ModArchive`, see `archive.rs`).

mod archive;

use std::{
    fs::{self, File},
    io::{self, BufRead, Read, Write},
    os::{
        fd::{BorrowedFd, RawFd},
        unix::fs::FileExt,
    },
    panic::{AssertUnwindSafe, catch_unwind},
    path::Path,
    sync::Arc,
    time::{Duration, Instant},
};

use jni::{
    JNIEnv,
    objects::{JObject, JString, JValue},
    sys::{jbyteArray, jint, jlong, jstring},
};
use nod::{
    common::PartitionKind,
    disc::fst::Fst,
    read::{DiscOptions, DiscReader, DiscStream, PartitionMeta, PartitionOptions, PartitionReader},
};
use zerocopy::IntoBytes;

/// Thrown instead of IOException when the listener asked to stop.
const CANCELLED_EXCEPTION: &str = "java/io/InterruptedIOException";
const PROGRESS_INTERVAL: Duration = Duration::from_millis(200);

enum Failure {
    Error(String),
    Cancelled,
}

impl<E: std::fmt::Display> From<E> for Failure {
    fn from(error: E) -> Self { Failure::Error(error.to_string()) }
}

type Outcome<T> = Result<T, Failure>;

/// A disc image read with pread through its own duplicate of the picker's descriptor. pread
/// keeps no file position, so nod's preloader threads can each hold a clone.
#[derive(Clone)]
struct FdStream {
    file: Arc<File>,
    len: u64,
}

impl DiscStream for FdStream {
    fn read_exact_at(&mut self, buf: &mut [u8], offset: u64) -> io::Result<()> {
        self.file.read_exact_at(buf, offset)
    }

    fn stream_len(&mut self) -> io::Result<u64> { Ok(self.len) }
}

fn open_disc(fd: RawFd) -> Outcome<DiscReader> {
    // SAFETY: the caller keeps the ParcelFileDescriptor open for the duration of the JNI call,
    // and only a duplicate outlives this borrow.
    let owned = unsafe { BorrowedFd::borrow_raw(fd) }.try_clone_to_owned()?;
    let file = File::from(owned);
    let len = file.metadata()?.len();
    let options = DiscOptions { preloader_threads: 4, ..Default::default() };
    Ok(DiscReader::new_stream(Box::new(FdStream { file: Arc::new(file), len }), &options)?)
}

fn open_data_partition(disc: &DiscReader) -> Outcome<Box<dyn PartitionReader>> {
    Ok(disc.open_partition_kind(PartitionKind::Data, &PartitionOptions::default())?)
}

/// Bytes an extraction writes: the system files plus every file in the FST.
fn extracted_size(disc: &DiscReader, meta: &PartitionMeta) -> Outcome<u64> {
    let mut total = (meta.raw_boot.len()
        + meta.raw_bi2.len()
        + meta.raw_apploader.len()
        + meta.raw_fst.len()
        + meta.raw_dol.len()) as u64;
    if disc.header().is_wii() {
        total += 0x100 + disc.region().map_or(0, |region| region.len()) as u64;
        for part in [&meta.raw_ticket, &meta.raw_tmd, &meta.raw_cert_chain] {
            total += part.as_ref().map_or(0, |bytes| bytes.len()) as u64;
        }
        total += meta.raw_h3_table.as_ref().map_or(0, |bytes| bytes.len()) as u64;
    }
    let fst = Fst::new(&meta.raw_fst)?;
    for (_, node, _) in fst.iter() {
        if node.is_file() {
            total += u64::from(node.length());
        }
    }
    Ok(total)
}

/// "<game id>\n<title>\n<bytes an extraction writes>"
fn header(fd: RawFd) -> Outcome<String> {
    let disc = open_disc(fd)?;
    let mut partition = open_data_partition(&disc)?;
    let meta = partition.meta()?;
    let total = extracted_size(&disc, &meta)?;
    let header = disc.header();
    Ok(format!("{}\n{}\n{}", header.game_id_str(), header.game_title_str(), total))
}

/// `sys/main.dol`, or a path below `files/`, of the data partition.
fn read_file(fd: RawFd, path: &str) -> Outcome<Vec<u8>> {
    let disc = open_disc(fd)?;
    let mut partition = open_data_partition(&disc)?;
    let meta = partition.meta()?;
    if path == "sys/main.dol" {
        return Ok(meta.raw_dol.to_vec());
    }
    let Some(relative) = path.strip_prefix("files/") else {
        return Err(Failure::Error(format!("Unsupported disc path {path}")));
    };
    let fst = Fst::new(&meta.raw_fst)?;
    let Some((_, node)) = fst.find(relative) else {
        return Err(Failure::Error(format!("{path} is not on this disc")));
    };
    let mut bytes = Vec::with_capacity(node.length() as usize);
    partition.open_file(node)?.read_to_end(&mut bytes)?;
    Ok(bytes)
}

struct Progress<'a> {
    done: u64,
    total: u64,
    last: Instant,
    report: &'a mut dyn FnMut(u64, u64) -> Outcome<bool>,
}

impl Progress<'_> {
    fn advance(&mut self, bytes: usize) -> Outcome<()> {
        self.done += bytes as u64;
        if self.last.elapsed() >= PROGRESS_INTERVAL {
            self.last = Instant::now();
            self.check()?;
        }
        Ok(())
    }

    fn check(&mut self) -> Outcome<()> {
        if (self.report)(self.done, self.total)? { Ok(()) } else { Err(Failure::Cancelled) }
    }
}

fn extract(fd: RawFd, out_dir: &Path, report: &mut dyn FnMut(u64, u64) -> Outcome<bool>) -> Outcome<()> {
    let disc = open_disc(fd)?;
    let mut partition = open_data_partition(&disc)?;
    let meta = partition.meta()?;
    let total = extracted_size(&disc, &meta)?;
    let mut progress = Progress { done: 0, total, last: Instant::now(), report };
    progress.check()?;

    extract_sys_files(&disc, &meta, out_dir, &mut progress)?;

    let files_dir = out_dir.join("files");
    fs::create_dir_all(&files_dir)?;
    let is_wii = disc.header().is_wii();
    let fst = Fst::new(&meta.raw_fst)?;
    for (_, node, path) in fst.iter() {
        let target = files_dir.join(&path);
        if node.is_dir() {
            fs::create_dir_all(&target).map_err(|e| format!("Creating directory {path}: {e}"))?;
            continue;
        }
        let mut file = File::create(&target).map_err(|e| format!("Creating file {path}: {e}"))?;
        let mut reader = partition.open_file(node).map_err(|e| {
            format!("Opening {path} on the disc (offset {}, size {}): {e}", node.offset(is_wii), node.length())
        })?;
        loop {
            let buf = reader.fill_buf().map_err(|e| format!("Reading {path}: {e}"))?;
            let len = buf.len();
            if len == 0 {
                break;
            }
            file.write_all(buf).map_err(|e| format!("Writing {path}: {e}"))?;
            reader.consume(len);
            progress.advance(len)?;
        }
        file.flush().map_err(|e| format!("Writing {path}: {e}"))?;
    }
    progress.check()
}

fn extract_sys_files(disc: &DiscReader, meta: &PartitionMeta, out_dir: &Path, progress: &mut Progress) -> Outcome<()> {
    let sys_dir = out_dir.join("sys");
    fs::create_dir_all(&sys_dir)?;
    write_file(meta.raw_boot.as_ref(), &sys_dir.join("boot.bin"), progress)?;
    write_file(meta.raw_bi2.as_ref(), &sys_dir.join("bi2.bin"), progress)?;
    write_file(meta.raw_apploader.as_ref(), &sys_dir.join("apploader.img"), progress)?;
    write_file(meta.raw_fst.as_ref(), &sys_dir.join("fst.bin"), progress)?;
    write_file(meta.raw_dol.as_ref(), &sys_dir.join("main.dol"), progress)?;

    let header = disc.header();
    if header.is_wii() {
        let disc_dir = out_dir.join("disc");
        fs::create_dir_all(&disc_dir)?;
        write_file(&header.as_bytes()[..0x100], &disc_dir.join("header.bin"), progress)?;
        if let Some(region) = disc.region() {
            write_file(region, &disc_dir.join("region.bin"), progress)?;
        }
        if let Some(ticket) = meta.raw_ticket.as_deref() {
            write_file(ticket, &out_dir.join("ticket.bin"), progress)?;
        }
        if let Some(tmd) = meta.raw_tmd.as_deref() {
            write_file(tmd, &out_dir.join("tmd.bin"), progress)?;
        }
        if let Some(cert_chain) = meta.raw_cert_chain.as_deref() {
            write_file(cert_chain, &out_dir.join("cert.bin"), progress)?;
        }
        if let Some(h3_table) = meta.raw_h3_table.as_deref() {
            write_file(h3_table, &out_dir.join("h3.bin"), progress)?;
        }
    }
    Ok(())
}

fn write_file(bytes: &[u8], path: &Path, progress: &mut Progress) -> Outcome<()> {
    fs::write(path, bytes).map_err(|e| format!("Writing {}: {e}", path.display()))?;
    progress.advance(bytes.len())
}

/// Runs `body`, turning a failure or a panic into a pending Java exception.
fn guarded<T>(env: &mut JNIEnv, fallback: T, body: impl FnOnce(&mut JNIEnv) -> Outcome<T>) -> T {
    match catch_unwind(AssertUnwindSafe(|| body(env))) {
        Ok(Ok(value)) => value,
        Ok(Err(failure)) => {
            // A Java exception raised inside a callback is already pending; keep it.
            if !env.exception_check().unwrap_or(false) {
                let _ = match failure {
                    Failure::Error(message) => env.throw_new("java/io/IOException", message),
                    Failure::Cancelled => env.throw_new(CANCELLED_EXCEPTION, "Extraction cancelled"),
                };
            }
            fallback
        }
        Err(_) => {
            let _ = env.throw_new("java/io/IOException", "nod-jni panicked; see logcat");
            fallback
        }
    }
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_org_wiicompiled_quest_launcher_NodDisc_header<'local>(
    mut env: JNIEnv<'local>,
    _this: JObject<'local>,
    fd: jint,
) -> jstring {
    guarded(&mut env, std::ptr::null_mut(), |env| Ok(env.new_string(header(fd)?)?.into_raw()))
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_org_wiicompiled_quest_launcher_NodDisc_readFile<'local>(
    mut env: JNIEnv<'local>,
    _this: JObject<'local>,
    fd: jint,
    path: JString<'local>,
) -> jbyteArray {
    guarded(&mut env, std::ptr::null_mut(), |env| {
        let path: String = env.get_string(&path)?.into();
        let bytes = read_file(fd, &path)?;
        Ok(env.byte_array_from_slice(&bytes)?.into_raw())
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_org_wiicompiled_quest_launcher_NodDisc_extract<'local>(
    mut env: JNIEnv<'local>,
    _this: JObject<'local>,
    fd: jint,
    out_dir: JString<'local>,
    listener: JObject<'local>,
) {
    guarded(&mut env, (), |env| {
        let out_dir: String = env.get_string(&out_dir)?.into();
        let mut report = |done: u64, total: u64| -> Outcome<bool> {
            let result = env.call_method(
                &listener,
                "update",
                "(JJ)Z",
                &[JValue::Long(done as i64), JValue::Long(total as i64)],
            );
            Ok(result?.z()?)
        };
        extract(fd, Path::new(&out_dir), &mut report)
    })
}

#[unsafe(no_mangle)]
pub extern "system" fn Java_org_wiicompiled_quest_launcher_ModArchive_extract<'local>(
    mut env: JNIEnv<'local>,
    _this: JObject<'local>,
    archive_path: JString<'local>,
    out_dir: JString<'local>,
    listener: JObject<'local>,
) -> jlong {
    guarded(&mut env, 0, |env| {
        let archive_path: String = env.get_string(&archive_path)?.into();
        let out_dir: String = env.get_string(&out_dir)?.into();
        // An exception thrown by the listener stops the extraction and stays pending for Java.
        let mut report = |done: u64, total: u64| -> bool {
            env.call_method(&listener, "update", "(JJ)Z", &[JValue::Long(done as i64), JValue::Long(total as i64)])
                .and_then(|value| value.z())
                .unwrap_or(false)
        };
        match archive::extract(Path::new(&archive_path), Path::new(&out_dir), &mut report) {
            Ok(files) => Ok(files as jlong),
            Err(archive::Failure::Error(message)) => Err(Failure::Error(message)),
            Err(archive::Failure::Cancelled) => Err(Failure::Cancelled),
        }
    })
}

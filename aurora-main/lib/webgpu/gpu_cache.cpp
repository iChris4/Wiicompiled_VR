#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <filesystem>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../fs_helper.hpp"
#include "../internal.hpp"
#include "../sqlite_utils.hpp"
#include "gpu.hpp"

#include <sqlite3.h>
#include <fmt/format.h>
#if defined(AURORA_CACHE_USE_ZSTD)
#include <zstd.h>
#endif
#define XXH_STATIC_LINKING_ONLY
#include <xxhash.h>

namespace aurora::webgpu {
static Module Log("aurora::gpu::cache");

static sqlite3* db;
static sqlite3_stmt* load_stmt;
static sqlite3_stmt* store_stmt;
static sqlite3_stmt* touch_stmt;
static bool cache_broken;
static std::mutex cache_mutex;

// Dawn calls store_to_cache while holding its device lock: the monolithic Vulkan pipeline cache is
// serialized under that lock and handed over as one blob (26 MiB on a desktop GPU, 58 MiB on a
// Quest 3), and compressing and committing it there held every other user of the device for the
// whole write, 0.4 s on a desktop and 0.9 to 3.8 s on a Quest at each race exit, long enough to
// stall the game thread and its music. The callback now only copies the blob and queues it, and
// one writer thread compresses and commits. Loads look at the queue first, so a stored blob can
// be read back at once. The newest content of each large blob is remembered by hash, so Dawn
// re-serializing an unchanged pipeline cache costs a hash instead of a rewrite.
struct KeyHasher {
  size_t operator()(const XXH128_hash_t& hash) const noexcept { return static_cast<size_t>(hash.low64 ^ hash.high64); }
};
struct KeyEqual {
  bool operator()(const XXH128_hash_t& a, const XXH128_hash_t& b) const noexcept { return XXH128_isEqual(a, b) != 0; }
};
using QueuedBlob = std::shared_ptr<const std::vector<uint8_t>>;

constexpr size_t LargeBlobBytes = size_t{1} << 20;

// Lock order: g_writeMutex and cache_mutex are never held together.
static std::mutex g_writeMutex;
static std::condition_variable g_writeCv;
// The newest content per key not yet committed. A key is in g_writeOrder at most once; a key in
// g_queuedBlobs but not in g_writeOrder is the one being written.
static std::unordered_map<XXH128_hash_t, QueuedBlob, KeyHasher, KeyEqual> g_queuedBlobs;
static std::deque<XXH128_hash_t> g_writeOrder;
static std::unordered_map<XXH128_hash_t, XXH128_hash_t, KeyHasher, KeyEqual> g_largeContent;
static std::thread g_writerThread;
static bool g_writerStop = false;
static bool g_writerBusy = false;

// Schema 3 added last_used (whole days since the Unix epoch) so stale blobs can be
// pruned: config-version bumps and driver updates change every Dawn cache key, and
// without an age column the orphaned rows accumulate forever (observed 1.6 GB).
// Schema 4 is a content reset for the vulkan_monolithic_pipeline_cache switch, which
// obsoletes every per-pipeline blob at once.
constexpr int CACHE_SCHEMA = 4;
constexpr int64_t PruneAfterDays = 30;
// Rows whose last_used lags today are collected in memory and written in one batch at
// shutdown: the load path sits inside a read transaction that is always rolled back,
// and day granularity makes anything more eager pointless.

static std::atomic<uint64_t> g_lookups{0};
static std::atomic<uint64_t> g_hits{0};
static std::atomic<uint64_t> g_stores{0};
static std::atomic<uint64_t> g_hitBytes{0};
static std::atomic<uint64_t> g_unchanged{0};
static std::vector<XXH128_hash_t> g_pendingTouches;

static int64_t days_now() { return static_cast<int64_t>(std::time(nullptr) / 86400); }

static void init_abort() {
  cache_broken = true;
  sqlite3_close(db);
  db = nullptr;
}

static int check(int ret) {
  if (ret != SQLITE_OK) {
    Log.error("SQLite operation failed: {}", sqlite3_errmsg(db));
  }

  return ret;
}

enum class SchemaState { Match, Mismatch, Error };

static SchemaState check_schema() {
  auto ret = sqlite::exec(db, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema table: {}", sqlite3_errmsg(db));
    return SchemaState::Error;
  }

  bool match = false;
  const auto cmd = fmt::format("SELECT * FROM aurora_schema WHERE value = {}", CACHE_SCHEMA);
  ret = sqlite::exec(db, cmd.c_str(), [&match](int, char**, char**) { match = true; }, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to check schema table: {}", sqlite3_errmsg(db));
    return SchemaState::Error;
  }
  return match ? SchemaState::Match : SchemaState::Mismatch;
}

static bool create_schema() {
  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open schema transaction: {}", sqlite3_errmsg(db));
    return false;
  }
  const auto cmd = fmt::format(
      R"(CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);
DROP TABLE IF EXISTS cache;
CREATE TABLE cache (
  key BLOB PRIMARY KEY NOT NULL,
  value BLOB NOT NULL,
  size INTEGER NOT NULL,
  compressed INTEGER NOT NULL,
  last_used INTEGER NOT NULL DEFAULT 0
);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
      CACHE_SCHEMA);
  const auto ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to create schema: {}", sqlite3_errmsg(db));
    return false;
  }
  tx.commit();
  return true;
}

static void prune_stale_rows() {
  const auto cmd = fmt::format("DELETE FROM cache WHERE last_used < {}", days_now() - PruneAfterDays);
  auto ret = sqlite::exec(db, cmd.c_str());
  if (ret != SQLITE_OK) {
    Log.error("Failed to prune stale cache rows: {}", sqlite3_errmsg(db));
    return;
  }
  const auto pruned = sqlite3_changes(db);
  if (pruned > 0) {
    Log.info("Pruned {} stale Dawn cache blobs (unused for {}+ days)", pruned, PruneAfterDays);
  }
  // Freed pages are only reused, never returned to the filesystem, so compact when a
  // meaningful amount was dropped. Blobs run hundreds of KB each, making even a few
  // hundred rows a noticeable slice of the file.
  if (pruned > 256) {
    ret = sqlite::exec(db, "VACUUM;");
    if (ret != SQLITE_OK) {
      Log.warn("Failed to vacuum Dawn cache after pruning: {}", sqlite3_errmsg(db));
    }
  }
}

static bool cache_init_core() {
  Log.debug("SQLite version {}", sqlite3_libversion());

  const auto path = fs_path_from_string(g_config.cachePath) / "dawn_cache.db";
  std::string file = fs_path_to_string(path);
  Log.debug("Using dawn cache at {}", file);
  auto ret = sqlite3_open(file.c_str(), &db);
  if (ret != SQLITE_OK) {
    Log.error("Failed to open database: {}", sqlite3_errmsg(db));
    return false;
  }

  // WAL mode + NORMAL = no need for disk syncs, consistent but not durable is fine.
  ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
  if (ret != SQLITE_OK) {
    Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
    return false;
  }

  switch (check_schema()) {
  case SchemaState::Match:
    prune_stale_rows();
    break;
  case SchemaState::Mismatch: {
    // Dropping the table would leave the freed pages inside the file, so a schema
    // change deletes the database outright; pre-schema-3 files had grown unbounded.
    Log.info("Dawn cache schema changed; recreating '{}'", file);
    sqlite3_close(db);
    db = nullptr;
    std::error_code ec;
    std::filesystem::remove(path, ec);
    auto wal = path;
    wal += "-wal";
    std::filesystem::remove(wal, ec);
    auto shm = path;
    shm += "-shm";
    std::filesystem::remove(shm, ec);
    ret = sqlite3_open(file.c_str(), &db);
    if (ret != SQLITE_OK) {
      Log.error("Failed to recreate database: {}", sqlite3_errmsg(db));
      return false;
    }
    ret = sqlite::exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
    if (ret != SQLITE_OK) {
      Log.error("Failed to set pragmas: {}", sqlite3_errmsg(db));
      return false;
    }
    if (!create_schema()) {
      return false;
    }
    break;
  }
  case SchemaState::Error:
    return false;
  }

  ret = sqlite3_prepare_v3(db, "SELECT value, size, compressed, last_used FROM cache WHERE key = ?", -1,
                           SQLITE_PREPARE_PERSISTENT, &load_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "REPLACE INTO cache (key, value, size, compressed, last_used) VALUES (?, ?, ?, ?, ?)",
                           -1, SQLITE_PREPARE_PERSISTENT, &store_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  ret = sqlite3_prepare_v3(db, "UPDATE cache SET last_used = ? WHERE key = ?", -1, SQLITE_PREPARE_PERSISTENT,
                           &touch_stmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.error("Failed to prepare statement: {}", sqlite3_errmsg(db));
    return false;
  }

  return true;
}

// Caller holds cache_mutex. Writes the queued last_used refreshes in one transaction.
static void flush_touches() {
  if (g_pendingTouches.empty() || db == nullptr || touch_stmt == nullptr) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open touch transaction");
    g_pendingTouches.clear();
    return;
  }
  const auto today = days_now();
  for (const auto& keyHash : g_pendingTouches) {
    check(sqlite3_bind_int64(touch_stmt, 1, today));
    check(sqlite3_bind_blob(touch_stmt, 2, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
    if (sqlite3_step(touch_stmt) != SQLITE_DONE) {
      Log.error("Failed to refresh cache row age: {}", sqlite3_errmsg(db));
    }
    check(sqlite3_reset(touch_stmt));
  }
  g_pendingTouches.clear();
  tx.commit();
}

static bool cache_init() {
  if (cache_broken) {
    return false;
  }

  if (db) {
    return true;
  }

  if (!cache_init_core()) {
    Log.error("SQLite DB init failed");
    init_abort();
    return false;
  }

  Log.debug("SQLite cache init succeeded");

  return true;
}

// Caller holds cache_mutex. `complete` is set when `value` received the whole entry.
static size_t load_from_database(const XXH128_hash_t& keyHash, void* value, size_t valueSize, bool& complete) {
  complete = false;
  if (!cache_init()) {
    return 0;
  }

  sqlite::Transaction tx(db, Log);
  if (!tx) {
    Log.error("Failed to open load transaction");
    return 0;
  }

  check(sqlite3_bind_blob(load_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));

  const auto ret = sqlite3_step(load_stmt);
  size_t foundSize;
  if (ret == SQLITE_ROW) {
    // Hit
    const auto foundPtr = sqlite3_column_blob(load_stmt, 0);
    foundSize = sqlite3_column_int64(load_stmt, 1);
    const bool compressed = sqlite3_column_int(load_stmt, 2) != 0;
    if (value == nullptr) {
      g_hits.fetch_add(1, std::memory_order_relaxed);
    } else {
      g_hitBytes.fetch_add(static_cast<uint64_t>(foundSize), std::memory_order_relaxed);
    }
    if (sqlite3_column_int64(load_stmt, 3) != days_now()) {
      g_pendingTouches.push_back(keyHash);
    }

    if (value && valueSize == foundSize) {
      if (compressed) {
#if defined(AURORA_CACHE_USE_ZSTD)
        const auto compSize = sqlite3_column_bytes(load_stmt, 0);
        const auto zstdRet = ZSTD_decompress(value, valueSize, foundPtr, compSize);
        if (ZSTD_isError(zstdRet)) {
          Log.error("zstd decompression error: {}", ZSTD_getErrorName(zstdRet));
          foundSize = 0;
        } else if (zstdRet != foundSize) {
          Log.error("zstd decompression size mismatch: expected {}, got {}", foundSize, zstdRet);
          foundSize = 0;
        }
#else
        Log.error("Cache entry is zstd-compressed but zstd support is disabled");
        foundSize = 0;
#endif
      } else {
        if (foundSize != 0 && !foundPtr) {
          Log.error("Cache entry is missing raw value data");
          foundSize = 0;
        } else if (foundSize != 0) {
          std::memcpy(value, foundPtr, foundSize);
        }
      }
      complete = foundSize != 0;
    }
  } else if (ret == SQLITE_DONE) {
    // Miss
    foundSize = 0;
  } else {
    Log.error("Looking up cache key failed: {}", sqlite3_errmsg(db));
    return 0;
  }

  check(sqlite3_reset(load_stmt));

  return foundSize;
}

size_t load_from_cache(void const* key, size_t keySize, void* value, size_t valueSize, void*) {
  const auto keyHash = XXH128(key, keySize, 0);
  // Dawn probes with value == nullptr for the size first, then fetches; count each
  // probe as one logical lookup so the hit rate reads per-entry.
  if (value == nullptr) {
    g_lookups.fetch_add(1, std::memory_order_relaxed);
  }

  QueuedBlob queued;
  {
    std::lock_guard lock(g_writeMutex);
    if (const auto entry = g_queuedBlobs.find(keyHash); entry != g_queuedBlobs.end()) {
      queued = entry->second;
    }
  }
  if (queued) {
    if (value == nullptr) {
      g_hits.fetch_add(1, std::memory_order_relaxed);
    } else if (valueSize == queued->size()) {
      std::memcpy(value, queued->data(), queued->size());
      g_hitBytes.fetch_add(queued->size(), std::memory_order_relaxed);
    }
    return queued->size();
  }

  bool complete = false;
  size_t foundSize = 0;
  {
    std::lock_guard lock(cache_mutex);
    foundSize = load_from_database(keyHash, value, valueSize, complete);
  }
  if (complete && foundSize >= LargeBlobBytes) {
    // Remember what the database holds, so re-storing the same bytes is skipped. A store queued
    // meanwhile already recorded newer content.
    const auto content = XXH3_128bits(value, foundSize);
    std::lock_guard lock(g_writeMutex);
    g_largeContent.try_emplace(keyHash, content);
  }
  return foundSize;
}

static void write_blob(const XXH128_hash_t& keyHash, const std::vector<uint8_t>& blob,
                       std::vector<uint8_t>& compressBuffer) {
  const void* storedValue = blob.data();
  sqlite3_uint64 storedValueSize = blob.size();
  int compressed = 0;
#if defined(AURORA_CACHE_USE_ZSTD)
  const auto bound = ZSTD_compressBound(blob.size());
  if (ZSTD_isError(bound)) {
    Log.error("Failed to calculate ZSTD_compressBound: {}", ZSTD_getErrorName(bound));
    return;
  }

  if (compressBuffer.size() < bound) {
    compressBuffer.resize(bound);
  }

  const auto compressRet = ZSTD_compress(compressBuffer.data(), compressBuffer.size(), blob.data(), blob.size(), 0);
  if (ZSTD_isError(compressRet)) {
    Log.error("ZSTD compression error: {}", ZSTD_getErrorName(compressRet));
    return;
  }

  if (compressRet < blob.size()) {
    storedValue = compressBuffer.data();
    storedValueSize = compressRet;
    compressed = 1;
  }
#endif

  std::lock_guard lock(cache_mutex);
  if (!cache_init()) {
    return;
  }

  sqlite::Transaction tx(db, Log, true);
  if (!tx) {
    Log.error("Failed to open store transaction");
    return;
  }

  // Both buffers outlive the statement's use of them: the binding is cleared below.
  check(sqlite3_bind_blob64(store_stmt, 1, &keyHash, sizeof(keyHash), SQLITE_TRANSIENT));
  check(sqlite3_bind_blob64(store_stmt, 2, storedValue, storedValueSize, SQLITE_STATIC));
  check(sqlite3_bind_int64(store_stmt, 3, static_cast<sqlite3_int64>(blob.size())));
  check(sqlite3_bind_int(store_stmt, 4, compressed));
  check(sqlite3_bind_int64(store_stmt, 5, days_now()));

  const auto ret = sqlite3_step(store_stmt);
  check(sqlite3_reset(store_stmt));
  check(sqlite3_bind_null(store_stmt, 2));
  if (ret != SQLITE_DONE) {
    Log.error("Failed to insert row: {}", sqlite3_errmsg(db));
    return;
  }

  tx.commit();
}

static void cache_writer_loop() {
  std::vector<uint8_t> compressBuffer;
  for (;;) {
    XXH128_hash_t keyHash{};
    QueuedBlob blob;
    {
      std::unique_lock lock(g_writeMutex);
      g_writeCv.wait(lock, [] { return g_writerStop || !g_writeOrder.empty(); });
      if (g_writeOrder.empty()) {
        // Stopping, and everything queued has been written.
        break;
      }
      keyHash = g_writeOrder.front();
      g_writeOrder.pop_front();
      blob = g_queuedBlobs.at(keyHash);
      g_writerBusy = true;
    }

    const auto start = std::chrono::steady_clock::now();
    write_blob(keyHash, *blob, compressBuffer);
    if (blob->size() >= LargeBlobBytes) {
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
      Log.info("Wrote a {:.1f} MiB Dawn cache blob in {} ms, off the GPU device",
               static_cast<double>(blob->size()) / (1024.0 * 1024.0), elapsed.count());
    }

    {
      std::lock_guard lock(g_writeMutex);
      if (const auto entry = g_queuedBlobs.find(keyHash); entry != g_queuedBlobs.end()) {
        if (entry->second == blob) {
          g_queuedBlobs.erase(entry);
        } else {
          // Stored again while this copy was being written: the newer content still goes out.
          g_writeOrder.push_back(keyHash);
        }
      }
      g_writerBusy = false;
    }
    g_writeCv.notify_all();
  }
}

void store_to_cache(void const* key, size_t keySize, void const* value, size_t valueSize, void*) {
  const auto keyHash = XXH128(key, keySize, 0);
  const auto* bytes = static_cast<const uint8_t*>(value);
  const bool large = valueSize >= LargeBlobBytes;
  const XXH128_hash_t content = large ? XXH3_128bits(value, valueSize) : XXH128_hash_t{};
  if (large) {
    std::lock_guard lock(g_writeMutex);
    if (const auto known = g_largeContent.find(keyHash);
        known != g_largeContent.end() && XXH128_isEqual(known->second, content)) {
      g_unchanged.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }

  // Dawn's buffer is only valid for this call.
  auto blob = std::make_shared<const std::vector<uint8_t>>(bytes, bytes + valueSize);
  {
    std::lock_guard lock(g_writeMutex);
    if (large) {
      g_largeContent.insert_or_assign(keyHash, content);
    }
    if (g_queuedBlobs.insert_or_assign(keyHash, std::move(blob)).second) {
      g_writeOrder.push_back(keyHash);
    }
    if (!g_writerThread.joinable()) {
      g_writerStop = false;
      g_writerThread = std::thread(cache_writer_loop);
    }
  }
  g_stores.fetch_add(1, std::memory_order_relaxed);
  g_writeCv.notify_all();
}

void flush_cache_writes() {
  std::unique_lock lock(g_writeMutex);
  g_writeCv.wait(lock, [] { return g_writeOrder.empty() && !g_writerBusy; });
}

void cache_shutdown() {
  {
    std::lock_guard lock(g_writeMutex);
    g_writerStop = true;
  }
  g_writeCv.notify_all();
  // The writer drains the queue before it exits.
  if (g_writerThread.joinable()) {
    g_writerThread.join();
  }
  {
    std::lock_guard lock(g_writeMutex);
    g_queuedBlobs.clear();
    g_writeOrder.clear();
    g_largeContent.clear();
    g_writerStop = false;
  }

  std::lock_guard lock(cache_mutex);
  flush_touches();
  check(sqlite3_finalize(load_stmt));
  check(sqlite3_finalize(store_stmt));
  check(sqlite3_finalize(touch_stmt));
  load_stmt = nullptr;
  store_stmt = nullptr;
  touch_stmt = nullptr;
  check(sqlite3_close(db));
  db = nullptr;
}

BlobCacheStats blob_cache_stats() noexcept {
  return BlobCacheStats{
      .lookups = g_lookups.load(std::memory_order_relaxed),
      .hits = g_hits.load(std::memory_order_relaxed),
      .stores = g_stores.load(std::memory_order_relaxed),
      .hitBytes = g_hitBytes.load(std::memory_order_relaxed),
      .unchanged = g_unchanged.load(std::memory_order_relaxed),
  };
}

} // namespace aurora::webgpu

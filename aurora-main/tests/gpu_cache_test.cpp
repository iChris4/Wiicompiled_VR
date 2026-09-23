// The Dawn blob cache writes off Dawn's device lock: store_to_cache copies and queues, a writer
// thread compresses and commits. These tests pin what callers rely on: a stored blob reads back
// at once, reaches the database, is not rewritten when unchanged, and survives shutdown.

#include "webgpu/gpu.hpp"

#include "internal.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace aurora {
AuroraConfig g_config{};
void log_internal(AuroraLogLevel, const char*, const char* message, unsigned int len) noexcept {
  std::fprintf(stderr, "%.*s\n", static_cast<int>(len), message);
}
void Module::show_fatal_dialog(const char*, std::string_view) noexcept {}
} // namespace aurora

auto fmt::formatter<AuroraLogLevel>::format(AuroraLogLevel level, format_context& ctx) const
    -> format_context::iterator {
  return fmt::format_to(ctx.out(), "{}", static_cast<int>(level));
}

namespace {

using aurora::webgpu::blob_cache_stats;
using aurora::webgpu::cache_shutdown;
using aurora::webgpu::flush_cache_writes;
using aurora::webgpu::load_from_cache;
using aurora::webgpu::store_to_cache;

class GpuCacheTest : public ::testing::Test {
protected:
  void SetUp() override {
    m_directory = std::filesystem::temp_directory_path() /
                  ("aurora_gpu_cache_test_" + std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()));
    std::filesystem::remove_all(m_directory);
    std::filesystem::create_directories(m_directory);
    m_path = m_directory.string();
    aurora::g_config.cachePath = m_path.c_str();
  }

  void TearDown() override {
    cache_shutdown();
    std::filesystem::remove_all(m_directory);
  }

  static void Store(const std::string& key, const std::vector<uint8_t>& value) {
    store_to_cache(key.data(), key.size(), value.data(), value.size(), nullptr);
  }

  // Dawn's protocol: probe for the size, then fetch into a buffer of that size.
  static std::vector<uint8_t> Load(const std::string& key) {
    const size_t size = load_from_cache(key.data(), key.size(), nullptr, 0, nullptr);
    std::vector<uint8_t> value(size);
    if (size != 0) {
      EXPECT_EQ(load_from_cache(key.data(), key.size(), value.data(), value.size(), nullptr), size);
    }
    return value;
  }

  // Pseudo-random bytes, so compression cannot hide a mix-up between two blobs.
  static std::vector<uint8_t> Bytes(size_t size, uint32_t seed) {
    std::vector<uint8_t> value(size);
    uint32_t state = seed * 2654435761u + 1u;
    for (auto& byte : value) {
      state = state * 1664525u + 1013904223u;
      byte = static_cast<uint8_t>(state >> 24);
    }
    return value;
  }

  std::filesystem::path m_directory;
  std::string m_path;
};

TEST_F(GpuCacheTest, StoredBlobReadsBackBeforeAndAfterItIsCommitted) {
  const auto value = Bytes(4096, 1);
  Store("pipeline", value);
  EXPECT_EQ(Load("pipeline"), value);
  flush_cache_writes();
  EXPECT_EQ(Load("pipeline"), value);
  EXPECT_TRUE(Load("missing").empty());
}

TEST_F(GpuCacheTest, ShutdownWritesWhatIsStillQueued) {
  const auto value = Bytes(3 << 20, 2);
  Store("monolithic", value);
  cache_shutdown();
  // The next call reopens the database: only a committed row can answer now.
  EXPECT_EQ(Load("monolithic"), value);
}

TEST_F(GpuCacheTest, NewestStoreOfAKeyWins) {
  const auto first = Bytes(2 << 20, 3);
  const auto second = Bytes(2 << 20, 4);
  Store("monolithic", first);
  Store("monolithic", second);
  EXPECT_EQ(Load("monolithic"), second);
  flush_cache_writes();
  cache_shutdown();
  EXPECT_EQ(Load("monolithic"), second);
}

TEST_F(GpuCacheTest, UnchangedLargeBlobIsNotRewritten) {
  const auto value = Bytes(2 << 20, 5);
  Store("monolithic", value);
  flush_cache_writes();
  const auto before = blob_cache_stats();
  Store("monolithic", value);
  auto after = blob_cache_stats();
  EXPECT_EQ(after.stores, before.stores);
  EXPECT_EQ(after.unchanged, before.unchanged + 1);

  auto changed = value;
  changed[changed.size() / 2] ^= 0xFF;
  Store("monolithic", changed);
  after = blob_cache_stats();
  EXPECT_EQ(after.stores, before.stores + 1);
  EXPECT_EQ(Load("monolithic"), changed);

  // Changing back is a change too.
  Store("monolithic", value);
  EXPECT_EQ(blob_cache_stats().stores, before.stores + 2);
  EXPECT_EQ(Load("monolithic"), value);
}

TEST_F(GpuCacheTest, BlobLoadedAtStartupIsNotRewrittenUnchanged) {
  const auto value = Bytes(2 << 20, 6);
  Store("monolithic", value);
  cache_shutdown();

  // A new session loads the blob Dawn saved last time, then serializes the same bytes again.
  ASSERT_EQ(Load("monolithic"), value);
  const auto before = blob_cache_stats();
  Store("monolithic", value);
  const auto after = blob_cache_stats();
  EXPECT_EQ(after.stores, before.stores);
  EXPECT_EQ(after.unchanged, before.unchanged + 1);
}

TEST_F(GpuCacheTest, SmallBlobsAreAlwaysWritten) {
  const auto value = Bytes(512, 7);
  Store("shader", value);
  flush_cache_writes();
  const auto before = blob_cache_stats();
  Store("shader", value);
  EXPECT_EQ(blob_cache_stats().stores, before.stores + 1);
  flush_cache_writes();
  EXPECT_EQ(Load("shader"), value);
}

} // namespace

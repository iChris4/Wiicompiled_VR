#include "platform/host_platform.h"

#include <cstdlib>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <pwd.h>
#endif

namespace RuntimePlatform {

std::optional<std::filesystem::path> ExecutableDirectory() noexcept {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
        return std::nullopt;
    }
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::nullopt;
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    return (ec ? std::filesystem::path(path) : resolved).parent_path();
#elif defined(__ANDROID__)
    // Bundled read-only runtime resources (wii_bootstrap/, dsp_coef.bin,
    // initial_pipeline_cache.db) unpacked from the APK by the activity. Stands
    // in for the executable directory the desktop builds look next to.
    if (const char* resources = std::getenv("MKW_ANDROID_RESOURCES_DIR"); resources && *resources) {
        return std::filesystem::path(resources);
    }
    return std::nullopt;
#elif defined(__linux__)
    std::vector<char> buffer(256);
    for (;;) {
        const auto length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            return std::nullopt;
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(std::string(buffer.data(), length)).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    return std::nullopt;
#endif
}

std::filesystem::path ApplicationDataDirectory(std::string_view applicationName) {
#if defined(_WIN32)
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / applicationName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / applicationName;
    }
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir && *user->pw_dir) {
        return std::filesystem::path(user->pw_dir) / "Library" / "Application Support" / applicationName;
    }
#elif defined(__ANDROID__)
    // The app's external files directory (Android/data/<package>/files) so a
    // user can push game data and Config.toml with adb or a file browser; the
    // activity falls back to the private files directory when there is none.
    if (const char* data = std::getenv("MKW_ANDROID_DATA_DIR"); data && *data) {
        return std::filesystem::path(data) / applicationName;
    }
#endif
    return std::filesystem::current_path() / applicationName;
}

std::filesystem::path LogDirectory(std::string_view applicationName) {
    return ApplicationDataDirectory(applicationName) / "Logs";
}

uint64_t CurrentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

} // namespace RuntimePlatform

#!/usr/bin/env bash
# Builds the pinned Dawn for Apple Vision Pro (or its simulator) as the install
# tree Aurora's package provider consumes (aurora-main/cmake/AuroraDawnProvider.cmake).
#
# No dawn-build release exists for visionOS and the ios-arm64 package is a
# Mach-O for the iOS platform, which the visionOS linker refuses, so Dawn is
# built from source here: the same revision as every other platform's package
# (and android/Build-QuestDawn.ps1), Metal only, monolithic static library.
# The Aurora Dawn patches are Vulkan-only and are not applied.
#
#   visionos/Build-VisionOSDawn.sh [--simulator] [--work DIR] [--jobs N] [--force]
#
# Prints the package path last. Cached: a package built from the same revision
# and flags is reused. Needs Xcode with the visionOS SDK, CMake 3.28+, Ninja,
# Python 3 and git (DAWN_FETCH_DEPENDENCIES clones Dawn's third_party).
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
dawn_commit="13abc3bc8ea2d3c2050f9e77a12d012108ceee24"
dawn_source_sha256="713bea5b92d4f6c5175752fd7cbf1c3c5ce36598ff5dd98685d8a1216614ebba"
work_dir="${repo_root}/.scratch/visionos-dawn"
jobs="$(sysctl -n hw.ncpu)"
sysroot="xros"
flavour="visionos-arm64"
force=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --simulator) sysroot="xrsimulator"; flavour="xrsimulator-arm64"; shift ;;
        --work) work_dir="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --force) force=1; shift ;;
        -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Darwin" || "$(uname -m)" != "arm64" ]]; then
    echo "ERROR: Dawn for visionOS is built on an Apple Silicon Mac" >&2
    exit 1
fi
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    # xcrun follows xcode-select, which may point at the Command Line Tools; those have no visionOS SDK.
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
sdk_path="$(xcrun --sdk "${sysroot}" --show-sdk-path 2>/dev/null || true)"
if [[ -z "${sdk_path}" ]]; then
    echo "ERROR: no ${sysroot} SDK; install Xcode with the visionOS platform" >&2
    exit 1
fi
for tool in cmake ninja python3 git; do
    command -v "${tool}" >/dev/null || { echo "ERROR: ${tool} not found" >&2; exit 1; }
done

flags=(
    -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_SYSTEM_NAME=visionOS
    "-DCMAKE_OSX_SYSROOT=${sysroot}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0
    -DCMAKE_SYSTEM_PROCESSOR=arm64
    -DDAWN_FETCH_DEPENDENCIES=ON
    -DDAWN_BUILD_MONOLITHIC_LIBRARY=STATIC
    -DBUILD_SHARED_LIBS=OFF
    -DDAWN_ENABLE_INSTALL=ON
    -DDAWN_BUILD_SAMPLES=OFF
    -DDAWN_BUILD_TESTS=OFF
    -DDAWN_BUILD_BENCHMARKS=OFF
    -DDAWN_USE_GLFW=OFF
    -DDAWN_SUPPORTS_GLFW_FOR_WINDOWING=OFF
    -DDAWN_ENABLE_METAL=ON
    -DDAWN_ENABLE_VULKAN=OFF
    -DDAWN_ENABLE_NULL=ON
    -DDAWN_ENABLE_DESKTOP_GL=OFF
    -DDAWN_ENABLE_OPENGLES=OFF
    -DTINT_BUILD_TESTS=OFF
    -DTINT_BUILD_CMD_TOOLS=OFF
    -DTINT_BUILD_IR_BINARY=OFF
    -DTINT_BUILD_MSL_WRITER=ON
    -DTINT_BUILD_HLSL_WRITER=OFF
    -DTINT_BUILD_GLSL_WRITER=OFF
    -DTINT_BUILD_SPV_WRITER=OFF
    -DTINT_BUILD_SPV_READER=OFF
    # Only the IR binary format needs protobuf, and building it would run a protoc
    # cross-compiled for the headset.
    -DDAWN_BUILD_PROTOBUF=OFF
)

mkdir -p "${work_dir}"
package_dir="${work_dir}/package-${flavour}"
package_path="${work_dir}/dawn-${flavour}.tar.gz"
manifest="${package_dir}/aurora-dawn.json"
cache_key="$(printf '%s|%s|%s\n' "${dawn_commit}" "${sdk_path##*/}" "${flags[*]}" | shasum -a 256 | awk '{print $1}')"

if [[ ${force} -eq 0 && -f "${manifest}" && -f "${package_path}" ]] &&
   grep -q "\"CacheKey\": \"${cache_key}\"" "${manifest}"; then
    echo "Dawn for ${flavour} is up to date: ${package_path}" >&2
    echo "${package_path}"
    exit 0
fi

source_archive="${work_dir}/dawn-${dawn_commit}.tar.gz"
if [[ ! -f "${source_archive}" ]]; then
    curl --fail --location --silent --show-error \
        "https://github.com/google/dawn/archive/${dawn_commit}.tar.gz" -o "${source_archive}"
fi
actual_sha256="$(shasum -a 256 "${source_archive}" | awk '{print $1}')"
if [[ "${actual_sha256}" != "${dawn_source_sha256}" ]]; then
    echo "ERROR: Dawn source archive hash mismatch: ${actual_sha256}" >&2
    exit 1
fi

source_dir="${work_dir}/dawn-${dawn_commit}"
if [[ ! -f "${source_dir}/CMakeLists.txt" ]]; then
    rm -rf "${source_dir}"
    mkdir -p "${source_dir}"
    tar -xzf "${source_archive}" --strip-components=1 -C "${source_dir}"
fi

build_dir="${work_dir}/build-${flavour}"
cmake -S "${source_dir}" -B "${build_dir}" -G Ninja "-DPython3_EXECUTABLE=$(command -v python3)" \
    "${flags[@]}" "-DCMAKE_INSTALL_PREFIX=${package_dir}"
cmake --build "${build_dir}" --parallel "${jobs}"
rm -rf "${package_dir}"
cmake --install "${build_dir}"

library="${package_dir}/lib/libwebgpu_dawn.a"
if [[ ! -f "${library}" || ! -f "${package_dir}/lib/cmake/Dawn/DawnConfig.cmake" ]]; then
    echo "ERROR: the Dawn install tree is incomplete: ${package_dir}" >&2
    exit 1
fi
# Debug info would multiply the archive the app links; the stock packages are stripped the same way.
xcrun strip -S "${library}"
ZERO_AR_DATE=1 xcrun ranlib "${library}"

cat > "${manifest}" <<EOF
{
  "SourceRevision": "${dawn_commit}",
  "SourceSha256": "${dawn_source_sha256}",
  "Sdk": "${sdk_path##*/}",
  "Flags": "${flags[*]}",
  "CacheKey": "${cache_key}",
  "ArchiveSha256": "$(shasum -a 256 "${library}" | awk '{print $1}')"
}
EOF

# A flat archive of the install tree, normalised so the same inputs give the same digest.
find "${package_dir}" -exec touch -h -t 198001010000 {} +
(
    cd "${package_dir}"
    find . -print | LC_ALL=C sort |
        COPYFILE_DISABLE=1 tar -cf - --no-recursion --uid 0 --gid 0 \
            --uname root --gname root --format=ustar -T -
) | gzip -n -9 > "${package_path}"
echo "Dawn for ${flavour}: ${package_path}" >&2
shasum -a 256 "${package_path}" >&2
echo "${package_path}"

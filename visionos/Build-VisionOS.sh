#!/usr/bin/env bash
# Builds WiiCompiled Vision, the Apple Vision Pro app, from an existing translation.
#
#   visionos/Build-VisionOS.sh [--team TEAMID] [--bundle-id ID]
#                              [--retro-rewind-dir DIR | --without-retro-rewind]
#                              [--simulator] [--build-dir DIR] [--dawn-package FILE]
#                              [--jobs N] [--install [--device UDID]] [--open]
#
# Steps: Dawn for the visionOS SDK (Build-VisionOSDawn.sh, cached), the Xcode
# project (visionos/CMakeLists.txt, which pulls the runtime in), the build, and
# with --install the app onto the paired headset (xcrun devicectl). --open
# opens the generated project in Xcode instead of building, for signing setup or
# debugging.
#
# The translation must exist first, exactly as for a desktop build:
# docs/building-macos.md steps 1 to 5 leave generated/build_shards/shards.cmake
# behind. `generate-data-init --target-os macos` is the right flavour, but the
# runtime's CMake rewrites the Windows/Linux blob assembly for Mach-O too.
#
# Retro Rewind rides along whenever the translation includes it (step C of that
# guide, run against a RetroRewind6 folder): the app then embeds both games and
# the launcher offers the choice. Pass that folder with --retro-rewind-dir so the
# app knows which pack version it was made from and installs that one on the
# headset; --without-retro-rewind leaves the mod out of the app.
#
# Signing: a free Apple ID's personal team can sign for a headset paired with
# this Mac (Xcode > Settings > Accounts). Pass its id with --team, or leave it
# out and pick the team once in the generated project; CMake remembers the value.
# The bundle identifier is made from the team, org.wiicompiled.vision.<team id>
# (org.wiicompiled.vision without --team, which only the project's own team can
# sign). --bundle-id chooses another; CMake remembers it too.
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
retro_rewind="AUTO"
retro_rewind_dir="${MKW_VISIONOS_RETRO_REWIND_DIR:-}"
team="${MKW_VISIONOS_TEAM:-}"
bundle_id="${MKW_VISIONOS_BUNDLE_ID:-}"
simulator=0
build_dir="${repo_root}/build-visionos"
dawn_package=""
jobs="$(sysctl -n hw.ncpu)"
install=0
device="${MKW_VISIONOS_DEVICE:-}"
open_project=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --retro-rewind-dir) retro_rewind_dir="${2:?}"; retro_rewind="ON"; shift 2 ;;
        --without-retro-rewind) retro_rewind="OFF"; shift ;;
        --team) team="${2:?}"; shift 2 ;;
        --bundle-id) bundle_id="${2:?}"; shift 2 ;;
        --simulator) simulator=1; shift ;;
        --build-dir) build_dir="${2:?}"; shift 2 ;;
        --dawn-package) dawn_package="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --install) install=1; shift ;;
        --device) device="${2:?}"; shift 2 ;;
        --open) open_project=1; shift ;;
        -h|--help) sed -n '2,31p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done
if [[ -n "${retro_rewind_dir}" ]]; then
    # The pack itself or the folder holding it, as local-build-macos.command accepts.
    if [[ ! -f "${retro_rewind_dir}/Binaries/Code.pul" && -f "${retro_rewind_dir}/RetroRewind6/Binaries/Code.pul" ]]; then
        retro_rewind_dir="${retro_rewind_dir}/RetroRewind6"
    fi
    if [[ ! -f "${retro_rewind_dir}/Binaries/Code.pul" ]]; then
        echo "ERROR: ${retro_rewind_dir} is not a RetroRewind6 folder (no Binaries/Code.pul)" >&2
        exit 1
    fi
    retro_rewind_dir="$(CDPATH= cd -- "${retro_rewind_dir}" && pwd)"
fi

if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
sysroot="xros"
if [[ ${simulator} -eq 1 ]]; then sysroot="xrsimulator"; fi

shards="${repo_root}/generated/build_shards/shards.cmake"
if [[ ! -f "${shards}" ]]; then
    echo "ERROR: no translation at ${shards}; run the translator first (docs/building-macos.md, steps 1 to 5)" >&2
    exit 1
fi

if [[ -z "${dawn_package}" ]]; then
    # (bash 3.2 counts an empty array as unbound under set -u, hence the string.)
    dawn_flag=""
    if [[ ${simulator} -eq 1 ]]; then dawn_flag="--simulator"; fi
    dawn_package="$("${repo_root}/visionos/Build-VisionOSDawn.sh" ${dawn_flag} --jobs "${jobs}" | tail -n 1)"
fi
if [[ ! -f "${dawn_package}" ]]; then
    echo "ERROR: Dawn package not found: ${dawn_package}" >&2
    exit 1
fi
case "${dawn_package}" in /*) ;; *) dawn_package="${PWD}/${dawn_package}" ;; esac

cmake_args=(
    -S "${repo_root}/visionos" -B "${build_dir}" -G Xcode
    -DCMAKE_SYSTEM_NAME=visionOS
    "-DCMAKE_OSX_SYSROOT=${sysroot}"
    -DCMAKE_OSX_ARCHITECTURES=arm64
    -DCMAKE_OSX_DEPLOYMENT_TARGET=2.0
    "-DAURORA_DAWN_PACKAGE_URL=file://${dawn_package}"
    "-DMKW_VISIONOS_RETRO_REWIND=${retro_rewind}"
    "-DMKW_VISIONOS_RETRO_REWIND_ROOT=${retro_rewind_dir}"
    -DAURORA_SDL3_PROVIDER=vendor
)
if [[ -n "${team}" ]]; then cmake_args+=("-DMKW_VISIONOS_TEAM=${team}"); fi
if [[ -n "${bundle_id}" ]]; then cmake_args+=("-DMKW_VISIONOS_BUNDLE_ID=${bundle_id}"); fi
cmake "${cmake_args[@]}"

if [[ ${open_project} -eq 1 ]]; then
    open "${build_dir}/WiiCompiledVision.xcodeproj"
    exit 0
fi

# -allowProvisioningUpdates lets automatic signing register the headset and refresh the profile.
cmake --build "${build_dir}" --config Release --target WiiCompiledVision --parallel "${jobs}" -- \
    -allowProvisioningUpdates
app="${build_dir}/Release-${sysroot}/WiiCompiledVision.app"
if [[ ! -d "${app}" ]]; then
    app="$(find "${build_dir}" -maxdepth 2 -name WiiCompiledVision.app -type d | head -n 1)"
fi
echo "App: ${app}"

if [[ ${install} -eq 1 ]]; then
    if [[ ${simulator} -eq 1 ]]; then
        xcrun simctl install booted "${app}"
        echo "Installed on the booted visionOS simulator"
    else
        # The first paired physical Apple Vision Pro (the list holds simulators too);
        # pair in Xcode > Devices and Simulators.
        if [[ -z "${device}" ]]; then
            device="$(xcrun devicectl list devices --hide-headers 2>/dev/null \
                | awk 'tolower($0) ~ /vision/ && $0 ~ /physical/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-Fa-f-]{25,}$/) { print $i; exit } }')"
        fi
        if [[ -z "${device}" ]]; then
            echo "ERROR: no paired Apple Vision Pro; pair it in Xcode first (or pass --device UDID)" >&2
            exit 1
        fi
        # A headset that is off the head is locked, and devicectl cannot reach it (error 4000,
        # "connection reset by peer"); one dozing off drops the connection too. Keep retrying
        # for about two minutes, long enough to put it on, and keep devicectl's wall of errors
        # out of the way unless the last attempt fails too.
        install_log="$(mktemp -t wiicompiled-install)"
        attempts=24
        attempt=1
        until xcrun devicectl device install app --device "${device}" "${app}" > "${install_log}" 2>&1; do
            if [[ ${attempt} -ge ${attempts} ]]; then
                cat "${install_log}" >&2
                rm -f "${install_log}"
                echo "ERROR: could not install on the Apple Vision Pro (${device})." >&2
                echo "       Put the headset on, unlock it, check it is still paired (Xcode > Window > Devices and Simulators), and run again." >&2
                exit 1
            fi
            if [[ ${attempt} -eq 1 ]]; then
                echo "" >&2
                echo ">>> The Apple Vision Pro cannot be reached. Please put your headset on and unlock it;" >&2
                echo ">>> the installer retries every 5 seconds (for about 2 minutes)." >&2
            else
                echo "    Still waiting for the headset (attempt ${attempt} of ${attempts})..." >&2
            fi
            attempt=$((attempt + 1)); sleep 5
        done
        cat "${install_log}"
        rm -f "${install_log}"
        echo "Installed on ${device}"
    fi
fi

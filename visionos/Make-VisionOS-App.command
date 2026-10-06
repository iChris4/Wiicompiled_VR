#!/usr/bin/env bash
# WiiCompiled for Apple Vision Pro, from your own Mario Kart Wii disc, in one go.
#
#   visionos/Make-VisionOS-App.command [--game IMAGE] [--retro-rewind download|DIR]
#                                      [--team TEAMID] [--bundle-id ID] [--device UDID]
#                                      [--jobs N] [--no-install] [--no-disc-copy]
#                                      [--reinstall] [--retranslate] [--offline]
#
# Double-clicked in Finder (no arguments) it asks for the disc image and whether to
# include Retro Rewind, then runs every step below in a Terminal window. The
# tutorial is docs/visionos-getting-started.md.
#
# The app's bundle identifier is made from the signing team's id,
# org.wiicompiled.vision.<team id>: org.wiicompiled.vision itself is registered to the
# project's team, and only that team can sign with it. --bundle-id chooses another, such
# as com.yourname.wiicompiled; the build remembers it, so later runs (double-clicked ones
# included) keep it without the option.
#
# Steps, each skipped when its result is already there:
#   1. Check this Mac: Apple Silicon, Xcode with the visionOS platform, Homebrew's
#      cmake/ninja/dotnet, an Apple ID team in Xcode, a paired headset.
#   2. Extract and verify the disc (Launcher/macos/extract-disc.command, nodtool).
#   3. Retro Rewind: fetch the pack (its own server) and the Retro-WFC payload.
#   4. Translate the game (Launcher/local-build-macos.command --translate-only).
#   5. Build, sign and install the app (visionos/Build-VisionOS.sh --install).
#   6. Copy the extracted disc into the app on the headset (devicectl).
#   7. Launch it.
#
# Nothing of the game is ever downloaded or shipped: the disc is yours, the
# translation and the app are made on this Mac, and only Retro Rewind's own pack
# comes from Retro Rewind's server. Everything lands under the repository:
# Assets/ (the extracted disc), generated/ (the translation), build-visionos/
# (the app), .scratch/ (tools, Dawn, the Retro Rewind pack).
set -euo pipefail

repo_root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "${repo_root}"

game=""
retro_rewind=""            # "" (none), "download", or a RetroRewind6 folder
team="${MKW_VISIONOS_TEAM:-}"
bundle_id="${MKW_VISIONOS_BUNDLE_ID:-}"
device="${MKW_VISIONOS_DEVICE:-}"
jobs="$(sysctl -n hw.ncpu)"
install=1
copy_disc=1
reinstall=0
retranslate=0
offline=0
interactive=0
[[ $# -eq 0 ]] && interactive=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --game) game="${2:?}"; shift 2 ;;
        --retro-rewind) retro_rewind="${2:?}"; shift 2 ;;
        --team) team="${2:?}"; shift 2 ;;
        --bundle-id) bundle_id="${2:?}"; shift 2 ;;
        --device) device="${2:?}"; shift 2 ;;
        --jobs) jobs="${2:?}"; shift 2 ;;
        --no-install) install=0; shift ;;
        --no-disc-copy) copy_disc=0; shift ;;
        --reinstall) reinstall=1; shift ;;
        --retranslate) retranslate=1; shift ;;
        --offline) offline=1; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

bold=$'\033[1m'; dim=$'\033[2m'; red=$'\033[31m'; green=$'\033[32m'; reset=$'\033[0m'
[[ -t 1 ]] || { bold=""; dim=""; red=""; green=""; reset=""; }
step() { printf '\n%s==> %s%s\n' "${bold}" "$*" "${reset}"; }
note() { printf '%s    %s%s\n' "${dim}" "$*" "${reset}"; }
done_msg() { printf '%s    %s%s\n' "${green}" "$*" "${reset}"; }
fail() {
    printf '\n%sSTOPPED:%s %s\n' "${red}" "${reset}" "$1" >&2
    shift
    for line in "$@"; do printf '  %s\n' "${line}" >&2; done
    if [[ ${interactive} -eq 1 ]]; then printf '\nPress Return to close this window.'; read -r; fi
    exit 1
}
with_timeout() {
    # with_timeout SECONDS command...: devicectl waits forever on a locked headset.
    local seconds="$1" pid i; shift
    "$@" & pid=$!
    for ((i = 0; i < seconds; i++)); do kill -0 "${pid}" 2>/dev/null || break; sleep 1; done
    if kill -0 "${pid}" 2>/dev/null; then kill "${pid}" 2>/dev/null; wait "${pid}" 2>/dev/null; return 124; fi
    wait "${pid}"
}
ask_disc() {
    # Asks for the disc image in the Terminal: dragging a file onto the window types its path,
    # with spaces and other specials backslash-escaped (or the whole path quoted) and a
    # trailing space. Prints the plain path; returns 1 when the user just presses Return.
    local raw path
    while true; do
        printf '\n    %sDrag your Mario Kart Wii disc image (.iso, .rvz, .wbfs, .ciso) onto this window,%s\n' "${bold}" "${reset}" >&2
        printf '    then press Return (or just Return to stop): ' >&2
        IFS= read -r raw || return 1
        # Trim surrounding blanks, then one pair of surrounding quotes, then the backslash escapes.
        raw="${raw#"${raw%%[![:space:]]*}"}"; raw="${raw%"${raw##*[![:space:]]}"}"
        [[ -n "${raw}" ]] || return 1
        if [[ "${raw}" == \'*\' || "${raw}" == \"*\" ]]; then path="${raw:1:${#raw}-2}"
        else path="$(printf '%s' "${raw}" | sed -E 's/\\(.)/\1/g')"; fi
        [[ "${path}" == "~/"* ]] && path="${HOME}/${path#"~/"}"
        if [[ -f "${path}" ]]; then printf '%s' "${path}"; return 0; fi
        printf '    %sNot a file: %s%s\n' "${red}" "${path}" "${reset}" >&2
    done
}
# The teams of the Apple IDs signed in to Xcode, one "TEAMID<tab>name (type)" per line.
# Current Xcode keeps them under IDEProvisioningTeamByIdentifier and older ones under
# IDEProvisioningTeams; an older key can linger beside the newer one, out of date. A key that
# is not there fails defaults, which under pipefail would end the script without a word.
xcode_teams() {
    { defaults read com.apple.dt.Xcode IDEProvisioningTeamByIdentifier 2>/dev/null || true
      defaults read com.apple.dt.Xcode IDEProvisioningTeams 2>/dev/null || true; } \
        | awk '
            function value(line) { sub(/^[^=]*= */, "", line); sub(/;[[:space:]]*$/, "", line); gsub(/"/, "", line); return line }
            /teamID = /   { id = value($0) }
            /teamName = / { name = value($0) }
            /teamType = / { type = value($0) }
            /^[[:space:]]*}/ {
                # A personal team is already named "... (Personal Team)".
                if (type != "" && index(name, "(" type ")") == 0) name = name " (" type ")"
                if (length(id) == 10 && !(id in seen)) { seen[id] = 1; print id "\t" name }
                id = ""; name = ""; type = "" }'
}
ask_team() {
    # Asks for the signing team in the Terminal when this Mac does not name exactly one ("$1":
    # the teams found, one per line): a number from the list, or a pasted Team ID. Prints the
    # id; returns 1 when the user just presses Return.
    local teams count raw id prompt i=0
    teams="$(printf '%s\n' "$1" | sed '/^$/d')"
    count="$(printf '%s\n' "${teams}" | sed '/^$/d' | wc -l | tr -d ' ')"
    if [[ "${count}" -gt 0 ]]; then
        printf '\n    %sSeveral teams can sign the app:%s\n' "${bold}" "${reset}" >&2
        while IFS= read -r id; do
            i=$((i + 1))
            printf '      %d) %s  %s\n' "${i}" "${id}" "$(xcode_teams | awk -F '\t' -v id="${id}" '$1 == id { print $2; exit }')" >&2
        done <<< "${teams}"
        prompt="Type the number of the team to sign with, or paste its Team ID"
    else
        printf '\n    %sNo Apple developer team found on this Mac.%s\n' "${bold}" "${reset}" >&2
        printf '    Sign in to Xcode (Xcode > Settings > Accounts, +, your Apple ID; a free account is\n' >&2
        printf '    fine) and run this script again. Signed in already? Paste your Team ID: the\n' >&2
        printf '    Organizational Unit of your Apple Development certificate in Keychain Access, or\n' >&2
        printf '    Membership details on developer.apple.com for a paid account.\n' >&2
        prompt="Team ID"
    fi
    while true; do
        printf '    %s (or just Return to stop): ' "${prompt}" >&2
        IFS= read -r raw || return 1
        raw="$(printf '%s' "${raw}" | tr -d '[:space:]' | tr '[:lower:]' '[:upper:]')"
        [[ -n "${raw}" ]] || return 1
        if [[ "${raw}" =~ ^[0-9]{1,3}$ ]] && (( 10#${raw} >= 1 && 10#${raw} <= count )); then
            printf '%s\n' "${teams}" | sed -n "$((10#${raw}))p"; return 0
        fi
        if [[ "${raw}" =~ ^[A-Z0-9]{10}$ ]]; then printf '%s' "${raw}"; return 0; fi
        printf '    %sNot a number from the list or a 10-character Team ID: %s%s\n' "${red}" "${raw}" "${reset}" >&2
    done
}
# Copies a folder into the app's container on the headset with a progress bar. devicectl
# reports nothing while it copies, so the folder goes over in batches (one folder's files,
# at most 128 MB or 400 files each, a single larger file alone) and the bar moves by each
# batch's bytes. devicectl skips files already there unchanged, so a rerun resumes.
copy_disc_with_progress() {
    local source="$1" destination="$2"
    local plan total done_bytes=0 started batch_bytes=0 batch_dir="" batch_id="" id size path dir attempt
    local sources=() tab=$'\t'
    plan="$(mktemp -t wiicompiled-copy)"
    # "<batch>\t<bytes>\t<relative path>", grouped by folder in path order.
    ( cd "${source}" && find -L . -type f -print0 | xargs -0 stat -L -f '%z%t%N' ) \
        | sed "s#${tab}\./#${tab}#" | sort -t "${tab}" -k2 \
        | awk -F '\t' -v OFS='\t' '
            { dir = $2; sub(/\/[^\/]*$/, "", dir); if (dir == $2) dir = "" }
            NR == 1 || dir != last || bytes + $1 > 134217728 || count >= 400 { batch++; bytes = 0; count = 0; last = dir }
            { bytes += $1; count++; print batch, $1, $2 }' > "${plan}"
    total="$(awk -F '\t' '{ t += $2 } END { printf "%d", t }' "${plan}")"
    started="$(date +%s)"
    draw_bar() {
        local width=36 filled percent elapsed rate eta
        # (bash evaluates both arms of ?: , so a zero divisor needs an if.)
        percent=100; if (( total > 0 )); then percent=$(( done_bytes * 100 / total )); fi
        filled=$(( width * percent / 100 ))
        elapsed=$(( $(date +%s) - started ))
        rate=0; if (( elapsed > 0 )); then rate=$(( done_bytes / elapsed )); fi
        # The first batches are the disc's many tiny files: no estimate until it means something.
        eta="estimating..."
        if (( done_bytes >= total )); then
            eta="$(printf 'done in %d:%02d' $(( elapsed / 60 )) $(( elapsed % 60 )))"
        elif (( rate > 0 && (percent >= 5 || elapsed >= 10) )); then
            eta="$(printf '%d:%02d left' $(( (total - done_bytes) / rate / 60 )) $(( (total - done_bytes) / rate % 60 )))"
        fi
        local bar; bar="$(printf '%*s' "${filled}" '' | tr ' ' '#')$(printf '%*s' $(( width - filled )) '' | tr ' ' '.')"
        printf '\r    [%s] %3d%%  %s of %s  %s/s  %-16s' "${bar}" "${percent}" \
            "$(human_bytes "${done_bytes}")" "$(human_bytes "${total}")" "$(human_bytes "${rate}")" "${eta}"
        [[ -t 1 ]] || printf '\n'
    }
    flush_batch() {
        [[ ${#sources[@]} -gt 0 ]] || return 0
        # Several sources go into the destination folder; a single one is written AS the
        # destination (a lone cert.bin sent to "DATA" became a file named DATA, wiping the
        # folder), so a one-file batch names the file's full path.
        local target="${destination}${batch_dir:+/${batch_dir}}"
        if [[ ${#sources[@]} -eq 2 ]]; then target="${target}/${sources[1]##*/}"; fi
        attempt=1
        until xcrun devicectl device copy to --device "${device}" "${sources[@]}" \
                --destination "${target}" --domain-type appDataContainer \
                --domain-identifier "${bundle}" < /dev/null > /dev/null 2>&1; do
            [[ ${attempt} -lt 24 ]] || { printf '\n'; rm -f "${plan}"; fail "The disc could not be copied to the headset." \
                "Put the headset on, unlock it and run the script again (it resumes where it stopped)," \
                "or copy Assets/DATA yourself: Finder > the headset > Files > WiiCompiled Vision > WiiCompiled > DATA."; }
            printf '\n'
            if [[ ${attempt} -eq 1 ]]; then
                note "The Apple Vision Pro cannot be reached. Please put your headset on and unlock it;"
                note "the copy retries every 5 seconds (for about 2 minutes)."
            else
                note "Still waiting for the headset (attempt ${attempt} of 24)..."
            fi
            attempt=$((attempt + 1)); sleep 5
        done
        done_bytes=$(( done_bytes + batch_bytes ))
        draw_bar
        sources=(); batch_bytes=0
    }
    draw_bar
    while IFS="${tab}" read -r id size path; do
        if [[ "${id}" != "${batch_id}" ]]; then
            flush_batch
            batch_id="${id}"
            dir="${path%/*}"; [[ "${dir}" == "${path}" ]] && dir=""
            batch_dir="${dir}"
        fi
        sources+=(--source "${source}/${path}")
        batch_bytes=$(( batch_bytes + size ))
    done < "${plan}"
    flush_batch
    printf '\n'
    rm -f "${plan}"
}
human_bytes() {
    awk -v b="$1" 'BEGIN { if (b >= 1e9) printf "%.2f GB", b / 1e9; else if (b >= 1e6) printf "%.0f MB", b / 1e6; else printf "%.0f KB", b / 1e3 }'
}
ask_yes() {
    # "$1" question; yes when not on a terminal would be presumptuous, so only asks when it can.
    [[ -t 0 ]] || return 1
    read -r -p "$1 [y/N] " answer
    [[ "${answer}" == y || "${answer}" == Y ]]
}

# ---------------------------------------------------------------------------
# Interactive choices (double-clicked from Finder)
# ---------------------------------------------------------------------------
if [[ ${interactive} -eq 1 && ${reinstall} -eq 0 ]]; then
    if [[ ! -f Assets/main.dol ]]; then
        game="$(/usr/bin/osascript <<'APPLESCRIPT' 2>/dev/null
set selectedFile to choose file with prompt "Choose your own Mario Kart Wii PAL (RMCP01) disc image (.iso, .rvz, .wbfs, .ciso)"
POSIX path of selectedFile
APPLESCRIPT
)" || exit 0
    fi
    choice="$(/usr/bin/osascript -e 'button returned of (display dialog "Include Retro Rewind (the community expansion; the pack is downloaded from its own server)?" buttons {"Mario Kart Wii only", "Include Retro Rewind"} default button "Include Retro Rewind")' 2>/dev/null)" || exit 0
    [[ "${choice}" == "Include Retro Rewind" ]] && retro_rewind="download"
fi

# ---------------------------------------------------------------------------
# 1. This Mac
# ---------------------------------------------------------------------------
step "Step 1/7  Checking this Mac"
[[ "$(uname -s)" == Darwin ]] || fail "This script is for macOS."
[[ "$(uname -m)" == arm64 ]] || fail "An Apple Silicon Mac (M1 or later) is required; the visionOS toolchain is arm64 only."

# Xcode proper, not just the Command Line Tools: the visionOS SDK and signing live in it.
xcode_dir="${DEVELOPER_DIR:-}"
if [[ -z "${xcode_dir}" || ! -d "${xcode_dir}/Platforms/XROS.platform" ]]; then
    selected="$(xcode-select -p 2>/dev/null || true)"
    if [[ -d "${selected}/Platforms" ]]; then xcode_dir="${selected}"; fi
fi
if [[ -z "${xcode_dir}" || ! -d "${xcode_dir}/Platforms" ]]; then
    for candidate in /Applications/Xcode.app /Applications/Xcode-beta.app; do
        if [[ -d "${candidate}/Contents/Developer/Platforms" ]]; then xcode_dir="${candidate}/Contents/Developer"; break; fi
    done
fi
[[ -n "${xcode_dir}" && -d "${xcode_dir}/Platforms" ]] || fail "Xcode is not installed." \
    "Install Xcode from the Mac App Store (https://apps.apple.com/app/xcode/id497799835), open it once," \
    "and run this script again. The Command Line Tools alone do not carry the visionOS SDK."
export DEVELOPER_DIR="${xcode_dir}"
note "Xcode: ${DEVELOPER_DIR} ($(xcodebuild -version 2>/dev/null | head -n 1 || echo 'version unknown'))"
if ! xcodebuild -checkFirstLaunchStatus >/dev/null 2>&1; then
    fail "Xcode has not finished its first-launch setup (licence and components)." \
        "Open Xcode once and accept the licence, or run:  sudo xcodebuild -runFirstLaunch"
fi
if ! xcodebuild -showsdks 2>/dev/null | grep -q -- '-sdk xros'; then
    note "The visionOS platform is not installed in Xcode (Xcode > Settings > Components, or: xcodebuild -downloadPlatform visionOS)."
    if ask_yes "Download it now (several GB)?"; then
        xcodebuild -downloadPlatform visionOS || fail "The visionOS platform download failed."
    else
        fail "The visionOS platform is required." "Install it in Xcode > Settings > Components (visionOS), then run this script again."
    fi
fi
note "visionOS SDK: $(xcodebuild -showsdks 2>/dev/null | awk '/-sdk xros/ {print $NF}' | head -n 1)"

# Homebrew tools for the translation.
missing=()
for tool in cmake ninja dotnet; do command -v "${tool}" >/dev/null 2>&1 || missing+=("${tool}"); done
if [[ ${#missing[@]} -gt 0 ]]; then
    hint="brew install cmake ninja"
    [[ " ${missing[*]} " == *" dotnet "* ]] && hint="${hint} && brew install --cask dotnet-sdk"
    fail "Missing tools: ${missing[*]}." \
        "Install Homebrew (https://brew.sh) if needed, then:  ${hint}" \
        "and run this script again."
fi
if ! dotnet --list-sdks 2>/dev/null | awk '{ split($1, v, "."); if (v[1] + 0 >= 8) found = 1 } END { exit found ? 0 : 1 }'; then
    fail ".NET SDK 8 or newer is required for the translator (found: $(dotnet --version 2>/dev/null || echo none))." \
        "brew install --cask dotnet-sdk"
fi
note "cmake $(cmake --version | head -n 1 | awk '{print $3}'), ninja $(ninja --version), .NET SDK $(dotnet --version)"

# nodtool extracts the disc (open source, no game data); fetched once.
nodtool="${repo_root}/.scratch/tools/nodtool"
if [[ ! -x "${nodtool}" ]]; then
    [[ ${offline} -eq 0 ]] || fail "nodtool is missing and --offline was given." "Put the macOS arm64 nodtool at ${nodtool}"
    note "Downloading nodtool (disc extraction tool)"
    mkdir -p "$(dirname "${nodtool}")"
    curl -fsSL --retry 3 "https://github.com/encounter/nod/releases/download/v2.0.0-alpha.10/nodtool-macos-arm64" -o "${nodtool}" \
        || fail "Could not download nodtool from GitHub."
    chmod +x "${nodtool}"
    xattr -d com.apple.quarantine "${nodtool}" 2>/dev/null || true
fi

# The signing team: an Apple ID added in Xcode > Settings > Accounts. A free account's
# personal team is enough for a headset paired with this Mac.
if [[ -z "${team}" ]]; then
    # The teams of the development certificates that are still valid (an expired personal
    # team's certificate lingers in the keychain): the certificate's OU is the team id.
    valid_hashes="$(security find-identity -v -p codesigning 2>/dev/null | awk '/Apple Development/ {print $2}')"
    teams="$(security find-certificate -a -c "Apple Development" -Z -p 2>/dev/null \
        | awk -v valid="${valid_hashes}" '
            BEGIN { n = split(valid, list, /[[:space:]]+/); for (i = 1; i <= n; i++) if (list[i] != "") ok[list[i]] = 1 }
            /^SHA-1 hash:/ { hash = $3; cert = ""; next }
            /BEGIN CERT/ { cert = "" }
            { cert = cert $0 "\n" }
            /END CERT/ && (hash in ok) { print cert | "openssl x509 -noout -subject 2>/dev/null"; close("openssl x509 -noout -subject 2>/dev/null") }' \
        | sed -n 's/.*OU *= *\([A-Z0-9]\{10\}\).*/\1/p' | sort -u)"
    if [[ -z "${teams}" ]]; then
        # No certificate yet: Xcode's account cache still knows the team, and the build
        # (-allowProvisioningUpdates) creates the certificate.
        teams="$(xcode_teams | cut -f 1 | sort -u)"
    fi
    count="$(printf '%s\n' "${teams}" | sed '/^$/d' | wc -l | tr -d ' ')"
    if [[ "${count}" == "1" ]]; then
        team="$(printf '%s\n' "${teams}" | sed '/^$/d')"
    elif [[ -t 0 ]] && team="$(ask_team "${teams}")"; then
        :
    elif [[ "${count}" == "0" ]]; then
        fail "No Apple developer team found on this Mac." \
            "Open Xcode > Settings > Accounts, press +, sign in with your Apple ID (a free account is fine)," \
            "then run this script again. Or pass --team TEAMID."
    else
        fail "Several teams are available: $(printf '%s ' ${teams})" \
            "Run again with --team TEAMID (the one to sign the app with)."
    fi
fi
note "Signing team: ${team}"

# The headset: paired with this Mac in Xcode > Window > Devices and Simulators.
if [[ ${install} -eq 1 ]]; then
    if [[ -z "${device}" ]]; then
        device="$(xcrun devicectl list devices --hide-headers 2>/dev/null \
            | awk 'tolower($0) ~ /vision/ && $0 ~ /physical/ { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9A-Fa-f-]{25,}$/) { print $i; exit } }')"
    fi
    [[ -n "${device}" ]] || fail "No paired Apple Vision Pro." \
        "On the headset: Settings > General > Remote Devices, then in Xcode: Window > Devices and Simulators," \
        "pair it and turn on Developer Mode when asked (Settings > Privacy & Security > Developer Mode)." \
        "Run this script again once it shows as paired; or pass --device UDID, or --no-install."
    note "Headset: ${device}"
fi

# ---------------------------------------------------------------------------
# 2. The disc
# ---------------------------------------------------------------------------
if [[ ${reinstall} -eq 0 ]]; then
    step "Step 2/7  Extracting the disc"
    if [[ -n "${game}" ]]; then
        [[ -f "${game}" ]] || fail "Disc image not found: ${game}"
        "${repo_root}/Launcher/macos/extract-disc.command" --game "${game}" --assets-dir "${repo_root}/Assets" --nodtool "${nodtool}" \
            || fail "The disc could not be extracted." "A clean PAL (RMCP01) Mario Kart Wii image is required (.iso, .rvz, .wbfs, .ciso, .gcm)."
        done_msg "Disc extracted to Assets/DATA"
    elif [[ -f Assets/main.dol && -f Assets/DATA/sys/fst.bin ]]; then
        done_msg "Already extracted (Assets/DATA); pass --game IMAGE to extract again"
    elif [[ -t 0 ]] && game="$(ask_disc)"; then
        "${repo_root}/Launcher/macos/extract-disc.command" --game "${game}" --assets-dir "${repo_root}/Assets" --nodtool "${nodtool}" \
            || fail "The disc could not be extracted." "A clean PAL (RMCP01) Mario Kart Wii image is required (.iso, .rvz, .wbfs, .ciso, .gcm)."
        done_msg "Disc extracted to Assets/DATA"
    else
        fail "No disc yet." "Run again and drag the disc image onto the Terminal window when asked," \
            "or pass --game /path/to/RMCP01.iso (or double-click the script to pick the file)."
    fi
fi

# ---------------------------------------------------------------------------
# 3. Retro Rewind
# ---------------------------------------------------------------------------
retro_dir=""
retro_wfc_dir="${repo_root}/build/retro-wfc"
if [[ ${reinstall} -eq 1 && -z "${retro_rewind}" && -f "${repo_root}/.scratch/retro-rewind/RetroRewind6/Binaries/Code.pul" \
      && -d "${repo_root}/generated/build_shards/retro_portable_sensitive" ]]; then
    retro_rewind="${repo_root}/.scratch/retro-rewind/RetroRewind6"   # keep what the last build had
fi
if [[ -n "${retro_rewind}" ]]; then
    step "Step 3/7  Retro Rewind"
    if [[ "${retro_rewind}" == "download" ]]; then
        retro_dir="${repo_root}/.scratch/retro-rewind/RetroRewind6"
        if [[ -f "${retro_dir}/Binaries/Code.pul" && -f "${retro_dir}/version.txt" && ${retranslate} -eq 0 ]]; then
            done_msg "Pack $(tr -d '[:space:]' < "${retro_dir}/version.txt") already here (.scratch/retro-rewind); pass --retranslate to refresh it"
        else
            [[ ${offline} -eq 0 ]] || fail "The Retro Rewind pack is not here and --offline was given."
            base_url="$(curl -fsSL --retry 3 https://update.rwfc.net/RetroRewind/RetroRewindInstall.txt | tr -d '[:space:]' \
                | sed 's#^http://update.rwfc.net:8000/#https://update.rwfc.net/#')" || fail "Retro Rewind's server could not be reached."
            [[ -n "${base_url}" ]] || fail "Retro Rewind's server did not say where its download is."
            zip="${repo_root}/.scratch/retro-rewind/$(basename "${base_url}")"
            mkdir -p "${repo_root}/.scratch/retro-rewind"
            if [[ -f "${zip}" ]] && unzip -tq "${zip}" >/dev/null 2>&1; then
                note "Pack zip already downloaded"
            else
                note "Downloading the pack from ${base_url} (about 1.9 GB)"
                rm -f "${zip}"
                curl -fL --retry 3 --progress-bar -o "${zip}" "${base_url}" || fail "The Retro Rewind download failed."
            fi
            note "Unpacking the RetroRewind6 folder"
            rm -rf "${retro_dir}"
            unzip -q -o "${zip}" 'RetroRewind6/*' -d "${repo_root}/.scratch/retro-rewind" || fail "The pack zip could not be unpacked."
            [[ -f "${retro_dir}/Binaries/Code.pul" ]] || fail "The download did not contain the pack."
            done_msg "Retro Rewind $(tr -d '[:space:]' < "${retro_dir}/version.txt") ready"
        fi
    else
        retro_dir="${retro_rewind}"
        [[ -f "${retro_dir}/Binaries/Code.pul" || -f "${retro_dir}/RetroRewind6/Binaries/Code.pul" ]] \
            || fail "${retro_dir} is not a RetroRewind6 folder (no Binaries/Code.pul)."
        [[ -f "${retro_dir}/Binaries/Code.pul" ]] || retro_dir="${retro_dir}/RetroRewind6"
        retro_dir="$(CDPATH= cd -- "${retro_dir}" && pwd)"
        done_msg "Using the pack at ${retro_dir}"
    fi
    # Online play needs Retro-WFC's shared payload; the translator checks its signature.
    payload="${retro_wfc_dir}/binary/payload.RMCPD00.bin"
    if [[ ! -f "${payload}" ]]; then
        if [[ ${offline} -eq 0 ]]; then
            mkdir -p "$(dirname "${payload}")"
            note "Downloading the Retro-WFC payload (online play)"
            curl -fsSL --retry 3 "https://rwfc.net/api/wfc/payload?g=RMCPD00" -o "${payload}" || { rm -f "${payload}"; note "Payload download failed; building without online play."; }
        else
            note "Offline: building Retro Rewind without online play."
        fi
    fi
fi

# ---------------------------------------------------------------------------
# 4. Translation
# ---------------------------------------------------------------------------
if [[ ${reinstall} -eq 0 ]]; then
    step "Step 4/7  Translating the game"
    profile="base"; [[ -n "${retro_dir}" ]] && profile="both"
    stamp="${repo_root}/generated/.visionos-translation"
    wanted="profile=${profile} disc=$(shasum -a 256 Assets/main.dol | cut -c1-16)"
    [[ -n "${retro_dir}" ]] && wanted="${wanted} codepul=$(shasum -a 256 "${retro_dir}/Binaries/Code.pul" | cut -c1-16)"
    if [[ ${retranslate} -eq 0 && -f "${stamp}" && "$(cat "${stamp}")" == "${wanted}" && -f generated/build_shards/shards.cmake ]]; then
        done_msg "Already translated for this disc${retro_dir:+ and pack} (generated/); pass --retranslate to redo it"
    else
        args=(--translate-only --profile "${profile}")
        if [[ -n "${retro_dir}" ]]; then
            args+=(--retro-rewind-package-dir "${retro_dir}")
            if [[ -f "${retro_wfc_dir}/binary/payload.RMCPD00.bin" ]]; then args+=(--retro-wfc-offline-dir "${retro_wfc_dir}"); else args+=(--skip-retro-wfc-payload); fi
        fi
        note "This takes a few minutes (the translator is built the first time)."
        rm -f "${stamp}"
        "${repo_root}/Launcher/local-build-macos.command" "${args[@]}" || fail "The translation failed; see the messages above."
        printf '%s' "${wanted}" > "${stamp}"
        done_msg "Translation ready (generated/build_shards)"
    fi
fi

# ---------------------------------------------------------------------------
# 5. The app
# ---------------------------------------------------------------------------
step "Step 5/7  Building and signing the app"
[[ -f generated/build_shards/shards.cmake ]] || fail "No translation found; run without --reinstall first."
build_args=(--team "${team}" --jobs "${jobs}")
if [[ -n "${bundle_id}" ]]; then build_args+=(--bundle-id "${bundle_id}"); fi
if [[ -n "${retro_dir}" ]]; then build_args+=(--retro-rewind-dir "${retro_dir}"); else build_args+=(--without-retro-rewind); fi
if [[ ${install} -eq 1 ]]; then build_args+=(--install --device "${device}"); fi
note "The first build compiles Dawn and the runtime as well: 30 to 60 minutes. Later builds take a minute or two."
"${repo_root}/visionos/Build-VisionOS.sh" "${build_args[@]}" || fail "The build or the installation failed; see the messages above." \
    "Signing problems: open Xcode > Settings > Accounts and check the Apple ID is signed in." \
    "If the bundle identifier is not available, run again with --bundle-id com.yourname.wiicompiled." \
    "Installation problems: unlock the headset, keep it awake, and check it is still paired in Xcode > Window > Devices and Simulators."
app="build-visionos/Release-xros/WiiCompiledVision.app"
if [[ ${install} -eq 1 ]]; then done_msg "App built and installed"; else done_msg "App built"; fi
[[ ${install} -eq 1 ]] || { printf '\nDone (not installed). App: %s\n' "${app}"; exit 0; }

# ---------------------------------------------------------------------------
# 6. The disc onto the headset
# ---------------------------------------------------------------------------
# The identifier the app was built with: CMake keeps an earlier run's --bundle-id, so a run
# without the option reads it back from the app rather than assuming the default.
bundle="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "${app}/Info.plist" 2>/dev/null)" \
    || fail "No bundle identifier in ${app}/Info.plist." "Run the script again; the build should have made the app there."
if [[ ${copy_disc} -eq 1 && ${reinstall} -eq 0 ]]; then
    step "Step 6/7  Copying the disc into the app on the headset"
    listing() {
        xcrun devicectl device info files --device "${device}" --domain-type appDataContainer --domain-identifier "${bundle}" \
            --subdirectory "$1" 2>/dev/null
    }
    # Reaching the headset at all first: a locked one answers nothing, which would read as
    # "no disc there" and start a pointless copy.
    attempt=1
    until xcrun devicectl device info files --device "${device}" --domain-type appDataContainer \
            --domain-identifier "${bundle}" --subdirectory Documents >/dev/null 2>&1; do
        [[ ${attempt} -lt 24 ]] || fail "The Apple Vision Pro cannot be reached." \
            "Put the headset on, unlock it and run the script again (finished steps are skipped)."
        if [[ ${attempt} -eq 1 ]]; then
            note "The Apple Vision Pro cannot be reached. Please put your headset on and unlock it;"
            note "the installer retries every 5 seconds (for about 2 minutes)."
        else
            note "Still waiting for the headset (attempt ${attempt} of 24)..."
        fi
        attempt=$((attempt + 1)); sleep 5
    done
    local_files="$(find -L "${repo_root}/Assets/DATA" -type f | wc -l | tr -d ' ')"
    remote_files() { listing Documents/WiiCompiled/DATA | grep 'Readable' | grep -vc 'Directory' || true; }
    if [[ "$(remote_files)" -ge "${local_files}" ]] && listing Documents/WiiCompiled/DATA/sys | grep -q 'fst.bin'; then
        done_msg "The headset already has the disc (Documents/WiiCompiled/DATA)"
    else
        note "About $(du -shL Assets/DATA | cut -f1) over the cable (a few minutes; much longer over Wi-Fi)"
        copy_disc_with_progress "${repo_root}/Assets/DATA" Documents/WiiCompiled/DATA
        arrived="$(remote_files)"
        [[ "${arrived}" -ge "${local_files}" ]] && listing Documents/WiiCompiled/DATA/sys | grep -q 'fst.bin' \
            || fail "The disc did not arrive complete on the headset (${arrived} of ${local_files} files); run the script again."
        done_msg "Disc copied"
    fi
else
    step "Step 6/7  Copying the disc (skipped)"
fi

# ---------------------------------------------------------------------------
# 7. Launch
# ---------------------------------------------------------------------------
step "Step 7/7  Launching WiiCompiled Vision on the headset"
if with_timeout 30 xcrun devicectl device process launch --terminate-existing --device "${device}" "${bundle}" >/dev/null 2>&1; then
    done_msg "Launched"
else
    note "Could not launch it from here (is the headset unlocked?); open WiiCompiled Vision from the Home View."
fi

cat <<EOF

${bold}All set.${reset} Put the headset on.

  - First launch with a free Apple ID: visionOS refuses "untrusted developer" apps until you
    trust yours once, in Settings > General > VPN & Device Management.
  - A free account's app stops opening after 7 days (a paid developer account: one year).
    Just run this script again with --reinstall: it re-signs and reinstalls in a minute or two.
  - Retro Rewind: pick it in the app's Play tab and press Download Retro Rewind; the pack
    comes from Retro Rewind's server straight to the headset (about 2 GB).
  - Race with a Bluetooth game controller or bare hands; pinch in the menus.

EOF
if [[ ${interactive} -eq 1 ]]; then printf 'Press Return to close this window.'; read -r; fi

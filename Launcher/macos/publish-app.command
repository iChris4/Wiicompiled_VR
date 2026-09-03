#!/usr/bin/env bash
# Turn one locally compiled macOS product into a self-contained .app bundle.
set -euo pipefail

fail() { printf 'publish-app.command: error: %s\n' "$*" >&2; exit 1; }
usage() {
    cat <<'EOF'
Usage: publish-app.command --build-dir DIR --product {WiiCompiled|RetroRewind} --output-dir DIR

Copies a locally built product and its runtime assets into OUTPUT-DIR/<product>.app.
It bundles non-system dylibs, rewrites their install names, and ad-hoc signs the
result. This is suitable for local use; a release must replace ad-hoc signing
with the project's Developer ID signing and notarization process.
EOF
}

build_dir=""; product=""; output_dir=""
while (($#)); do
    case "$1" in
        --build-dir) build_dir=${2:-}; shift 2 ;;
        --product) product=${2:-}; shift 2 ;;
        --output-dir) output_dir=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
done
[[ "$product" == WiiCompiled || "$product" == RetroRewind ]] || fail '--product must be WiiCompiled or RetroRewind'
for tool in codesign ditto install_name_tool otool; do command -v "$tool" >/dev/null || fail "required macOS tool is unavailable: $tool"; done
[[ -x "$build_dir/$product" ]] || fail "missing compiled product: $build_dir/$product"
for asset in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap; do [[ -e "$build_dir/$asset" ]] || fail "missing runtime asset: $build_dir/$asset"; done

app="$output_dir/$product.app"
macos="$app/Contents/MacOS"
frameworks="$app/Contents/Frameworks"
resources="$app/Contents/Resources"
rm -rf "$app"
mkdir -p "$macos" "$frameworks" "$resources"
cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>$product</string>
  <key>CFBundleIdentifier</key><string>org.wiicompiled.$product</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>$product</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>14.0</string>
  <key>NSHighResolutionCapable</key><true/>
</dict></plist>
EOF
ditto "$build_dir/$product" "$macos/$product"
for asset in dsp_coef.bin initial_pipeline_cache.db wii_bootstrap; do
    ditto "$build_dir/$asset" "$resources/$asset"
    ln -s "../Resources/$asset" "$macos/$asset"
done

# Build a closure of Homebrew dylibs. System libraries remain system references.
queue=("$macos/$product")
while ((${#queue[@]})); do
    current=${queue[0]}
    queue=("${queue[@]:1}")
    while IFS= read -r dependency; do
        [[ "$dependency" == /opt/homebrew/* || "$dependency" == /usr/local/* ]] || continue
        [[ -f "$dependency" ]] || continue
        name=$(basename "$dependency")
        if [[ ! -f "$frameworks/$name" ]]; then
            ditto "$dependency" "$frameworks/$name"
            install_name_tool -id "@rpath/$name" "$frameworks/$name"
            queue+=("$frameworks/$name")
        fi
    done < <(otool -L "$current" | tail -n +2 | awk '{print $1}')
done
while IFS= read -r binary; do
    while IFS= read -r old; do
        [[ "$old" == /opt/homebrew/* || "$old" == /usr/local/* ]] || continue
        name=$(basename "$old")
        [[ -f "$frameworks/$name" ]] || continue
        if [[ "$binary" == "$macos/$product" ]]; then
            install_name_tool -change "$old" "@executable_path/../Frameworks/$name" "$binary"
        else
            install_name_tool -change "$old" "@loader_path/$name" "$binary"
        fi
    done < <(otool -L "$binary" | tail -n +2 | awk '{print $1}')
done < <(find "$frameworks" -type f -print; printf '%s\n' "$macos/$product")

find "$frameworks" -type f -exec codesign --force --sign - {} +
codesign --force --deep --sign - "$app"
codesign --verify --deep --strict "$app"
printf 'MKWCBUILD:APP=%s\n' "$app"

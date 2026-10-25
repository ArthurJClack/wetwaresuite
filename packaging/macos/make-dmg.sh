#!/usr/bin/env bash
# wetwaresuite.dmg for macOS: a "wetwaresuite" folder with one .app per app
# (wetwaresuite image.app, wetwaresuite cad.app, ...) plus the launcher
# (wetwaresuite.app), and an Applications shortcut so installing is one drag.
# Each app is its own program with its own dock icon (the logo in its colour)
# and the file types it opens; they find each other through the sibling bundles.
#   packaging/macos/make-dmg.sh <stage dir> <version> <out.dmg>
# <stage dir> is the folder the downloads workflow packs into the zip.
set -euo pipefail

stage=$1
version=${2#v}
plist_version=${version%%-*}  # Info.plist wants plain numbers (0.2.0, not 0.2.0-preview)
out=$3
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
folder="$work/dmg/wetwaresuite"
mkdir -p "$folder"

# CFBundleDocumentTypes for one app, from packaging/file-types.tsv
doc_types() {
  local app=$1 exts
  exts=$(awk -F'\t' -v app="$app" '!/^#/ && $2 == app { sub(/^\./, "", $1); printf "<string>%s</string>", $1 }' "$root/packaging/file-types.tsv")
  [ -z "$exts" ] && return 0
  local own
  own=$(awk -F'\t' -v app="$app" '!/^#/ && $2 == app && $4 == "yes" { sub(/^\./, "", $1); printf "<string>%s</string>", $1 }' "$root/packaging/file-types.tsv")
  echo "  <key>CFBundleDocumentTypes</key><array>"
  if [ -n "$own" ]; then
    echo "    <dict><key>CFBundleTypeName</key><string>wetwaresuite $app file</string><key>CFBundleTypeRole</key><string>Editor</string><key>LSHandlerRank</key><string>Owner</string><key>CFBundleTypeExtensions</key><array>$own</array></dict>"
  fi
  echo "    <dict><key>CFBundleTypeName</key><string>files wetwaresuite $app opens</string><key>CFBundleTypeRole</key><string>Editor</string><key>LSHandlerRank</key><string>Alternate</string><key>CFBundleTypeExtensions</key><array>$exts</array></dict>"
  echo "  </array>"
}

# make_app <bundle name> <program> <bundle id> <icns> <app id or "">
make_app() {
  local name=$1 prog=$2 id=$3 icns=$4 app=$5
  local bundle="$folder/$name.app"
  mkdir -p "$bundle/Contents/MacOS" "$bundle/Contents/Resources"
  cp "$stage/$prog" "$bundle/Contents/MacOS/"
  cp "$icns" "$bundle/Contents/Resources/icon.icns"
  cp "$root/LICENSE" "$bundle/Contents/Resources/"
  cat > "$bundle/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>$name</string>
  <key>CFBundleDisplayName</key><string>$name</string>
  <key>CFBundleIdentifier</key><string>$id</string>
  <key>CFBundleExecutable</key><string>$prog</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$plist_version</string>
  <key>CFBundleVersion</key><string>$plist_version</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.graphics-design</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>GPL-3.0</string>
$( [ -n "$app" ] && doc_types "$app" )
</dict>
</plist>
EOF
  # ad-hoc signature: not notarized, but apple silicon needs a signed bundle to run at all
  codesign --force --sign - "$bundle"
}

[ -x "$stage/wetwaresuite-gui" ] || { echo "no wetwaresuite-gui in $stage" >&2; exit 1; }
# the launcher also carries the command-line programs (wetwaresuite, the hub, share)
make_app wetwaresuite wetwaresuite-gui io.github.arthurjclack.wetwaresuite "$root/branding/wetwaresuite.icns" ""
for prog in wetwaresuite wetwaresuite-hub wetwaresuite-share; do
  [ -x "$stage/$prog" ] && cp "$stage/$prog" "$folder/wetwaresuite.app/Contents/MacOS/"
done
codesign --force --sign - "$folder/wetwaresuite.app"
while IFS=$'\t' read -r id name comment; do
  [[ -z "$id" || "$id" == \#* ]] && continue
  [ -x "$stage/wetwaresuite-$id" ] || { echo "no wetwaresuite-$id in $stage" >&2; exit 1; }
  make_app "$name" "wetwaresuite-$id" "io.github.arthurjclack.wetwaresuite.$id" "$root/branding/apps/$id.icns" "$id"
done < "$root/packaging/apps.tsv"

ln -s /Applications "$work/dmg/Applications"
cat > "$work/dmg/README.txt" <<'EOF'
drag the wetwaresuite folder into Applications. every app is in it and shows up
in Launchpad on its own: wetwaresuite image, wetwaresuite cad, ... each opens in
its own window with its own dock icon. "wetwaresuite" is the launcher.

first open (the apps are not notarized yet): right-click an app, choose Open,
then Open again. or run once in Terminal:
  xattr -dr com.apple.quarantine /Applications/wetwaresuite

from a terminal:
  "/Applications/wetwaresuite/wetwaresuite image.app/Contents/MacOS/wetwaresuite-image" help
  /Applications/wetwaresuite/wetwaresuite.app/Contents/MacOS/wetwaresuite help

tutorials: https://github.com/ArthurJClack/wetwaresuite/tree/main/docs/tutorials
license: GPL-3.0
EOF

rm -f "$out"
hdiutil create -volname wetwaresuite -srcfolder "$work/dmg" -ov -format UDZO "$out"
ls -la "$out"

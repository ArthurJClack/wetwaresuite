#!/usr/bin/env bash
# wetwaresuite.dmg for macOS: one wetwaresuite.app (the launcher window with
# every tool) that carries every program inside it, plus an Applications
# shortcut so installing is one drag.
#   packaging/macos/make-dmg.sh <stage dir> <version> <out.dmg>
# <stage dir> is the folder the downloads workflow packs into the zip.
set -euo pipefail

stage=$1
version=${2#v}
out=$3
root=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

app="$work/dmg/wetwaresuite.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
# every program sits next to the launcher so they find each other (app.installed, app.send)
find "$stage" -maxdepth 1 -type f -name 'wetwaresuite*' -perm -u+x -exec cp {} "$app/Contents/MacOS/" \;
[ -x "$app/Contents/MacOS/wetwaresuite-gui" ] || { echo "no wetwaresuite-gui in $stage" >&2; exit 1; }
cp "$root/branding/wetwaresuite.icns" "$app/Contents/Resources/"
cp "$root/LICENSE" "$app/Contents/Resources/"

cat > "$app/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>wetwaresuite</string>
  <key>CFBundleDisplayName</key><string>wetwaresuite</string>
  <key>CFBundleIdentifier</key><string>io.github.arthurjclack.wetwaresuite</string>
  <key>CFBundleExecutable</key><string>wetwaresuite-gui</string>
  <key>CFBundleIconFile</key><string>wetwaresuite</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$version</string>
  <key>CFBundleVersion</key><string>$version</string>
  <key>LSMinimumSystemVersion</key><string>11.0</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.graphics-design</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSHumanReadableCopyright</key><string>GPL-3.0</string>
</dict>
</plist>
EOF

# ad-hoc signature: not notarized, but apple silicon needs a signed bundle to run at all
codesign --force --deep --sign - "$app"

ln -s /Applications "$work/dmg/Applications"
cat > "$work/dmg/README.txt" <<'EOF'
drag wetwaresuite into Applications, then open it from Launchpad.

first open (the app is not notarized yet): right-click wetwaresuite, choose Open,
then Open again. or run once in Terminal:
  xattr -dr com.apple.quarantine /Applications/wetwaresuite.app

each app on its own, or from a terminal:
  /Applications/wetwaresuite.app/Contents/MacOS/wetwaresuite-image
  /Applications/wetwaresuite.app/Contents/MacOS/wetwaresuite help

tutorials: https://github.com/ArthurJClack/wetwaresuite/tree/main/docs/tutorials
license: GPL-3.0
EOF

rm -f "$out"
hdiutil create -volname wetwaresuite -srcfolder "$work/dmg" -ov -format UDZO "$out"
ls -la "$out"

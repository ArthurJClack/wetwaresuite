#!/usr/bin/env bash
# linux installers from the folder the downloads workflow packs into the zip:
#   wetwaresuite-linux-x86_64.AppImage  one file, chmod +x and double-click, any distro
#   wetwaresuite-linux-amd64.deb        ubuntu/debian/mint: start-menu entry, icon and file types per app
#   packaging/linux/make-packages.sh <stage dir> <version> <out dir>
# the AppImage needs appimagetool; set APPIMAGETOOL to a local copy, or it is
# downloaded. set SKIP_APPIMAGE=1 to build only the .deb.
set -euo pipefail

stage=$(cd "$1" && pwd)
version=${2#v}
mkdir -p "$3"
out=$(cd "$3" && pwd)
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# lays out a /usr tree: programs, icons, one start-menu entry per app
usr_tree() {
  local usr=$1
  mkdir -p "$usr/bin" "$usr/share/applications" "$usr/share/doc/wetwaresuite"
  find "$stage" -maxdepth 1 -type f -name 'wetwaresuite*' -perm -u+x -exec cp {} "$usr/bin/" \;
  [ -x "$usr/bin/wetwaresuite-gui" ] || { echo "no wetwaresuite-gui in $stage" >&2; exit 1; }
  cp "$root/branding/wetwaresuite.desktop" "$usr/share/applications/"
  mkdir -p "$usr/share/icons/hicolor/scalable/apps" "$usr/share/icons/hicolor/256x256/apps" "$usr/share/mime/packages"
  # each app is its own program: its own entry, icon (the logo in its colour), window class and file types
  while IFS=$'\t' read -r id name comment; do
    [[ -z "$id" || "$id" == \#* ]] && continue
    [ -x "$usr/bin/wetwaresuite-$id" ] || { echo "no wetwaresuite-$id in $stage" >&2; exit 1; }
    mimes=$(awk -F'\t' -v app="$id" '!/^#/ && $2 == app { printf "%s;", $3 }' "$root/packaging/file-types.tsv")
    cat > "$usr/share/applications/wetwaresuite-$id.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=$name
Comment=$comment
Exec=wetwaresuite-$id %F
Icon=wetwaresuite-$id
StartupWMClass=wetwaresuite-$id
Terminal=false
Categories=Graphics;Office;
MimeType=$mimes
EOF
    cp "$root/branding/apps/$id.svg" "$usr/share/icons/hicolor/scalable/apps/wetwaresuite-$id.svg"
    cp "$root/branding/apps/$id.png" "$usr/share/icons/hicolor/256x256/apps/wetwaresuite-$id.png"
  done < "$root/packaging/apps.tsv"
  # wetwaresuite's own formats (.wwcad, .wwcut, ...) are new types to the desktop: name them
  {
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<mime-info xmlns="http://www.freedesktop.org/standards/shared-mime-info">'
    awk -F'\t' '!/^#/ && $4 == "yes" { printf "  <mime-type type=\"%s\"><comment>wetwaresuite %s file</comment><glob pattern=\"*%s\"/></mime-type>\n", $3, $2, $1 }' \
      "$root/packaging/file-types.tsv"
    echo '</mime-info>'
  } > "$usr/share/mime/packages/wetwaresuite.xml"
  cp "$root/branding/icon.svg" "$usr/share/icons/hicolor/scalable/apps/wetwaresuite.svg"
  for size in 16 32 48 64 128 256 512; do
    mkdir -p "$usr/share/icons/hicolor/${size}x${size}/apps"
    cp "$root/branding/png/icon-$size.png" "$usr/share/icons/hicolor/${size}x${size}/apps/wetwaresuite.png"
  done
  cp "$root/LICENSE" "$usr/share/doc/wetwaresuite/copyright"
}

# --- .deb ---
pkg="$work/deb"
usr_tree "$pkg/usr"
mkdir -p "$pkg/DEBIAN"
size=$(du -sk "$pkg/usr" | cut -f1)
cat > "$pkg/DEBIAN/control" <<EOF
Package: wetwaresuite
Version: $version
Architecture: amd64
Maintainer: wetwaresuite <https://github.com/ArthurJClack/wetwaresuite/issues>
Installed-Size: $size
Depends: libc6 (>= 2.35), libstdc++6, libgcc-s1
Section: graphics
Priority: optional
Homepage: https://github.com/ArthurJClack/wetwaresuite
Description: all-in-one lightweight creative and office suite
 cad, blueprints, photo, paint, audio, video, game assets, office, maps,
 circuits and a universal file toolkit as small standalone apps on one
 shared core. every app also runs headless and as an MCP server.
EOF
dpkg-deb --root-owner-group --build "$pkg" "$out/wetwaresuite-linux-amd64.deb"

# --- AppImage ---
if [ "${SKIP_APPIMAGE:-0}" != 1 ]; then
  appdir="$work/wetwaresuite.AppDir"
  usr_tree "$appdir/usr"
  cp "$here/AppRun" "$appdir/AppRun"
  chmod +x "$appdir/AppRun"
  cp "$root/branding/wetwaresuite.desktop" "$appdir/"
  cp "$root/branding/png/icon-256.png" "$appdir/wetwaresuite.png"
  ln -s wetwaresuite.png "$appdir/.DirIcon"
  tool=${APPIMAGETOOL:-}
  if [ -z "$tool" ]; then
    tool="$work/appimagetool"
    curl -fsSL -o "$tool" https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage
    chmod +x "$tool"
  fi
  # extract-and-run: works without FUSE (CI runners, containers)
  APPIMAGE_EXTRACT_AND_RUN=1 ARCH=x86_64 "$tool" --no-appstream "$appdir" "$out/wetwaresuite-linux-x86_64.AppImage"
fi
ls -la "$out"

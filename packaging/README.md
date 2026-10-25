# packaging

one-file installers per OS, built by the `downloads` workflow (`.github/workflows/downloads.yml`) from the same folder that goes into each zip, and attached to every release next to the zips.

| file | OS | built by | what you get |
| --- | --- | --- | --- |
| `wetwaresuite-windows-x64-setup.exe` | windows 10/11 | `windows/wetwaresuite.iss` (Inno Setup 6) | per-user install (no admin needed, or pick all users), start menu entry per app with its own icon, each app in "Open with" for its file types, optional desktop shortcuts and PATH entry, uninstaller in Settings > Apps |
| `wetwaresuite-macos-arm64.dmg` | macOS 11+, apple silicon | `macos/make-dmg.sh` | a `wetwaresuite` folder with one `.app` per app (own dock icon and file types) plus `wetwaresuite.app`, the launcher, and an Applications shortcut to drag it onto |
| `wetwaresuite-linux-x86_64.AppImage` | any linux with glibc 2.35+ | `linux/make-packages.sh` + `linux/AppRun` | one file, no install. `--run image` starts one app; a symlink named `wetwaresuite-image` does too |
| `wetwaresuite-linux-amd64.deb` | ubuntu 22.04+, debian 12+, mint | `linux/make-packages.sh` | every program in `/usr/bin`, a start-menu entry, icon and file types per app |

`apps.tsv` is the app list (id, start-menu name, one-line comment) the linux entries and mac bundles come from; the windows start menu lists the same apps by hand and the workflow fails if they drift apart.

`file-types.tsv` (ext, app, mime type, own) says which app opens which file type, and `windows/associations.iss` registers them on windows. Both are made by `python3 file_types.py` from the table in `apps/common/apps.cpp`; `test_apps` and the downloads workflow fail when they're out of date. Each app is only added to "Open with" for other programs' formats; it becomes the default only for wetwaresuite's own (`.wwcad`, `.wwcut`, ...). `release-notes.md` goes at the top of every release's notes.

the AppImage's launcher and every app inside are the same builds as the zip, so `app.installed` and `app.send` find each other the same way.

## updates

installed copies update themselves from the newest release (see `docs/tutorials/updates.md`): the windows setup, zips and hub installs swap their programs in place from the zip, the macOS folder its `.app` bundles from the dmg, the AppImage itself; the `.deb` and all-users windows installs download and install on a click. so a release must keep these asset names, and the workflow builds a release with `-DWETWARESUITE_VERSION=<the version>` (the updater compares it with the release tag) and attaches `SHA256SUMS`. other builds report `<version>-preview` and never update by themselves.

## build one locally

after a release build (`cmake --build build --config Release`, hub and share with `cargo build --release`), make the stage folder the way the workflow's `package` step does, then:

```sh
packaging/linux/make-packages.sh wetwaresuite-linux-x64 0.2.0 dist     # SKIP_APPIMAGE=1 for just the .deb
packaging/macos/make-dmg.sh wetwaresuite-macos-arm64 0.2.0 dist/wetwaresuite-macos-arm64.dmg
iscc /DAppVersion=0.2.0 /DStageDir=%CD%\wetwaresuite-windows-x64 /DOutputDir=%CD%\dist packaging\windows\wetwaresuite.iss
```

all three tools are free (Inno Setup's own license, appimagetool MIT, hdiutil and dpkg-deb ship with the OS); none of them ends up inside what gets installed, so the suite stays GPL-3.0 only.

## not done yet

- code signing and notarization (so the first-run warnings go away) needs paid certificates, so the builds are unsigned for now.
- intel macs and arm linux/windows have no build yet (the CI runners build x64 windows/linux and arm64 mac).

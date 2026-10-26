# Updates

An installed wetwaresuite keeps itself up to date. Once a day, when you open an app, it quietly asks GitHub whether there's a newer release, downloads it, checks it and puts it in place. You don't have to click anything, nothing pops up, and the apps you have open keep working; the next time you start one you get the new version. Updates are free like everything else: there's no paid channel and nothing is held back.

This page covers the window, the command line, batch scripts and Claude (MCP), what happens on each kind of install, and how to turn it off.

## What it does, step by step

1. **When**: any app's window starts a background check at most once a day (an hour after a failed try, so being offline never nags). Running commands, batch scripts and `mcp` never check by themselves.
2. **Check**: one request to `https://api.github.com/repos/ArthurJClack/wetwaresuite/releases/latest` (the newest full release; previews are skipped). Nothing about you or your files is sent, and there's no account.
3. **Download**: the one file your install needs (see the table below), into the `updates` folder in the data folder, with `curl` (built into windows 10 and 11, macOS and most linux).
4. **Check it**: the file's SHA-256 has to match the one the release lists (GitHub's own digest for the file, or the `SHA256SUMS` file the downloads workflow attaches). A file that doesn't match is thrown away.
5. **Make sure it starts**: the new `wetwaresuite --version` has to answer with the new version before anything is replaced.
6. **Swap**: the old programs are renamed out of the way and the new ones moved in. If any step fails, the old ones are put back, so a half-done update can't break your install.

The check runs in its own small process (the `files` app), so closing the window doesn't cut it off, and only one app updates at a time.

## Each kind of install

| You installed with | What the update does | Needs a password? |
| --- | --- | --- |
| windows `setup.exe`, just for you (the default) | swaps the programs in place from the new zip; Settings > Apps shows the new version | no |
| windows `setup.exe`, for all users (Program Files) | downloads the new `setup.exe`; click **Install update** and it asks for admin rights | yes, once per update |
| macOS `.dmg` (the `wetwaresuite` folder in Applications) | swaps each `.app` in the folder from the new dmg | no |
| linux AppImage | swaps the one `.AppImage` file | no |
| linux `.deb` | downloads the new `.deb`; click **Install update** (or run `app.update_install`) and it asks for your password | yes, once per update |
| a zip, unpacked anywhere | swaps the programs in the folder | no |
| `hub install` | swaps the apps you installed (and only those) | no |
| built from source | never updates by itself: `git pull` and rebuild. `app.update_check` still tells you when a release is out | - |
| a `preview` build | doesn't update by itself (it's ahead of the newest release); `app.update_install` moves it to the newest release | - |

A folder you can't write to (say, a zip unpacked somewhere read-only) gets a plain message saying so; move the folder somewhere of yours.

## In the window

Click **Aa** at the top right of any app (or **Updates** in the launcher):

- **Update by itself**: on by default. Turn it off and no app checks again until you turn it back on. It's one setting for every app.
- **Check now**: checks straight away (and installs, when updating by itself is on). It runs in the background, so you can keep working; the line underneath says how it went, for example *You have the newest version (0.2.0).* or *Updated to 0.3.0. Apps that are open keep the old version until you start them again.*
- **Install update**: shows up when a new version is waiting, for example when updates are off, or on a `.deb` or an all-users windows install that needs your password.
- Offline, the line says *Couldn't check for updates: offline (couldn't reach github.com). It tries again later.* and that's all.

## From the command line

Every app has the same commands (they're part of `app.*`), so any of them works:

```sh
wetwaresuite-files app.update_status             # this version, the kind of install, on/off, the last check
wetwaresuite-files app.update_check              # is there a newer release? downloads nothing
wetwaresuite-files app.update_install            # download, check, install now (waits until it's done)
wetwaresuite-files app.update_install background=true   # the same in its own process; returns at once
wetwaresuite-files app.update_settings auto=false        # stop updating by itself (auto=true turns it back on)
```

What `app.update_status` prints, for example:

```
wetwaresuite 0.2.0 (folder install in /home/me/wetwaresuite-linux-x64)
updates by itself: on
last check: You have the newest version (0.2.0).
```

`app.update_check` and `app.update_install` exit with 1 when they couldn't do it (offline, a bad download), so scripts can tell.

## Servers, cron and batch scripts

A headless machine never checks by itself (only the window does). To keep a server up to date, run the install from cron or Task Scheduler:

```sh
# crontab -e: every night at 4
0 4 * * * /opt/wetwaresuite/wetwaresuite-files app.update_install
```

Or in a [batch script](headless-and-batch.md):

```
# update.txt: run with  wetwaresuite batch update.txt
app.update_check
app.update_install
app.update_status
```

## With Claude (MCP)

The tools are `app_update_status`, `app_update_check`, `app_update_install` and `app_update_settings`, in every app's MCP server. Ask, for example, "is wetwaresuite up to date?", "update wetwaresuite" or "turn off wetwaresuite's automatic updates".

## Turning it off

Any one of these:

- untick **Update by itself** in the Aa menu,
- `app.update_settings auto=false`,
- edit `update.txt` in the data folder (`~/.local/share/wetwaresuite` on linux, `~/Library/Application Support/wetwaresuite` on macOS, `%APPDATA%\wetwaresuite` on windows) to say `auto=off`,
- set the environment variable `WETWARESUITE_NO_UPDATE=1` (for packagers, CI and locked-down machines: nothing checks in the background, whatever the setting says).

## For people making releases

The updater only needs what the [downloads workflow](../../packaging/README.md) already makes: a release with the installers and zips. A release build reports its own version (`-DWETWARESUITE_VERSION=0.3.0`, set from the version you type in Actions > downloads > Run workflow), and the workflow attaches a `SHA256SUMS` file. Every other build reports `<version>-preview`, so preview and pull request builds never update themselves.

`WETWARESUITE_UPDATE_URL` points the updater at another release feed (a fork's `.../releases/latest`, or a `file://` JSON file in tests).

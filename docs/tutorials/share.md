# Share: send files and live-sync folders over the LAN or internet

`achclwss-share` is one small program (about 1 MB, no dependencies) that does four jobs:

- **share a folder with any device**: one command prints a link, a QR code and a 6-character code. The other side needs nothing installed: a phone, tablet or laptop browser gets a file browser with downloads, "download folder as zip" and, if you allow it, uploads by drag and drop.
- **live-sync a code project between two machines**: edit on your laptop, the change is on your desktop a fraction of a second later, and the other way round. `.gitignore` rules are respected and `node_modules`, `target`, `.git` and friends stay out by default.
- **send one file or folder** to another machine that runs the tool.
- **let Claude do all of the above** over MCP.

Everything is headless: it runs in a terminal, over SSH, in a script or under Claude. When the ACHCLWSS GUI lands, its "Share this folder" button will call exactly these commands, so what you learn here carries over.

Every command and output below was run against the real binary. Addresses, tokens and codes differ on your machines.

## Contents

1. [Get the binary](#1-get-the-binary)
2. [Share a folder with a phone or laptop (one command)](#2-share-a-folder-with-a-phone-or-laptop)
3. [Live-sync a code project between two dev machines](#3-live-sync-a-code-project-between-two-machines)
4. [Send one file or folder](#4-send-one-file-or-folder)
5. [Find shares nearby](#5-find-shares-nearby)
6. [Share over the internet (cloudflared or tailscale)](#6-share-over-the-internet)
7. [Drive it from Claude (MCP)](#7-drive-it-from-claude-mcp)
8. [Security: what protects your files, and what doesn't](#8-security)
9. [Troubleshooting](#9-troubleshooting)
10. [Command reference](#10-command-reference)

## 1. Get the binary

It is a dependency-free Rust crate in `share/`:

```sh
cd share
cargo build --release
# the binary: share/target/release/achclwss-share (achclwss-share.exe on Windows)
```

Copy it anywhere on your `PATH`, or next to the `hub` binary so `hub share ...` finds it. `hub list` shows it as the `share` tool, and `hub mcp-config` includes it.

```sh
$ achclwss-share --version
achclwss-share 0.1.0
```

## 2. Share a folder with a phone or laptop

### The one-command version

Go to the folder and run the tool with no arguments:

```sh
$ cd ~/Pictures/holiday
$ achclwss-share
Sharing /home/me/Pictures/holiday (read-only). Press Ctrl+C to stop.

  On another device:  http://192.168.1.20:8740/?k=eu23xrfuios6eax2zn43z4xnum6cg3ie
  On this machine:    http://127.0.0.1:8740/?k=eu23xrfuios6eax2zn43z4xnum6cg3ie
  Pairing code:       Y2JQUF   (type it on the page, or `achclwss-share receive Y2JQUF`)

  Scan with a phone camera:

  █▀▀▀▀▀█  ▀█▀█ ▀▄█▄█▀ ▀▀▀█ █▀▀▀▀▀█
  █ ███ █ █▄▄ ▀ ▀▄ ▄   ▀██  █ ███ █
  █ ▀▀▀ █ █ ▀▄▄▀▀▀▀█▀▀▀▀▀▀█ █ ▀▀▀ █
  ▀▀▀▀▀▀▀ █▄█ █ ▀ █ ▀▄█▄▀ ▀ ▀▀▀▀▀▀▀
  ...

  Listening on all network interfaces (0.0.0.0) port 8740; use --local for this machine only.
  LAN traffic is plain HTTP: anyone with the link or code can open it. Only share it with people you trust.
```

That is the whole setup. On the other device, any of these works:

- **scan the QR code** with the phone's camera app and tap the link;
- **open the "On another device" link** (paste it into a chat with yourself, for example);
- **type the address without the `?k=...` part** (`http://192.168.1.20:8740`) and enter the **pairing code** `Y2JQUF` on the page that appears. Codes are case-insensitive and skip look-alike characters (no 0/O or 1/I/L).

The QR code is drawn in black on white with ANSI colours so it scans on dark and light terminals. It only appears when the output is a terminal; `--no-qr` hides it.

The page lists the folder. Tap a folder to open it, a file to view it in the browser, `download` to save it, `zip` (or "Download all") to get a whole folder as one zip file. Zips are streamed as they are built, so even a big folder starts downloading at once. Videos and audio can be scrubbed (the server supports range requests). The filter box narrows the list, and the column headers sort it.

The terminal shows what happens (uploads, deletes, pairings, blocked guessers). Press **Ctrl+C** to stop sharing; the link stops working immediately.

### Choosing what to share

```sh
achclwss-share serve ~/Projects/site        # a folder
achclwss-share serve ~/Downloads/video.mp4   # just one file: nothing else is visible
achclwss-share ~/Projects/site               # a bare folder path works too
```

With no folder, the current folder is shared, and the first line always says which. As a safety net, a bare `achclwss-share` refuses to share your whole home folder or a drive root (`/`, `C:\`); name it explicitly (`achclwss-share serve ~`) if you really mean it.

**Double-clicking the binary** works as a "1-click share", with one honest caveat: the folder it shares is whatever folder the system starts it in.

| system | what a double-click shares |
| --- | --- |
| Windows (Explorer) | the folder the `.exe` is in. Put `achclwss-share.exe` in the folder you want to share (or make a shortcut whose "Start in" is that folder) and double-click it. Close the window to stop. |
| macOS (Finder) | Terminal opens in your home folder, which the tool refuses to share by accident. Run it from a terminal in the right folder instead. |
| Linux file managers | varies (often your home folder, refused the same way). Use a terminal or a launcher with a working directory set. |

### Letting people upload

By default a share is **read-only**. Add `--write` to let visitors upload files, create folders, rename and delete:

```sh
$ achclwss-share serve ~/inbox --write
Sharing /home/me/inbox (read-write). Press Ctrl+C to stop.
...
  127.0.0.1 uploaded holiday/IMG_2041.jpg (3.1 MB)
```

The page then shows **Upload** and **New folder** buttons, `rename`/`delete` on each row, and accepts drag and drop of files *and whole folders* anywhere on the page. A progress bar shows the upload. Uploads land atomically: a half-finished upload never replaces a file, and a file's modification time is kept.

`--max-upload MB` caps a single upload (default 4096 MB).

### Ports and interfaces

The default port is 8740; if it's busy the next free one is used (8741, 8742, ...). `--port N` picks one exactly (and fails if it's taken; `--port 0` lets the OS choose). By default the share listens on all interfaces (`0.0.0.0`), which is what makes it reachable from the LAN; `--local` listens on `127.0.0.1` only (useful with a tunnel, see section 6), and `--bind IP` on one address.

## 3. Live-sync a code project between two machines

Say you edit on a laptop and build or run on a desktop (or the other way round, or a laptop and a Raspberry Pi). You want both copies of `~/code/myapp` to stay identical while you work, without committing and pulling.

### Pair the machines

On the first machine (the **host**), say the desktop:

```sh
desktop$ achclwss-share sync ~/code/myapp
Hosting live sync of /home/me/code/myapp (read-write). Press Ctrl+C to stop.

  On another device:  http://192.168.1.20:8770/?k=edtzgny5rz5ewde5ke5bujhvpqk3duob
  On this machine:    http://127.0.0.1:8770/?k=edtzgny5rz5ewde5ke5bujhvpqk3duob
  Pairing code:       EYCZPJ   (on the other machine: achclwss-share sync <folder> EYCZPJ)

  Waiting for the other machine. Changes made there show up here, and the other way round.
```

On the second machine, give the folder to keep in sync and the code:

```sh
laptop$ achclwss-share sync ~/code/myapp EYCZPJ
Looking for EYCZPJ...
Syncing /home/me/code/myapp with myapp on desktop (http://192.168.1.20:8770). Press Ctrl+C to stop.
  Skipping .git, node_modules, target, build, .venv, __pycache__ and .gitignore/.shareignore matches.
  ↓ .gitignore (12 B)
  ↓ package.json (17 B)
  ↓ src/index.js (18 B)
  In sync (3 files). Watching for changes...
```

The laptop found the desktop on the network by its code (see section 5), paired, and pulled the project down. The folder on the laptop didn't need to exist. Meanwhile the desktop logs:

```
  192.168.1.31 paired with the code
  sync peer connected; watching the folder for changes
```

Now edit anywhere. Save `src/util.js` on the laptop and the desktop has it within a few hundred milliseconds; edit `src/index.js` on the desktop and the laptop gets it just as fast; delete a file on either side and it disappears on the other:

```
  ↑ src/util.js (19 B)
  ↓ src/index.js (33 B)
  ✗ package.json (deleted there)
```

(`↑` sent to the other machine, `↓` received, `✗` deleted.) Stop with Ctrl+C on either machine; start it again any time and it catches up.

You can also join with the full link instead of the code, which is the way to go when the machines are on different networks or discovery is blocked:

```sh
laptop$ achclwss-share sync ~/code/myapp "http://192.168.1.20:8770/?k=edtzgny5rz5ewde5ke5bujhvpqk3duob"
```

or with the address plus the code: `achclwss-share sync ~/code/myapp http://192.168.1.20:8770 --code EYCZPJ`.

More than one machine can join the same host; each stays in sync with it (and so with each other).

### How fast is it?

Measured on loopback (the automated test in `share/tests/sync.rs`): a new, edited or deleted file shows up on the other side in **50 to 250 ms**, a 1 MB file in about 110 ms. On a real LAN add your network's round trip. The joining side checks its own folder every 250 ms (stat only; a file is re-hashed only when its size or time changed) and holds a long-poll open to the host, which rescans every 150 ms while someone is connected, so host-side changes are pushed immediately. Only changed files are transferred, whole (there's no delta encoding, so a small edit to a 500 MB file resends it).

### What's synced and what isn't

These folders are skipped everywhere in the tree, because they're large, machine-specific or regenerable:

`.git/`  `node_modules/`  `target/`  `build/`  `.venv/`  `__pycache__/`

Pass `--include-heavy` (on the joining side) to sync them too. Syncing `.git/` between two machines that both run git commands is a recipe for a corrupted repository, so leave it out unless you know only one side touches git.

On top of that, **`.gitignore` and `.shareignore`** files are honoured, in any folder, with the usual git syntax:

```gitignore
# .shareignore: like .gitignore, but only for achclwss-share.
# Comments go on their own lines (as in git, "#" mid-line is part of the pattern).

# any .log file, at any depth
*.log
# but do sync keep.log (a later "!" rule re-includes)
!keep.log
# only at the top of the project (a leading slash anchors the pattern)
/local.env
# a folder named dist, anywhere (a trailing slash means folders only)
dist/
# pdfs directly inside docs/ (a slash in the middle anchors too)
docs/*.pdf
# "raw" any number of folders below assets/
assets/**/raw
# "cache" anywhere
**/cache
# a file literally named "#notes.txt"
\#notes.txt
```

Rules from deeper folders and later lines win, and, as in git, nothing inside an ignored folder can be re-included. Use `.shareignore` for things you want in git but not synced (or the reverse: a `!` rule in `.shareignore` re-includes something `.gitignore` excludes, like a local `.env` you do want on both machines).

The ignore files sync too, so both machines converge on the same rules. Ignored files are left alone on both sides: they're never sent, never deleted, never overwritten. Symlinks are skipped (they're not followed, so a link can't pull files from outside the folder into the sync). Empty folders aren't synced, and a folder emptied by a synced delete is removed (like git). Files whose names Windows can't store (`a:b`, `CON`, trailing dots) are skipped so a project behaves the same on every OS.

### Conflicts

If both machines change the same file before either has seen the other's change (for example while the laptop was offline), nothing is lost: the **host's version keeps the name**, and the joining machine's version is saved next to it as `name.conflict-<machine>.ext` on both sides:

```
  ! conflict on README.md: kept desktop's version, saved this one as README.conflict-laptop.md
  ↓ README.md (10 B)
  ↑ README.conflict-laptop.md (12 B)
```

Compare the two, keep what you want and delete the other; the deletion syncs. Other cases resolve themselves:

| this side | other side | result |
| --- | --- | --- |
| changed | unchanged | this side's version is copied over |
| deleted | unchanged | deleted on the other side too |
| deleted | changed | the change wins: the file comes back with the new content |
| changed | changed to the same content | nothing to do |
| changed | changed differently | conflict copy, as above |
| new file | new file with different content (first sync) | conflict copy, as above |

Every upload and delete tells the other side which version it expects to replace; if the file changed there in the meantime the write is refused and the next round treats it as a conflict instead. A sync never overwrites an edit it hasn't seen.

The joining machine remembers the last synced state (in `~/.cache/achclwss-share/sync/`, `~/Library/Caches/...` on macOS, `%LOCALAPPDATA%\achclwss-share\sync\` on Windows), so files deleted while sync was stopped stay deleted when it restarts. If that cache is cleared, the next start behaves like a first sync: nothing is deleted, files that differ become conflict copies.

## 4. Send one file or folder

For a one-off transfer between two machines that both have the tool.

**Pull style**: the sender offers, the receiver fetches.

```sh
sender$ achclwss-share send ~/Downloads/dataset.zip
Sending /home/me/Downloads/dataset.zip (read-only). Press Ctrl+C to stop.
  ...
  Pairing code:       Q4M8ZT   (on the other machine: achclwss-share receive Q4M8ZT)
  Will stop after the first finished download (use --keep to stay up).
```

```sh
receiver$ achclwss-share receive Q4M8ZT
Looking for Q4M8ZT...
Receiving dataset.zip from sender...
  ↓ dataset.zip (412.0 MB)
Saved 1 file(s), 412.0 MB to /home/you/dataset.zip.
```

The sender stops by itself once a `receive` is done. A browser can open the sender's link too (it shows just that one file); a browser download doesn't stop the sender, so press Ctrl+C when they have it. Sending a folder works the same and recreates it, subfolders and all. `receive --to DIR` saves somewhere other than the current folder; existing files with the same names are overwritten.

**Push style**: the receiver waits, the sender pushes into it.

```sh
receiver$ achclwss-share receive --to ~/incoming
Receiving into /home/you/incoming (read-write). Press Ctrl+C to stop.
  ...
  Pairing code:       54JXUG   (on the other machine: achclwss-share send <path> 54JXUG)
```

```sh
sender$ achclwss-share send demo/photos 54JXUG
Sending demo/photos to incoming on receiver...
  ↑ photos/cat.jpg (2.9 KB)
Sent 1 file(s), 2.9 KB.
```

The receiver logs the upload and exits. Any `serve --write` share also accepts `send PATH CODE`.

## 5. Find shares nearby

Running shares announce themselves on the LAN every two seconds (a small UDP broadcast on port 8741 that contains the machine name, folder name and port, never the token or code). List them:

```sh
$ achclwss-share peers
Listening for shares on this network for 3s...
  myapp                desktop      sync     http://192.168.1.20:8770  (writable)
  holiday              laptop       share    http://192.168.1.31:8740
Join one with its pairing code (shown where it was started), e.g. `achclwss-share sync myproj CODE`.
```

`--wait 10` listens longer. Joining by code (`sync FOLDER CODE`, `receive CODE`, `send PATH CODE`) listens for up to 8 seconds and tries the code against each share it hears; only the right one accepts it. `--no-announce` keeps a share quiet (then people need the link).

## 6. Share over the internet

Add `--internet` to any `serve`, `sync`, `send` or `receive`. There's no ACHCLWSS relay server, so the tool uses what your machine has, in this order:

1. **cloudflared** on your `PATH`: starts a free Cloudflare quick tunnel and prints its `https://` link. No account, no router setup, HTTPS from the visitor to Cloudflare.

   ```sh
   $ achclwss-share serve ~/Projects/site --internet
   ...
     Opening an internet route...
     Over the internet:  https://sample-words-here.trycloudflare.com/?k=y2sgq6nkkfkpi5j5dvdeztifwfxcettt
                         via cloudflared quick tunnel (https)
   ```

   The QR code then encodes the internet link. Install: `brew install cloudflared`, `winget install Cloudflare.cloudflared`, or the `.deb`/`.rpm` from Cloudflare's site. The tunnel runs as long as the share does. A quick tunnel's link is random and changes each run.

2. **tailscale** installed and logged in (`tailscale up`): prints your tailnet address. Only devices on your tailnet can reach it, and traffic is end-to-end encrypted by WireGuard.

   ```
     Over the internet:  http://100.101.102.103:8782/?k=gacekqnkmol52mcu4wdxplqxt3up7ctv
                         via tailscale (encrypted tailnet, only your devices)
   ```

   This is the best option for **live sync over the internet**: run `sync` as usual and join with the tailscale link. (The sync, send and receive commands speak plain HTTP only, so they can't use a cloudflared `https://` link; browsers can.)

3. Neither: you get one line of advice instead of a link:

   ```
     Internet: no internet route found: install cloudflared (free quick tunnel, https) or tailscale (private tailnet), or forward TCP port 8780 on your router and share http://<public-ip>:8780/?k=<token>
   ```

   Forwarding a port works but sends plain HTTP across the internet; prefer a tunnel.

Whichever route is used, **the token is still required**: the internet link carries it, and without it visitors get the pairing page, where wrong guesses are rate-limited (section 8). With `--local --internet`, the share is reachable only through the cloudflared tunnel, not from the LAN.

## 7. Drive it from Claude (MCP)

`achclwss-share mcp` is an MCP server on stdio. Shares and syncs Claude starts run on background threads inside that server, and stop when Claude calls the matching stop tool or the session ends.

### Connect

Claude Code:

```sh
claude mcp add achclwss-share -- /path/to/achclwss/share/target/release/achclwss-share mcp
```

Claude Desktop: run `hub mcp-config --bin-dir /path/to/bin` (where `achclwss-share` lives next to the other binaries) and paste the output, which now has an `achclwss-share` entry next to `achclwss`; or add it by hand:

```json
{
  "mcpServers": {
    "achclwss-share": {
      "command": "/path/to/achclwss-share",
      "args": ["mcp"]
    }
  }
}
```

### The tools

| tool | what it does |
| --- | --- |
| `share_start` | share a folder or file (`folder`, `write`, `internet`, `port`, `local`); returns the link, LAN links, code, token and internet link |
| `share_stop` | stop one share (`id`) or all |
| `share_status` | running shares and syncs, with links, codes and traffic counters |
| `share_peers` | shares announced on the LAN (`seconds`) |
| `sync_start` | `folder` alone hosts it for sync; with `peer` (a code or link) joins one; `include_heavy` |
| `sync_status` | each sync's state, file count, uploads, downloads, conflicts, last error and recent changes |
| `sync_stop` | stop one sync (`id`) or all |
| `send_file` | `path` alone serves it for download; with `to` (a code or link) uploads it into a writable share |
| `list_remote` | list a folder of another share (`url` is a link or code, `path`) |
| `fetch_remote` | download a file or folder from another share into `dest` |

Every result has a readable text part and `structuredContent` with the same facts as fields (`url`, `code`, `token`, `stats`...), so Claude can pass a link on or check a counter.

### Example prompts

> Share my ~/Pictures/holiday folder so I can grab it on my phone.

Claude calls `share_start` with that folder and replies with the link and the code. (Over MCP there's no terminal QR; ask Claude for the link and open it on the phone, or type the code.)

> I'm on my laptop. Keep ~/code/myapp in sync with my desktop; the desktop shows code EYCZPJ.

Claude calls `sync_start` with `folder` and `peer: "EYCZPJ"`, then `sync_status` to confirm it's in sync and how many files came across.

> Host this repo for syncing and tell me what to type on the other machine.

`sync_start` with just `folder`; Claude passes on the `achclwss-share sync <folder> CODE` line from the result.

> What's being shared right now? Stop everything.

`share_status`, then `share_stop` and `sync_stop`.

> There's a share on the network from my desktop. List it and download the `exports` folder here.

`share_peers` to find it; you give Claude the code (or link), then `list_remote` and `fetch_remote` with `path: "exports"`.

> Send build/app.apk to the phone-test laptop, which is waiting with code 54JXUG.

`send_file` with `path` and `to: "54JXUG"`.

Links and tokens are secrets: Claude sees them in tool results, so they end up in the conversation. Stop shares you no longer need.

## 8. Security

This tool opens a port on your machine, so here is exactly what protects it.

**Every request needs the share's secret token.** It's 160 random bits from the operating system's generator (`/dev/urandom`; `BCryptGenRandom` on Windows), printed as 32 characters in the link (`?k=...`). The first visit swaps it for a cookie (`HttpOnly`, `SameSite=Strict`) and drops it from the address bar; scripts send it as `Authorization: Bearer TOKEN`. Comparisons are constant-time. A new token is made every time a share starts, so old links die with the share.

**The 6-character pairing code** (about 30 bits) is only accepted by the pairing endpoint, which trades it for the token. Each address gets 5 wrong codes per minute before it is blocked for 5 minutes, and after 30 wrong codes in total pairing switches itself off for that share (the link still works). An attacker's chance of guessing it is around one in thirty million. Separately, an address that presents 10 wrong tokens in a minute is blocked for 5 minutes.

**Read-only unless you say `--write`.** A write needs the token too, and a browser write also needs a custom header that cross-site pages can't send (so a malicious website you visit can't use your cookie to upload or delete).

**Paths can't escape the shared folder.** Every path from the network is checked: no `..`, no absolute paths, no drive letters (`C:`), no UNC paths (`\\server`), no backslashes, no NUL bytes, no Windows device names (`CON`, `NUL`, `COM1`...) or alternate data streams (`file:stream`). After resolving, the real location (symlinks followed) must still be inside the share, so a symlink pointing outside is refused; writes never pass through symlinks at all. A sync peer or a remote share can't make *your* side write outside its folder either: every path it sends goes through the same checks.

**Shared files can't run code in the share's page.** Files are served with `Content-Security-Policy: sandbox` and `nosniff`, so an HTML or SVG file in the share opens without scripts and can't reach the API.

**What isn't protected:**

- **LAN traffic is plain HTTP.** Someone who can watch your network traffic (an open café Wi-Fi, a compromised router) can see the files and the token. Use it on networks you trust, or over tailscale (encrypted) or a cloudflared tunnel (HTTPS to Cloudflare).
- **Anyone with the link or code has access** until you stop the share. Don't post links publicly; stop shares when done.
- **It listens on all interfaces by default** (`0.0.0.0`), so any network the machine is on (including VPNs) can reach the port; the token is what keeps them out. Use `--local` or `--bind IP` to narrow it.
- A share started with `--write` lets anyone with the link delete or overwrite files in that folder. Share a dedicated folder, not your project's only copy.

## 9. Troubleshooting

**"On another device" link doesn't load from the phone.**

- Both devices must be on the same network. Guest Wi-Fi and some office or mesh networks isolate devices from each other ("client isolation"); try a different network or a phone hotspot.
- **Firewall.** Windows shows a "Windows Defender Firewall has blocked some features" prompt the first time; tick **Private networks** and allow it. If you clicked Cancel, allow it later under *Windows Security > Firewall & network protection > Allow an app through firewall*, or run (as admin) `netsh advfirewall firewall add rule name="achclwss-share" dir=in action=allow program="C:\path\to\achclwss-share.exe"`. macOS asks "Do you want the application to accept incoming network connections?": click **Allow** (or later in *System Settings > Network > Firewall > Options*). On Linux with ufw: `sudo ufw allow 8740:8760/tcp` and `sudo ufw allow 8741/udp`.
- The printed LAN address is the interface that routes to the internet. On machines with several networks (VPN, Docker, virtual machines) it may be the wrong one; find the right address with `ip addr` / `ipconfig` / System Settings and use `http://THAT-IP:PORT/?k=TOKEN` (the same token), or `--bind THAT-IP`.

**"no share answered on this network within 8s" when joining by code.** Discovery uses UDP broadcast, which doesn't cross subnets or routers and is blocked by some Wi-Fi setups and firewalls (allow UDP 8741 inbound on the joining machine). Use the full link instead of the code: it works across subnets wherever TCP gets through. `achclwss-share peers` shows whether announcements arrive at all.

**"found 1 share(s) nearby but none accepted code ..."** A typo in the code, or the share was restarted (a new code each time). Too many wrong tries blocks you for five minutes ("too many failed attempts").

**"isn't a pairing code".** Codes use the digits 2 to 9 and the letters A to Z without I, L and O; you probably read an O as a 0 or similar.

**"wrong or expired share token".** The link is from an earlier run of the share; each run has a new token. Get the new link.

**Port already in use.** Without `--port` the next free port is used automatically. With `--port N` pick another number. Only one `peers`/join can listen for announcements at a time on one machine.

**Sync says "sync paused: ... (retrying)".** The other machine went away (sleep, network change). It keeps retrying every second and catches up when it's back; nothing needs restarting.

**A file keeps coming back after I delete it.** The other side changed it after your delete; an edit beats a delete. Delete it again.

**I see `name.conflict-<machine>.ext` files.** Both machines changed that file; see [Conflicts](#conflicts).

**A big folder (or `node_modules`) isn't syncing.** It's in the heavy list or matched by `.gitignore`/`.shareignore`; `--include-heavy` or a `!` rule in `.shareignore` brings it in.

**Garbled QR code on Windows.** The tool enables ANSI colours in the console automatically; very old consoles may still not show it. Use the link or the code, or `--no-qr`.

## 10. Command reference

```
achclwss-share                          share the current folder (read-only)
achclwss-share serve [FOLDER]           share a folder; the other side just needs a browser
achclwss-share sync FOLDER              host FOLDER for live two-way sync; prints a code
achclwss-share sync FOLDER CODE|LINK    join a sync host and keep FOLDER in sync with it
achclwss-share send PATH                offer a file or folder for download; prints a code
achclwss-share send PATH CODE|LINK      upload a file or folder into a waiting `receive`
achclwss-share receive CODE|LINK        download what a `send` is offering
achclwss-share receive                  wait for a `send PATH CODE`; files land in --to DIR
achclwss-share peers                    list shares announced on this network
achclwss-share mcp                      MCP server on stdio, for Claude

--write            allow uploads, renames and deletes from the browser (serve)
--internet         also reach it from outside the LAN: cloudflared tunnel, else tailscale
--local            listen on 127.0.0.1 only (default: all interfaces, i.e. 0.0.0.0)
--bind IP          listen on one address
--port N           TCP port (default 8740, or the next free one)
--max-upload MB    largest upload accepted (default 4096)
--include-heavy    also sync .git, node_modules, target, build, .venv, __pycache__
--code CODE        pairing code for a link without ?k= (sync/receive/send)
--to DIR           where `receive` saves (default: current folder)
--keep             `send`/`receive`: keep running after the first transfer
--no-qr            don't print the QR code
--no-announce      don't announce on the LAN (joining by code then needs the link)
--wait SECONDS     how long `peers` listens (default 3)
```

`hub share ARGS...` runs the same commands when `achclwss-share` sits next to `hub`.

### The HTTP API

Everything the web page does is plain HTTP, so `curl` works (send the token as a bearer header):

```sh
T=eu23xrfuios6eax2zn43z4xnum6cg3ie
curl -H "Authorization: Bearer $T" http://192.168.1.20:8740/api/list?path=photos     # JSON listing
curl -H "Authorization: Bearer $T" -O http://192.168.1.20:8740/f/photos/cat.jpg      # download
curl -H "Authorization: Bearer $T" -o all.zip http://192.168.1.20:8740/zip/          # whole share as zip
curl -H "Authorization: Bearer $T" -T notes.txt http://192.168.1.20:8740/f/inbox/notes.txt   # upload (--write)
curl -H "Authorization: Bearer $T" -X DELETE http://192.168.1.20:8740/f/inbox/notes.txt     # delete (--write)
```

Other endpoints: `GET /api/info`, `GET /api/tree?path=` (every file below a folder), `GET /api/hash?path=` (SHA-256), `POST /api/mkdir?path=`, `POST /api/move?from=&to=`, `POST /api/pair` (body `{"code":"..."}`), and `GET /api/sync/manifest?since=&wait=` (what sync uses).

# Connecting an AI (optional)

wetwaresuite works completely on its own: every tool is a command you can run, script and batch with no AI and no internet. If you *want* an AI assistant like Claude to use the tools for you, this page shows the easy ways to connect one. You can disconnect again at any time, and nothing else changes.

There are three ways, from easiest to most flexible:

1. **`hub connect`**: one command that adds wetwaresuite to Claude Desktop and Claude Code for you.
2. **By hand**: paste a few lines into a config file (see [mcp.md](mcp.md)).
3. **Over HTTP**: run `wetwaresuite mcp --http` so an AI client on another computer, in a container, or one that only speaks HTTP can use it.

## 1. The one-command way: `hub connect`

`hub` is the small install manager that comes with wetwaresuite. Put it in the same folder as the `wetwaresuite` program (that's how releases and `hub install` lay things out), then run:

```sh
hub connect
```

It looks for Claude Desktop and Claude Code on your computer, and adds wetwaresuite to each one it finds:

```
Skipping wetwaresuite-share: it isn't in /home/sam/wetwaresuite (build or install it, then run this again).
Claude Desktop: added wetwaresuite (/home/sam/.config/Claude/claude_desktop_config.json).
  Quit and reopen Claude Desktop to load the tools.
Claude Code: saved a backup as /home/sam/.claude.json.backup-1792146708.
Claude Code: added wetwaresuite (/home/sam/.claude.json).
  Quit and reopen Claude Code to load the tools.
```

Then **quit and reopen** the app. In Claude Desktop, the tools appear under the tools/connectors button in the chat box; in Claude Code, type `/mcp` to see them.

What it does, exactly:

- It finds the config files here:

  | App | Windows | macOS | Linux |
  | --- | --- | --- | --- |
  | Claude Desktop | `%APPDATA%\Claude\claude_desktop_config.json` | `~/Library/Application Support/Claude/claude_desktop_config.json` | `~/.config/Claude/claude_desktop_config.json` |
  | Claude Code | `%USERPROFILE%\.claude.json` | `~/.claude.json` | `~/.claude.json` |

  (Claude Code uses `$CLAUDE_CONFIG_DIR/.claude.json` if you set that variable.)
- It **saves a backup** of each file before changing it, next to it, named `...json.backup-<number>`. To undo by hand, copy the backup back.
- It only changes the `mcpServers` section, and only the `wetwaresuite` and `wetwaresuite-share` entries in it. Your other settings and other MCP servers are kept as they are.
- Running it again is safe. If everything is already set up it says `already connected` and changes nothing. If you moved the programs, it updates the paths.
- If Claude Code is installed but has never been started, its file doesn't exist yet; start it once and run `hub connect` again.
- If a config file isn't valid JSON (for example you edited it and left a comma), it leaves that file alone and tells you.

Options:

```sh
hub connect --dry-run           # show what would change, change nothing
hub connect --remove            # take wetwaresuite out again (with a backup)
hub connect --desktop           # only Claude Desktop
hub connect --code              # only Claude Code
hub connect --bin-dir D:/apps/wetwaresuite     # the programs are somewhere else
hub connect --config path/to/mcp.json          # edit any MCP config file with the same layout
```

`hub mcp-config` still prints the settings without changing anything, if you'd rather paste them in yourself.

## 2. By hand

See [mcp.md](mcp.md), sections 2 and 3. In short, for Claude Code:

```sh
claude mcp add wetwaresuite -- /full/path/to/wetwaresuite mcp
```

and for Claude Desktop, add this to its config file and restart it:

```json
{
  "mcpServers": {
    "wetwaresuite": { "command": "/full/path/to/wetwaresuite", "args": ["mcp"] }
  }
}
```

## 3. Over HTTP (remote MCP)

Normally the AI app starts `wetwaresuite mcp` itself and talks to it through a pipe. That's the simplest and most private way. Sometimes you want the suite to run as a small server instead:

- the files and the suite are on another computer (a desktop at home, a server, a NAS),
- you run it in Docker (see [docker.md](docker.md)),
- your AI client only supports MCP over HTTP.

Start it:

```sh
wetwaresuite mcp --http
```

```
MCP server listening on http://127.0.0.1:8765/mcp
connect Claude Code with:
  claude mcp add --transport http wetwaresuite http://127.0.0.1:8765/mcp
press Ctrl+C to stop
```

and connect Claude Code with the command it prints. It keeps running until you press Ctrl+C.

The options:

| Option | What it does |
| --- | --- |
| `--http` | serve over HTTP instead of stdin/stdout |
| `--port N` | the port (default 8765). `--port 0` picks any free port and prints it |
| `--host H` | the address to listen on. The default, `127.0.0.1`, means **only this computer** can connect. `0.0.0.0` means every network this computer is on |
| `--token T` | require every request to carry `Authorization: Bearer T`. Also read from the `WETWARESUITE_TOKEN` environment variable |
| `--tools a,b` | serve only some tools, exactly as with stdio |

### Letting another computer connect

```sh
wetwaresuite mcp --http --host 0.0.0.0
```

```
MCP server listening on http://<this-computer's-address>:8765/mcp
token (made up for this run; pass --token or set WETWARESUITE_TOKEN to keep one): 3f9c...
connect Claude Code with:
  claude mcp add --transport http wetwaresuite http://<this-computer's-address>:8765/mcp --header "Authorization: Bearer 3f9c..."
press Ctrl+C to stop
```

Because anyone who can reach that address could otherwise open and change your files, the server **refuses to listen on anything but this computer without a token**. If you don't give one, it makes one up and prints it. Replace `<this-computer's-address>` with the computer's local IP address (Windows: `ipconfig`; macOS: System Settings > Network; Linux: `hostname -I`).

To keep the same token between runs, choose one and pass it:

```sh
wetwaresuite mcp --http --host 0.0.0.0 --token "a-long-random-secret"
# or
WETWARESUITE_TOKEN="a-long-random-secret" wetwaresuite mcp --http --host 0.0.0.0
```

On the other computer:

```sh
claude mcp add --transport http wetwaresuite http://192.168.1.20:8765/mcp --header "Authorization: Bearer a-long-random-secret"
```

Any MCP client that supports the "Streamable HTTP" transport works the same way: give it the URL ending in `/mcp` and the `Authorization` header.

### Safety notes

- The connection is plain HTTP, not HTTPS. That's fine on your own computer or home network. To use it across the internet, put it behind something that adds encryption, such as a reverse proxy (Caddy, nginx) or a private network (Tailscale, WireGuard), rather than opening the port on your router.
- Paths in commands are paths on the computer running the server, and the AI can read and write anything that computer's user can. Run it as a user, or in a container, that only sees the folders you want to share.
- With no token, the server only answers requests addressed to `localhost`/`127.0.0.1`, and refuses requests from web pages on other sites, so a website you visit can't use it through your browser.
- Requests are limited in size (16 MB) and a connection that goes quiet is dropped, so one stuck client can't block it for long.

### Checking it by hand

With `curl` you can talk to it directly:

```sh
curl -s http://127.0.0.1:8765/mcp -H 'Content-Type: application/json' \
  -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"convert_formats"}}'
```

The answer is JSON, the same as over stdio. A `GET` of `/mcp` answers `405` (this server doesn't push messages on its own, which the protocol allows), and a missing or wrong token answers `401`.

## Which should I use?

- One computer, Claude Desktop or Claude Code: **`hub connect`**. Done.
- Files on another computer, or a container: **`--http` with a token**.
- No AI at all: skip this page. Everything works without it; see [getting-started.md](getting-started.md) and [headless-and-batch.md](headless-and-batch.md).

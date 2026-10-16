# Running wetwaresuite in Docker (optional)

You never need Docker for wetwaresuite. The normal programs run directly on Windows, macOS and Linux. Docker is just one more option, handy when you want to:

- run the suite on a home server or NAS and let an AI client on another computer use it,
- keep it in a sealed box that can only see one folder,
- try it without building anything on your own machine.

The image contains the `wetwaresuite` program (every tool), `hub` and `wetwaresuite-share`, running as an ordinary (non-root) user. Your files go in a folder that is mounted at `/work` inside the container. The image never connects to the internet.

## 1. Build the image

You need [Docker](https://docs.docker.com/get-docker/) installed. From the wetwaresuite source folder:

```sh
docker build -t wetwaresuite .
```

This compiles everything inside the container. It takes a few minutes the first time. Building downloads the Debian and Rust base images and the pinned nlohmann/json source once; after that, nothing in the image goes online.

## 2. Use it like the normal program

Run any command, with your current folder mounted as `/work`:

```sh
docker run --rm -v "$PWD:/work" wetwaresuite wetwaresuite --version
docker run --rm -v "$PWD:/work" wetwaresuite wetwaresuite list photo
docker run --rm -v "$PWD:/work" wetwaresuite wetwaresuite convert.file input=photo.png output=photo.jpg
```

On Windows PowerShell, write `-v "${PWD}:/work"`; in the old Command Prompt, `-v "%cd%:/work"`.

Batch scripts work too, fed through standard input (note the `-i`):

```sh
docker run --rm -i -v "$PWD:/work" wetwaresuite wetwaresuite batch < resize.txt
```

Paths inside commands are paths **inside the container**, so files in your current folder are just `photo.png` (or `/work/photo.png`).

## 3. Serve it to an AI client over HTTP

With no command, the container starts the MCP server over HTTP on port 8765, listening on all of the container's addresses and requiring a token:

```sh
docker run --rm -p 127.0.0.1:8765:8765 -v "$PWD:/work" -e WETWARESUITE_TOKEN=pick-a-long-random-secret wetwaresuite
```

You will see:

```
MCP server listening on http://<this-computer's-address>:8765/mcp
clients must send: Authorization: Bearer <your token>
connect Claude Code with:
  claude mcp add --transport http wetwaresuite http://<this-computer's-address>:8765/mcp --header "Authorization: Bearer <your token>"
press Ctrl+C to stop
```

Then connect a client that speaks MCP over HTTP. For Claude Code on the same computer:

```sh
claude mcp add --transport http wetwaresuite http://127.0.0.1:8765/mcp --header "Authorization: Bearer pick-a-long-random-secret"
```

Things to know:

- `-p 127.0.0.1:8765:8765` publishes the port **only to this computer**. To let other computers on your network reach it, use `-p 8765:8765`, and give them the token. Anyone with the token can read and change every file in the mounted folder, so keep it secret.
- If you leave out `-e WETWARESUITE_TOKEN=...`, the server makes up a token and prints it when it starts (`docker logs <container>` shows it again if you started it with `-d`). It changes every time the container starts, so set your own for anything long-lived.
- The token is checked on every request. Requests without it get `401 Unauthorized`.
- The connection is plain HTTP. On your own computer or home network that is fine. Across the internet, put it behind something that adds HTTPS (a reverse proxy such as Caddy, or a private network such as Tailscale).
- Only the mounted folder is visible. Mount more with extra `-v` options, for example `-v "$HOME/Pictures:/work/pictures"`.

To serve only some tools, pass the command yourself:

```sh
docker run --rm -p 127.0.0.1:8765:8765 -v "$PWD:/work" -e WETWARESUITE_TOKEN=secret wetwaresuite \
  wetwaresuite mcp --tools photo,paint --http --host 0.0.0.0
```

(`--host 0.0.0.0` matters inside a container: the default, 127.0.0.1, would be the container's own loopback and unreachable from outside.)

## 4. Keep it running

```sh
docker run -d --name wetwaresuite --restart unless-stopped \
  -p 127.0.0.1:8765:8765 -v "$HOME/wetwaresuite-files:/work" \
  -e WETWARESUITE_TOKEN=pick-a-long-random-secret wetwaresuite
docker logs wetwaresuite     # see what it printed
docker stop wetwaresuite     # stop it (it shuts down cleanly)
```

## 5. Use the hub and share

Both are in the image:

```sh
docker run --rm wetwaresuite hub list
docker run --rm -it --network host -v "$PWD:/work" wetwaresuite wetwaresuite-share serve /work
```

`share` finds other devices with a LAN broadcast, which needs `--network host` (Linux only). See [share.md](share.md).

## Troubleshooting

- **`permission denied` writing files.** The container runs as user id 10001. Either make the mounted folder writable for it (`chmod o+w folder`), or run as yourself: add `--user "$(id -u):$(id -g)"`.
- **The client can't connect.** Check the container is running (`docker ps`), the port is published (`-p ...:8765:8765`), and the client sends `Authorization: Bearer <token>`. `curl -H "Authorization: Bearer <token>" http://127.0.0.1:8765/` should answer with a line of text.
- **Video and office conversion fail.** The image is kept small and leaves out ffmpeg and LibreOffice. Make your own image on top: a `Dockerfile` with `FROM wetwaresuite`, `USER root`, `RUN apt-get update && apt-get install -y --no-install-recommends ffmpeg`, `USER wetware`.

See also: [ai-connect.md](ai-connect.md) for every way to connect an AI, and [offline.md](offline.md) for what touches the network.

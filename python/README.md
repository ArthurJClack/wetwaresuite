# wetwaresuite for Python

Drive the wetwaresuite suite from Python 3.8 or newer. Pure Python, no dependencies: it starts `wetwaresuite mcp` and talks to it over stdin/stdout, so every command of every tool in your build is a method, with no wrapper code to keep up to date.

```python
import wetwaresuite

with wetwaresuite.Suite() as suite:
    img = suite.photo.open(path="cat.jpg").doc         # "img1"
    r = suite.photo.resize(doc=img, width=800, preview=True)
    print(r.message, r["width"], r["height"])          # resized to 800x600 800 600
    r.save_image("preview.png")                        # previews arrive as PNG bytes
    suite.photo.save(doc=img, path="cat_small.jpg")

    for hit in suite.library.search(query="type:image width>=3840")["items"]:
        print(hit["path"])

    suite.automate.pipeline(path="scripts/web.yaml", vars={"file": "cat.jpg", "out": "web"})
```

## Install

```sh
pip install ./python            # from the repository root
# or, without installing:
export PYTHONPATH=/path/to/wetwaresuite/python
```

The client needs the `wetwaresuite` binary. It looks at `Suite(binary=...)`, then `$WETWARESUITE_BIN`, then `PATH`, then the repository's `build/apps/wetwaresuite/` folder.

## Reference

**`Suite(binary=None, tools=None, raise_errors=True, timeout=None, cwd=None, env=None, stderr=None)`** starts one suite process. Use it as a context manager or call `close()`. `tools=["photo", "convert"]` serves only those tools; `timeout` is seconds per command; `cwd` is the folder relative paths mean.

- `suite.<tool>.<command>(**params)`: `suite.photo.resize(doc="img1", width=800)`. A single positional argument fills `doc`. The method's docstring is the command's description and parameter list (`help(suite.photo.resize)`).
- `suite.call("photo.resize", doc="img1", width=800)`: the same by id; `suite.command(id)` returns the method.
- `suite.commands()`: every command id; `suite.tools()`: tools by name.
- `suite.undo()`, `suite.redo()`, `suite.history()`, `suite.documents()`: the session's undo history and open documents (documents stay open between calls).

Arguments: `pathlib.Path` becomes a string, a list becomes a number array or a comma list (whichever the parameter takes), a dict becomes JSON (`vars={"out": "web"}`). Commands that have a `json` switch get `json=True` automatically, so lists such as search results come back as Python data.

**`Result`** is what every command returns:

| attribute | meaning |
| --- | --- |
| `ok` | did it succeed (also `bool(result)`) |
| `message` | the message a person would read |
| `text` | the message plus every field, as the command line prints it |
| `data` | the fields as a dict; `result["width"]`, `result.get("x")` and `"x" in result` work too; JSON list fields are decoded |
| `doc` | the document id the command made or changed, if any |
| `images`, `image` | preview images as PNG bytes (all, or the first) |
| `save_image(path, index=0)` | write a preview to a file |
| `attachments` | every attached file as `(mime, bytes, name)` |

With `raise_errors=True` (the default) a failed command raises `CommandError`, whose `result` holds the failed Result; with `raise_errors=False` it returns the Result with `ok == False`. `WetwaresuiteError` covers a binary that can't be found, starts badly or exits.

**`run_batch(text, binary=None, keep_going=False, check=True, cwd=None, timeout=None)`** runs batch text (one command per line, `-> name` captures, `$name` reuse) in a fresh `wetwaresuite batch` process and returns a `BatchResult` with `ok`, `returncode`, `output` and `errors`. It raises `WetwaresuiteError` when a line fails, unless `check=False`.

**`find_binary(binary=None)`** returns the path the client would use.

## Examples

A background watch folder while Python carries on:

```python
with wetwaresuite.Suite() as suite:
    w = suite.automate.watch_start(folder="drop", pipeline="scripts/web.yaml", pattern="*.png,*.jpg")
    ...  # drop files in; each one runs the pipeline in its own session
    print(suite.automate.watch_status().message)
    suite.automate.watch_stop(id=w["id"])
```

Tolerating failures:

```python
with wetwaresuite.Suite(raise_errors=False) as suite:
    for path in ["a.png", "missing.png"]:
        r = suite.photo.open(path=path)
        print(path, "ok" if r else "failed: " + r.message)
```

More in [docs/tutorials/automate.md](../docs/tutorials/automate.md).

## Tests

```sh
cd python && python3 -m unittest discover -s tests -t .
```

The tests that need the suite run against `$WETWARESUITE_BIN` (or the binary found as above) and are skipped when there is none. CMake runs them as the `automate_python` test when it finds Python 3.

## Notes

- One `Suite` is one process, and its commands run one at a time; use several `Suite`s for parallel work.
- Background watches and open documents live as long as the `Suite`.
- The client follows the server's tool list, so it works with any build: tools that weren't built simply aren't there.

License: GPL-3.0, like the suite.

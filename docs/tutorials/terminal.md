# Terminal: command lines inside the suite

`terminal` runs your system's shell inside the suite. On Linux and macOS that is your login shell (bash, zsh, sh, dash or ksh), and pwsh too if it is installed. On Windows it is cmd by default, with PowerShell, pwsh and Git Bash's bash to choose from.

Each terminal:

- remembers its folder and exported variables from one command to the next;
- keeps a scrollback and a history of commands with their exit codes;
- finishes file names with Tab;
- can stop a running command, like Ctrl+C;
- lets you type answers into a program that asks questions.

Scripts and Claude drive the same terminals with the same commands. `terminal.exec` runs a single command line without keeping a terminal open.

`terminal` is part of the system app (`wetwaresuite-system`) and the all-in-one `wetwaresuite`. Nothing goes online unless the commands you type do. Each task is shown in the window, on the command line, in batch scripts (see [headless and batch](headless-and-batch.md)) and from Claude (see [mcp.md](mcp.md)). Outputs are copied from real runs with bash on Linux.

## The Terminal window

Start the window with `wetwaresuite-system` and click **Open** on the **Terminal** card. You can also click **Terminal here** in the [Files](explorer.md) panel to open one in the folder you are looking at.

- **New terminal** opens another tab. Each tab is named after its folder, and shows **(busy)** while a command runs. Close a tab with its x.
- In the advanced view (View > Show advanced options), a **Shell** picker next to it chooses which shell new terminals use.
- **The prompt** at the bottom shows the folder. Type a command and press **Enter**.
  - **Up** and **Down** bring back earlier commands.
  - **Tab** finishes a file or folder name. When several names fit, they are all listed above the prompt.
- **While a command runs**, the prompt changes to `input >`. What you type there goes to the program, as answers to its questions.
  - **Stop**, or **Ctrl+C** with an empty prompt, stops the command and everything it started.
- **Clear** empties the scrollback. The folder, variables and history stay.
- The output uses a monospace font and scrolls to the bottom as it arrives.

## Task 1: one command, no terminal

```
$ wetwaresuite terminal.exec command="ls | wc -l" folder=/tmp/demo
12

output: 12
exit_code: 0
cwd: /tmp/demo
timed_out: false
```

`terminal.exec` takes these options:

- `folder=` sets where it runs (by default, your home folder);
- `shell=` picks a shell;
- `timeout=` stops the command after that many seconds (default 60).

This is the simplest thing to use from scripts.

`terminal.shells` lists the shells it found. The first one is the default:

```
$ wetwaresuite terminal.shells
bash (/usr/bin/bash)
sh (/usr/bin/sh)
dash (/usr/bin/dash)
```

## Task 2: a terminal that remembers

```
# term.txt: a terminal that keeps its folder and variables between commands
terminal.open folder=/tmp/demo -> t
terminal.run doc=$t command="cd shop/src && export GREETING=hello"
terminal.run doc=$t command="pwd; echo $GREETING; ls | sort"
terminal.complete doc=$t line="cat ca"
terminal.run doc=$t command="python3 -c 'n = input(\"name? \"); print(\"hi\", n)'" wait=false
terminal.send doc=$t text=Sam wait=1
terminal.run doc=$t command="sleep 30" wait=false
terminal.interrupt doc=$t
terminal.read doc=$t wait=2
terminal.run doc=$t command="false || echo failed with $?"
terminal.history doc=$t
terminal.close doc=$t
```

```
$ wetwaresuite batch term.txt
opened term1 (bash in /tmp/demo, 0 commands)

cwd: /tmp/demo/shop/src

/tmp/demo/shop/src
hello
__pycache__
bug.py
cart.py
report.py

cart.py
common: cart.py

started
src $ python3 -c 'n = input("name? "); print("hi", n)'
Sam
name? hi Sam

started
stopped
exit_code: 130
src $ sleep 30
[stopped]

failed with 1

1  cd shop/src && export GREETING=hello
2  pwd; echo $GREETING; ls | sort
3  python3 -c 'n = input("name? "); print("hi", n)'
4  sleep 30  (exit 130)
5  false || echo failed with $?

closed term1
```

How the commands behave:

- **`terminal.run`** runs a command line as if typed at the prompt. Pipes, `&&`, `;`, `cd`, `export` and the shell's own commands all work.
  - **Waiting.** It waits for the command to finish (up to `timeout`, 30 seconds by default) and returns the output and exit code. Anything that takes longer keeps running, and so does anything run with `wait=false`.
  - **Following along.** `terminal.read` gives the new output and says whether the command is still `running`. `wait=` waits up to that many seconds for it to finish, and `all=true` returns the whole scrollback.
- **`terminal.send`** types into the running command, then presses Enter (unless you pass `newline=false`). `end=true` closes its input, like Ctrl+D.
- **`input=`** on `terminal.run` gives a command all of its input at once.
- **`terminal.interrupt`** stops the command and everything it started, like Ctrl+C. The exit code is 130.
- **`terminal.complete`** returns the names that finish the last word of a command line, and `common`, the part they all share. This is what Tab does in the window.
- **Batch variables.** In a batch script, `$t` is a capture. Shell variables like `$GREETING` and `$?`, which are not captures, are passed to the shell unchanged.

## Claude

```sh
claude mcp add wetwaresuite-terminal -s user -- wetwaresuite-system mcp --tools terminal
```

Example prompts and the tools Claude calls:

- *"What's my IP address and which ports are listening?"*: `terminal_exec` with `ip -brief addr`, then with `ss -tlnp`. On Windows it uses `ipconfig` and `netstat -ano`.
- *"Start the dev server in ~/site and tell me when it's up"*: `terminal_open`, then `terminal_run` with `npm run dev` and `wait=false`, then `terminal_read` until the output says it is listening.
- *"Run the installer and answer yes to everything"*: `terminal_run` with `wait=false`, then `terminal_send text=y` for each question.

Claude's terminals show up as tabs in the window, so you can watch what it runs and stop it.

## Parameter reference

| Command | Main parameters |
|---|---|
| `terminal.exec` | `command`, `folder`, `shell`, `timeout` |
| `terminal.open`, `terminal.close` | `folder`, `shell`, `doc` |
| `terminal.run` | `doc`, `command`, `wait`, `timeout`, `input` |
| `terminal.read` | `doc`, `all`, `wait` |
| `terminal.send` | `doc`, `text`, `newline`, `end`, `wait` |
| `terminal.interrupt`, `terminal.clear` | `doc` |
| `terminal.complete` | `doc`, `line` |
| `terminal.history` | `doc`, `count` |
| `terminal.shells` | |

## Limitations

- **Not a full terminal.** A command's input and output are pipes, not a pseudo-terminal. Plain programs, build tools, package managers and scripts work. Full-screen programs (vim, nano, top, less, htop) and colour-hungry interfaces don't, and some programs print less when they see no terminal. For those, use your system's own terminal.
- **Passwords.** Password prompts that read straight from the terminal (sudo, ssh) can't be answered with `terminal.send`. Use `sudo -S` or keys instead.
- **Colours.** ANSI colours are removed from the output, not drawn.

# Monitor: what your computer is doing

`monitor` shows what your computer is doing:

- how busy the processor is, in total and per core;
- memory and swap;
- every disk and how full it is;
- network speed;
- the battery;
- every program that is running.

You can end a program that is stuck, and record a short chart of processor and memory use.

It reads the numbers straight from the operating system: `/proc` and `/sys` on Linux, the Windows system APIs, and `sysctl` and `libproc` on macOS. Nothing goes online, and nothing is installed. `monitor` is part of the system app (`wetwaresuite-system`) and the all-in-one `wetwaresuite`.

Each task is shown in the window, on the command line, in batch scripts (see [headless and batch](headless-and-batch.md)) and from Claude (see [mcp.md](mcp.md)). Outputs are copied from a real run on a 4-core Linux virtual machine.

## The window

Start the window with `wetwaresuite-system` and click **Open** on the **This computer** card (called **System monitor** in Advanced mode). To open it at the program list:

```sh
wetwaresuite-system gui --action monitor.processes
```

It updates about once a second while it is on screen. It has two tabs.

**Overview** shows:

- the system and how long it has been up;
- live charts of processor and memory use over the last two minutes;
- the processor model, with one "In use" meter in Simple mode, or a meter per core in Advanced mode;
- memory and swap;
- each disk with how much is free (Advanced mode adds the file system);
- the battery, when there is one;
- network connections. Simple mode lists the connections that are up, with download and upload speed. Advanced mode shows every interface with its totals.

**Programs** (called **Processes** in Advanced mode) lists every running program:

- **The columns.** It shows the program's name, processor and memory use. Advanced mode adds the PID, threads, user and full command line.
- **Finding a program.** Type in **find a program** to filter the list.
- **Sorting.** Pick **busiest first**, **most memory first**, **by name** or **by id**.
- **Ending a program.** Select a row and press **End program**. It asks before it does anything (**End it** / **Keep it running**). On Linux and macOS this asks the program to quit politely.
- **Advanced mode** adds **Force end**, which stops a program at once, and **Details**, which shows its parent, the programs it started, and its command line.

## Task 1: a quick look

```
$ wetwaresuite monitor.overview
Intel(R) Xeon(R) Processor @ 2.10GHz
4 threads on 4 cores, 2100 MHz
in use: [....................] 0%
  core 0   [..........]   0%
  core 1   [..........]   0%
  core 2   [..........]   0%
  core 3   [..........]   0%
load average 0.03, 0.12, 0.24
memory: [#...................] 4%  631 MB used of 16 GB, 15 GB free

/  [#...........] 9%  15 GB free of 252 GB (ext4)
/opt/claude-code  [###########.] 90%  25 MB free of 301 MB (ext4)
/opt/env-runner  [########....] 65%  16 MB free of 47 MB (ext4)

$ wetwaresuite monitor.system
Linux (Ubuntu 24.04.5 LTS)
Linux 6.18.44-fc-v139, x86_64
computer: vm
user: root
up for 2 h 20 min
```

Each part is also a command of its own: `monitor.cpu`, `monitor.memory`, `monitor.disks`, `monitor.network` and `monitor.battery`. Each one returns text for people and fields for scripts:

```
$ wetwaresuite monitor.memory
memory: [#...................] 4%  631 MB used of 16 GB, 15 GB free

percent: 3.92196
total: 16874930176
used: 661827584
available: 16213102592
swap_total: 0
swap_used: 0

$ wetwaresuite monitor.network
lo: down 0 B/s, up 0 B/s; 89 MB received, 89 MB sent
eth0: down 878 B/s, up 878 B/s; 188 MB received, 383 MB sent

$ wetwaresuite monitor.battery
no battery
present: false
```

Speeds are measured over a short moment, so a single command gives a real number, not zero.

## Task 2: find a program and end it

```
$ wetwaresuite monitor.processes sort=memory limit=5
    pid   cpu%    memory  thr  user         name
    113    0.0    378 MB   10  root         claude
    103    0.0     47 MB   10  root         environment-man
     84    0.0     33 MB   15  root         rclone-filestor
  24985    0.0    6.6 MB    1  root         wetwaresuite-sy
      1    0.0    5.5 MB    9  root         process_api
79 in all

$ wetwaresuite monitor.processes filter=sleep
    pid   cpu%    memory  thr  user         name
  25027    0.0    6.1 MB    1  root         wetwaresuite-sy
  25015    0.0    3.7 MB    1  root         bash
  25022    0.0    1.5 MB    1  root         sleep
  25023    0.0    1.5 MB    1  root         sleep

$ wetwaresuite monitor.process pid=25022
sleep (pid 25022)
command: sleep 601
user: root
processor: 0.0% (0 s used so far)
memory: 1.5 MB
threads: 1
parent: 1 process_api

$ wetwaresuite monitor.kill name=sleep
ended 2 of 2
```

The options:

- `filter=` matches the name or the command line. That is why the shell that started the `sleep` commands, and the monitor itself, show up above.
- `user=` keeps only one user's programs.
- `sort=` is `cpu`, `memory`, `name` or `pid`.
- The `rows` field has every column, including the full command line.

How `monitor.kill` works:

- **Which programs.** Give it a `pid=`, or a `name=` that must match exactly.
- **How it ends them.** On Linux and macOS it asks the program to quit (SIGTERM), and `force=true` ends it at once (SIGKILL). On Windows a program is always ended at once (TerminateProcess), so save your work in it first.
- **No undo.** A program that has ended can't be brought back, so the command says so in its description. Claude is told this too.

## Task 3: record a chart

```
$ wetwaresuite monitor.watch seconds=3 interval=1 output=usage.png
processor: average 1%, peak 1%; memory 4% (3 samples)
cpu: 0.5,0.5,0.501253
memory: 3.84324,3.84324,3.84324
```

This samples processor and memory use for `seconds` (5 by default), every `interval` seconds (0.5 by default). It returns the average, the peak and every sample, plus an 800 x 300 PNG chart. The chart is saved to `output=` if you give one, and otherwise attached to the result.

This is handy around a slow job:

```
# profile.txt: how busy does a build make the machine?
terminal.exec command="cmake --build build --parallel" folder=~/src/game timeout=600
monitor.watch seconds=10 interval=0.5 output=after-build.png
```

## Claude

```sh
claude mcp add wetwaresuite-monitor -s user -- wetwaresuite-system mcp --tools monitor
```

Example prompts and the tools Claude calls:

- *"Why is my laptop so slow?"*: `monitor_overview`, then `monitor_processes sort=cpu limit=10`. Claude names the busy programs, and calls `monitor_process` on a suspicious one.
- *"How much space is left on each drive?"*: `monitor_disks`.
- *"Chrome is frozen, end it"*: `monitor_processes filter=chrome`, then `monitor_kill`. Claude should confirm with you first, because ending a program can't be undone.
- *"Watch the CPU for 30 seconds and show me"*: `monitor_watch seconds=30`. Claude receives the chart as a picture.

## Parameter reference

| Command | Main parameters |
|---|---|
| `monitor.overview`, `monitor.system`, `monitor.cpu`, `monitor.memory`, `monitor.disks`, `monitor.network`, `monitor.battery` | |
| `monitor.processes` | `sort` (cpu, memory, name, pid), `filter`, `user`, `limit` |
| `monitor.process` | `pid` |
| `monitor.kill` | `pid` or `name`, `force` |
| `monitor.watch` | `seconds`, `interval`, `output` |

## Limitations

- No temperature or fan sensors, and no GPU use.
- On Linux, program names are the kernel's short names, at most 15 characters (`wetwaresuite-sy`). The full command line is in `rows` and in the Advanced view.
- Per-program network and disk use are not shown, only the totals.
- The numbers come from separate code for each system. The Linux version is the most tested; the Windows and macOS versions build from the same commands, but have had less testing on real machines.

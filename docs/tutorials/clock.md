# Clock: time, timers, stopwatch and alarms

`clock` has:

- the time here and in about a hundred cities;
- converting a time between places;
- countdown timers you can pause;
- a stopwatch with laps;
- alarms (daily, weekdays, weekends, chosen days, once, or on a date), which are saved;
- a calendar with week numbers;
- date maths: days between two dates, working days, and "3 weeks from today".

Time zones and summer-time rules are built in, so nothing goes online. `clock` is part of the system app (`wetwaresuite-system`) and the all-in-one `wetwaresuite`.

Each task is shown in the window, on the command line, in batch scripts (see [headless and batch](headless-and-batch.md)) and from Claude (see [mcp.md](mcp.md)). The outputs were copied from real runs with the computer set to London time (`TZ=Europe/London`).

## The window

Start the window with `wetwaresuite-system` and click **Open** on the **Clock and timers** card (called **Clock** in Advanced mode). To jump to one part directly:

```sh
wetwaresuite-system gui --action clock.world        # also clock.timer_start, clock.stopwatch, clock.alarm_add, clock.calendar
```

The tabs:

- **Clock**: the time in big digits, with the date, the time zone and the week number. Running timers and how many alarms are set are shown underneath.
- **World**: a row per city with its time and how far ahead or behind it is. Type a city in **add a city** and press **Add**, or **Remove** one.
- **Timers**: type a length (`5m`, `1h30m`, `90s`, `1:30:00`) and an optional label, then press **Start**. Each timer has a countdown with **Pause**, **Resume** and **Cancel**.
- **Stopwatch**: **Start**/**Stop**, **Lap** and **Reset**. Laps are listed with their split times.
- **Alarms**: a time (`07:30` or `6:15pm`), which days (daily, weekdays, weekends, once, or typed like `mon,wed,fri` or a date) and a label, then **Add alarm**. Each alarm has an on/off tick box and **Remove**.
- **Calendar**: a month with week numbers and today in [brackets]. **<**, **Today** and **>** move between months.
- **Dates**: days between two dates (with working days), and a date plus some time. In Advanced mode it also converts a time between places ("9:00 in New York to London|Tokyo").

When a timer finishes or an alarm goes off, a message pops up at the top of the window, wherever you are in the app. It has **OK** and **Show the clock** buttons. If the window is in the background, its taskbar or dock icon flashes. The window wakes itself up for this, so it is on time even while you are not touching it.

## Task 1: what time is it, here and elsewhere

```
$ wetwaresuite clock.now
Saturday 17 October 2026, 01:08:18
UTC+01:00, summer time; week 42, day 290 of 365
Unix time 1792195698
2026-10-17T01:08:18+01:00

$ wetwaresuite clock.world
here: Saturday 17 October 2026, 01:08 (UTC+01:00)
London           01:08            +0 h      UTC+01:00 summer
New York         20:08 day before  -5 h      UTC-04:00 summer
Los Angeles      17:08 day before  -8 h      UTC-07:00 summer
Tokyo            09:08            +8 h      UTC+09:00
Sydney           11:08            +10 h     UTC+11:00 summer
Delhi            05:38            +4.5 h    UTC+05:30
Dubai            04:08            +3 h      UTC+04:00
Paris            02:08            +1 h      UTC+02:00 summer
Shanghai         08:08            +7 h      UTC+08:00

$ wetwaresuite clock.world places="London|New York|Tokyo" at="2026-12-31 23:00"
here: Thursday 31 December 2026, 23:00 (UTC+00:00)
London           23:00            +0 h      UTC+00:00
New York         18:00            -5 h      UTC-05:00
Tokyo            08:00 next day   +9 h      UTC+09:00
```

How places and times are written:

- **Places** can be a city (`Tokyo`, `New York`), a zone id (`Asia/Kolkata`) or an offset (`UTC+5:30`). Separate several with `|`.
- **Finding a city.** `clock.zones search=india` lists the cities the clock knows that match.
- **Another day.** `at=` asks about a time other than now. Summer time is worked out for that date, which is why New York is 5 hours behind London in both answers above.

## Task 2: plan a call across time zones

```
$ wetwaresuite clock.convert time=9:00 from="New York" to="London|Tokyo|Sydney"
Friday 16 October 2026, 09:00 New York (UTC-04:00) is
  14:00  Friday 16 October 2026  London (UTC+01:00, summer time)
  22:00  Friday 16 October 2026  Tokyo (UTC+09:00)
  00:00  Saturday 17 October 2026  Sydney (UTC+11:00, summer time)
```

`time=` takes `9:00`, `3pm`, `2026-03-29 01:30` or `tomorrow 3pm`. Without `to=`, it converts to UTC and here.

## Task 3: dates

```
$ wetwaresuite clock.date_diff from=2026-10-17 to=2026-12-25
69 days
9 weeks and 6 days
0 years, 2 months, 8 days
49 working days (Mon-Fri)

$ wetwaresuite clock.date_add date=2026-10-16 add="10 working days"
Friday 30 October 2026  (2026-10-30)

$ wetwaresuite clock.calendar month=10
           October 2026
  wk  Mo  Tu  We  Th  Fr  Sa  Su
  40               1   2   3   4
  41   5   6   7   8   9  10  11
  42  12  13  14  15  16[17]  18
  43  19  20  21  22  23  24  25
  44  26  27  28  29  30  31
```

More options:

- `add=` understands days, weeks, months, years, hours and working days, and combinations like `"1 year 2 months"` or `"-10 days"`.
- `clock.calendar month=0` prints the whole year.
- Weeks start on Monday and use ISO week numbers.
- Working days are Monday to Friday. Public holidays are not taken into account.

## Task 4: timers, a stopwatch and alarms

```
# kitchen.txt: timers, a stopwatch with laps, and alarms
clock.timer_start duration=3s label=eggs
clock.timer_start duration=4m label=tea
clock.timer_pause timer=tea
clock.timers
clock.stopwatch action=start
clock.wait seconds=1
clock.stopwatch action=lap
clock.wait timer=eggs
clock.stopwatch action=stop
clock.due
clock.timer_cancel all=true
clock.alarm_add time=7:30 days=weekdays label="get up"
clock.alarm_add time=6pm days=once label="call Sam"
clock.alarms
clock.alarm_toggle alarm=a1
clock.alarm_remove alarm=a2
undo
clock.alarms
```

```
$ wetwaresuite batch kitchen.txt
timer t1 set for 0:03 (eggs)
timer t2 set for 4:00 (tea)
paused t2 with 4:00 left
t1  0:03 left of 0:03  "eggs"
t2  4:00 left (paused) of 4:00  "tea"

started
0:00.0 (running)
waited 0:01.0
lap 1
0:01.0 (running)
  lap 1  0:01.0  (0:01.0)
timer t1 is done
stopped
0:03.0
  lap 1  0:01.0  (0:01.0)
timer t1 is done: eggs

removed 2 timers
alarm a1  07:30  weekdays  "get up", next Mon 19 Oct
alarm a2  18:00  2026-10-17  "call Sam", next Sat 17 Oct
a1  07:30  weekdays  "get up", next Mon 19 Oct
a2  18:00  2026-10-17  "call Sam", next Sat 17 Oct

alarm a1  07:30  weekdays  "get up"  (off)
removed alarm a2
undone
a1  07:30  weekdays  "get up"  (off)
a2  18:00  2026-10-17  "call Sam", next Sat 17 Oct
```

How these commands work:

- **Naming timers.** Timers are named `t1`, `t2` and so on, and can also be picked by their label (`timer=tea`). Without `timer=`, pause and resume work on the newest timer.
- **Waiting in a script.** `clock.wait` waits for some seconds, or until a timer is done. It waits at most 10 minutes, which makes timers useful in scripts.
- **What's due.** `clock.due` reports each finished timer and each alarm that has gone off since the last look, once. The window asks it every second. A script can call it in a loop.
- **Alarms are saved** in the suite's settings, so they come back next time the app opens. A `once` alarm is stored with its date (today if the time is still ahead, otherwise tomorrow), so it rings one time.
- **Undo.** Removing or toggling an alarm can be undone.

## Claude

```sh
claude mcp add wetwaresuite-clock -s user -- wetwaresuite-system mcp --tools clock
```

Example prompts and the tools Claude calls:

- *"What time is it in Tokyo and Sydney?"*: `clock_world` with `places=Tokyo|Sydney`.
- *"Find a time for a call between 9 and 5 in London, New York and Bangalore"*: `clock_world` with several `at=` times, then Claude picks the overlap.
- *"How many working days until the 25th of December?"*: `clock_date_diff`.
- *"Set an alarm for 7:15 on weekdays"*: `clock_alarm_add` with `time=7:15 days=weekdays`.
- *"Remind me in 20 minutes to check the oven"*: `clock_timer_start` with `duration=20m label=oven`. The message pops up in the window when it is done.

## Parameter reference

| Command | Main parameters |
|---|---|
| `clock.now` | `place` |
| `clock.world` | `places`, `at` |
| `clock.convert` | `time`, `from`, `to` |
| `clock.zones` | `search` |
| `clock.calendar` | `year`, `month` |
| `clock.date_diff`, `clock.date_add` | `from`, `to`; `date`, `add` |
| `clock.timer_start`, `clock.timer_pause`, `clock.timer_resume`, `clock.timer_cancel`, `clock.timers` | `duration`, `label`, `timer`, `all` |
| `clock.stopwatch` | `action` (show, start, stop, lap, reset) |
| `clock.alarm_add`, `clock.alarm_toggle`, `clock.alarm_remove`, `clock.alarms` | `time`, `days`, `label`, `alarm`, `on` |
| `clock.due`, `clock.wait` | `seconds`, `timer` |

## Limitations

- **Places and summer time.** The clock knows about a hundred cities. Its summer-time rules are the current ones for each region (Europe, North America, Israel, Australia, New Zealand and Chile), not the full historical tz database, so past years may be off by an hour where the rules changed. Your own computer's time zone always comes from the system, so "here" is always right.
- **Timers and the stopwatch** last only while the app (or the batch script, or the MCP session) is running. They are not saved.
- **Alarms only ring while the suite is open**, and they ring as a pop-up and a flashing window, with no sound. Nothing runs in the background when the app is closed.
- No public holidays, and no calendars other than the Gregorian one.

# Flash: firmware onto Arduino, ESP32/ESP8266 and STM32 boards

`flash` puts firmware on microcontroller boards over a serial port, with no IDE and no window: it lists the serial ports and says what is plugged into each (CH340, CP210x, FTDI, an Arduino, an ST-Link, an ESP32-S3's own USB...), detects which bootloader is answering, inspects firmware files (Intel HEX, raw `.bin`, ELF, Espressif images), writes and verifies them, and then gives you a serial monitor and a way to send text to the running sketch.

```sh
wetwaresuite flash.flash port=/dev/ttyUSB0 file=Blink.ino.hex
```

The bootloader protocols are built in, so **nothing else needs to be installed**:

| Protocol | Boards | Notes |
| --- | --- | --- |
| STK500v1 | Arduino Uno, Nano, Pro Mini, Duemilanove (optiboot and the older ATmegaBOOT) | what avrdude calls `-c arduino` |
| STK500v2 | Arduino Mega 2560 (wiring bootloader) | avrdude `-c wiring` |
| AVR109 | Arduino Leonardo, Micro (Caterina) | entered with a 1200 baud "touch" |
| Espressif ROM | ESP32, ESP32-S2/S3/C3/C6/H2, ESP8266 | SLIP framing, uncompressed upload, MD5 verify (not on ESP8266) |
| AN3155 | any STM32 through its USART system bootloader | 8E1, BOOT0 high |

If you already use avrdude, esptool, STM32CubeProgrammer, st-flash or dfu-util, pass `backend=` and flash runs that instead (and `flash.command` prints the exact command line). ST-Link (SWD) and USB DFU aren't serial bootloaders, so for those an external tool is the only way.

**No board on your desk?** Every command works against simulated boards: `sim:uno`, `sim:nano`, `sim:nano-old`, `sim:pro-mini`, `sim:mega2560`, `sim:leonardo`, `sim:micro`, `sim:esp32`, `sim:esp32-s2`, `sim:esp32-s3`, `sim:esp32-c3`, `sim:esp32-c6`, `sim:esp8266`, `sim:stm32` (an F103 "Blue Pill") and `sim:stm32f4` (an F411). They speak the real protocols byte for byte, model the auto-reset wiring, and "run" the firmware afterwards by printing a line about it. Every CLI and batch example below was run against the real binary with these simulated boards; the outputs are copied from those runs.

There is no GUI yet; the GUI that comes later will call these same commands. Each task below is shown three ways: one CLI command, a batch script, and asking Claude over MCP.

## Defaults you should know about

These are the settings flash uses when you don't say otherwise. They are right for the common boards, but they *are* defaults, and a wrong one is the usual reason an upload fails:

| Board preset | Protocol | Bootloader baud | Reset method | Raw `.bin` address |
| --- | --- | --- | --- | --- |
| `uno`, `nano` | stk500v1 | **115200** | `arduino` | 0x0 |
| `nano-old`, `pro-mini` | stk500v1 | **57600** | `arduino` | 0x0 |
| `mega2560` | stk500v2 | 115200 | `arduino` | 0x0 |
| `leonardo`, `micro` | avr109 | 57600 (USB, so it doesn't matter) | `1200bps` | 0x0 |
| `esp32`, `esp32-s2` | esp | 115200 | `esp-classic` (`esp-usb-jtag` on a 303a:xxxx port) | **0x10000** |
| `esp32-s3`, `esp32-c3`, `esp32-c6`, `esp32-h2` | esp | 115200 | same | **0x10000** |
| `esp8266` | esp | 115200 | `esp-classic` | **0x0** |
| `stm32` | stm32 (8E1) | 115200 | **`none`** (set BOOT0 yourself) | **0x08000000** |

Other defaults:

- `board=auto` (the default) picks the board from the port's USB id when it implies one (an Uno R3 is 2341:0043, a Mega 2341:0042, an ESP32-S3's native USB 303a:1001...). A bare USB-serial chip (CH340, CP210x, FTDI) doesn't say what board is behind it; then pass `board=`, or run `flash.detect` first.
- `verify=true`: AVR and STM32 read every written byte back; ESP32-family chips compare an MD5 the ROM computes over the written flash. The ESP8266 ROM has no MD5 command, so its writes are not verified (the result says `verified: false`).
- `run=true`: after writing, the board is reset (AVR: leaving the bootloader starts the sketch; ESP: an EN pulse via RTS; STM32: the bootloader's GO command, or a reset with BOOT0 low for `reset=dtr-boot0`).
- `erase=true` (STM32): the whole flash is mass-erased before writing, because flash bits can only be cleared. AVR bootloaders erase page by page as they write; the ESP ROM erases the sectors it is about to write.
- ESP: the ROM is told the flash is 4 MB (`flash_size=` changes it; it only matters for writes beyond 4 MB) and stays at 115200 baud unless you pass `flash_baud=460800` (ESP8266's ROM can't change baud).
- Reset methods (`reset=`):
  - `arduino`: DTR and RTS off for 250 ms then on. The falling edge reaches RESET through the board's 100 nF capacitor.
  - `1200bps`: open the port at 1200 baud and drop DTR; a Leonardo/Micro restarts into its bootloader for 8 seconds, often under a new port name, which flash waits for.
  - `esp-classic`: esptool's sequence for the two-transistor auto-reset circuit on DevKits and NodeMCUs (RTS pulls EN low, then DTR pulls IO0 low while EN rises).
  - `esp-usb-jtag`: esptool's sequence for the USB-Serial-JTAG built into the ESP32-S3/C3/C6/H2.
  - `dtr-boot0`: for an STM32 with the adapter's DTR wired to NRST and RTS to BOOT0.
  - `none`: do nothing. Put the board in its bootloader yourself (hold BOOT/IO0 and tap EN on an ESP, set the BOOT0 jumper and press reset on an STM32) and then run the command.

`flash.boards` prints the same table with notes, so Claude can see it too.

## Before you start: permissions and drivers

- **Linux**: serial ports belong to the `dialout` group (Debian, Ubuntu, Fedora) or `uucp` (Arch). `sudo usermod -aG dialout $USER`, then log out and in. Without it you get "permission denied: on Linux add yourself to the dialout ... group".
- **Windows**: CH340 and CP210x boards need the vendor's driver on older Windows; Windows 10/11 usually fetches it. Ports are `COM3`, `COM4`...; Device Manager shows which is which, and so does `flash.ports`.
- **macOS**: use the `/dev/cu.*` name (not `/dev/tty.*`). Old CH340 boards need WCH's driver; macOS 11+ includes one.
- Close the Arduino IDE serial monitor, PuTTY, screen or Cura first. A port can only be open in one program; otherwise flash says "the port is busy".

## Task 1: what is plugged in?

### CLI

```sh
$ wetwaresuite flash.ports
2 serial ports
/dev/ttyUSB0  [1a86:7523]  CH340  (USB Serial)  note: cheap USB-serial: Nano clones, many ESP32/ESP8266 boards
/dev/ttyACM0  [2341:0043]  Arduino Uno R3  -> board uno
sim:uno  (simulated avr board)
sim:nano  (simulated avr board)
...
```

(On the machine the examples were run on there is no hardware, so it printed `no USB serial ports found` followed by the simulated boards; the two real lines above show what a CH340 Nano clone and an original Uno look like. `usb=true` lists only USB ports, `include_sim=false` hides the simulated ones.)

What identification you get depends on the OS:

- **Linux**: `/dev/ttyUSB*`, `/dev/ttyACM*` (also `ttyAMA*` and other USB ttys) with VID:PID, manufacturer, product and serial number from sysfs.
- **Windows**: COM ports from the registry (`HARDWARE\DEVICEMAP\SERIALCOMM`, with `QueryDosDevice` as a fallback), VID:PID and the device's friendly name from the USB and FTDIBUS enumeration keys.
- **macOS**: `/dev/cu.*`, identified from the driver's name only (`cu.wchusbserial*` is a CH340, `cu.SLAB_USBtoUART` a CP210x, `cu.usbserial-A...` an FTDI, `cu.usbmodem*` a USB CDC device like an Uno R3 or an ESP32-S3). No VID:PID there yet.

Recognised ids: CH340/CH341/CH343/CH9102 (1a86), CP210x/CP2105 (10c4), FTDI FT232R/FT2232/FT4232/FT232H/FT-X (0403), Prolific PL2303 (067b), Arduino Uno/Mega/Leonardo/Micro (2341, 2a03), Espressif USB-Serial-JTAG (303a:1001) and ESP32-S2 native USB (303a:0002), ST-Link V2/V2-1/V3 (0483:3748, 374b, 3752, 374e, 374f, 3753), STM32 DFU (0483:df11), STM32 virtual COM (0483:5740) and SEGGER J-Link (1366).

### Ask which board it is

Probing actually talks to the bootloaders, which resets the board:

```sh
$ wetwaresuite flash.detect port=sim:nano-old
detected ATmega328P - flash with board=nano-old
found: true
protocol: stk500v1
chip: ATmega328P
board: nano-old
baud: 57600
details: ATmega328P via optiboot 8.0
tried: esp-rom, stk500v1@115200, stk500v1@57600
```

The order is: the ESP ROM (classic reset), STK500v1 at 115200 then 57600, STK500v2 at 115200, then the STM32 bootloader at 8E1. `only=avr|esp|stm32` limits it, and `probe=false` uses the USB id alone without touching the board. An ST-Link or J-Link port is never probed: detect just says it's an SWD probe and which backend to use.

```sh
$ wetwaresuite flash.detect port=sim:stm32 only=stm32
detected STM32F10x medium density (F103C8/CB "Blue Pill") - flash with board=stm32
found: true
protocol: stm32
chip: STM32F10x medium density (F103C8/CB "Blue Pill")
board: stm32
baud: 115200
details: STM32F10x medium density (F103C8/CB "Blue Pill"), bootloader v2.2
tried: stm32-usart
```

### Ask Claude

> What boards do I have plugged in, and which one is the ESP32?

Claude calls `flash_ports`, then `flash_detect` on the likely port (with `only=esp`), and tells you the port name and chip.

## Task 2: check a firmware file before flashing

Arduino IDE: *Sketch > Export Compiled Binary* writes `Blink.ino.hex` (and `Blink.ino.with_bootloader.hex`; use the plain one). arduino-cli: `arduino-cli compile --output-dir out`. PlatformIO: `.pio/build/<env>/firmware.hex` or `.bin`. ESP-IDF: `build/<project>.bin`, `build/bootloader/bootloader.bin`, `build/partition_table/partition-table.bin`. STM32CubeIDE: `Debug/<project>.bin` or `.hex` or `.elf`.

### CLI

```sh
$ wetwaresuite flash.inspect file=blink.hex
ihex, 1024 bytes in 1 region(s), 0x00000000..0x00000400
format: ihex
bytes: 1024
low_address: 0x00000000
high_address: 0x00000400
regions: 0x00000000 +1024
```

An Espressif image (a `.bin` starting with 0xE9) also shows its header:

```sh
$ wetwaresuite flash.inspect file=app.bin
esp-image, 1248 bytes in 1 region(s), 0x00000000..0x000004e0; Espressif esp32 image, flash dio 16MB @40m, 1 segments, checksum ok
format: esp-image
bytes: 1248
low_address: 0x00000000
high_address: 0x000004e0
regions: 0x00000000 +1248
esp_chip: esp32
flash_mode: dio
flash_size: 16MB
flash_freq: 40m
esp_segments: 1
esp_entry: 0x00000000
checksum_ok: true
```

`flash_size` and `flash_mode` come from the image header, which the build set (this one was made by a test script). A merged/factory image (bootloader, partition table and app in one file) is recognised and reported as `merged_image_at: 0x00001000` (or `0x00000000` on the S3/C3/C6); flash writes it at that offset.

ELF files list their machine (AVR, ARM, Xtensa, RISC-V), entry point and allocated sections with their flash (load) addresses; the loadable program headers are what gets written, exactly like `objcopy -O ihex`. AVR ELFs drop the RAM/EEPROM/fuse address spaces (0x800000 and above).

Options: `address=0x08000000` places a raw `.bin` at that address, `hex=true` adds the first 64 bytes in hex, and `preview=true` attaches a small memory-map picture showing where the data lands.

### Batch

```sh
# check.txt
flash.inspect file=build/app.bin hex=true
flash.inspect file=build/bootloader/bootloader.bin
```

```sh
wetwaresuite batch check.txt
```

### Ask Claude

> Is build/firmware.elf an STM32 build, and how big is it?

Claude calls `flash_inspect` with the file and reads `machine: ARM`, the regions and the size.

## Task 3: flash an Arduino Uno or Nano

### CLI

```sh
$ wetwaresuite flash.flash port=sim:uno file=blink.hex
flashed 1024 bytes to ATmega328P on sim:uno, verified (read-back)
board: uno
protocol: stk500v1
backend: native
chip: ATmega328P
bytes: 1024
blocks: 8
verified: true
verify: read-back
regions: 0x00000000 +1024
seconds: 0.00
log: connecting (stk500v1) at 115200 baud
found ATmega328P (optiboot 8.0)
verified 8 pages by reading them back
```

On real hardware the port is `/dev/ttyACM0` (an original Uno), `/dev/ttyUSB0` (a CH340 clone) or `COM5`, and the upload of a small sketch takes 2 to 5 seconds.

The common failure is a Nano with the old bootloader, which runs at 57600 baud:

```sh
$ wetwaresuite flash.flash port=sim:nano-old board=uno file=blink.hex
flash failed: no answer from an STK500v1 (optiboot) bootloader at 115200 baud. Old-bootloader Nanos and Pro Minis run at 57600: try board=nano-old (or baud=57600). Check the port, and that nothing else (an IDE serial monitor) has it open; flash.detect probes for you.
```

```sh
$ wetwaresuite flash.flash port=sim:nano-old board=nano-old file=blink.hex
flashed 1024 bytes to ATmega328P on sim:nano-old, verified (read-back)
```

A sketch that doesn't fit is refused before anything is written ("the firmware reaches 0x... but only 32256 bytes are free for a sketch"). A `with_bootloader.hex` gets the same error, because the bootloader can't overwrite itself.

Leonardo and Micro need no extra steps; flash does the 1200 baud touch and waits for the bootloader's port:

```sh
$ wetwaresuite flash.flash port=sim:leonardo file=blink.hex
flashed 1024 bytes to ATmega32U4 on sim:leonardo, verified (read-back)
board: leonardo
protocol: avr109
...
```

### Batch

```sh
# upload.txt: flash, then watch what the sketch prints
flash.flash port=/dev/ttyUSB0 board=nano-old file=out/Blink.ino.hex
flash.monitor port=/dev/ttyUSB0 baud=9600 seconds=5
```

### Ask Claude

> Upload out/Blink.ino.hex to the Nano on /dev/ttyUSB0 and tell me what it prints.

Claude calls `flash_flash` (port, file, and `board=nano-old` if the first try says 57600), then `flash_monitor` with the baud from your `Serial.begin`.

## Task 4: flash an ESP32 (app, or bootloader + partitions + app)

### CLI: just the app

```sh
$ wetwaresuite flash.flash port=sim:esp32 file=app.bin address=0x10000
flashed 1248 bytes to esp32 on sim:esp32, verified (md5)
board: esp32
protocol: esp
backend: native
chip: esp32
bytes: 1248
blocks: 2
verified: true
verify: md5
regions: 0x00010000 +1248
seconds: 0.08
log: synced with the ESP ROM bootloader
chip is esp32
erasing and writing 1248 bytes at 0x00010000
verified 0x00010000 by MD5
reset to run the new firmware
```

`address=0x10000` is the default for ESP32-family boards (the `factory`/`app0` partition of the standard layouts), so it can be left out; it's shown here to be explicit. For an ESP8266 the default is 0x0. If your partition table puts the app elsewhere, pass that address. Addresses must be multiples of 0x1000 (4 KB sectors).

### CLI: several files

A fresh chip, or a changed partition table, needs all three parts. Use the addresses your build printed (ESP-IDF prints them after `idf.py build`; Arduino-ESP32 shows them in the verbose upload log):

```sh
$ wetwaresuite flash.flash port=sim:esp32 parts="0x1000=boot.bin,0x8000=partitions.bin,0x10000=app.bin" verify=false
flashed 3744 bytes to esp32 on sim:esp32
board: esp32
protocol: esp
backend: native
chip: esp32
bytes: 3744
blocks: 6
verified: false
regions: 0x00001000 +1248, 0x00008000 +1248, 0x00010000 +1248
seconds: 0.08
log: synced with the ESP ROM bootloader
chip is esp32
erasing and writing 1248 bytes at 0x00001000
erasing and writing 1248 bytes at 0x00008000
erasing and writing 1248 bytes at 0x00010000
reset to run the new firmware
```

The bootloader goes at **0x1000 on ESP32 and ESP32-S2** but at **0x0 on the S3, C3, C6 and H2**. A wrong bootloader address leaves the chip printing `invalid header` forever (reflash with the right one; the ROM bootloader can't be damaged).

Speed: the ROM loader writes uncompressed, about 10 KB/s at 115200 baud, so a 1 MB app takes around 100 seconds. `flash_baud=460800` makes it roughly 4 times faster on most CP210x/CH340 boards (and works on the ESP32 family, not the ESP8266). esptool is faster still because it uploads a RAM "stub" and compresses; use `backend=esptool` when it's installed and speed matters.

If it doesn't connect: hold the board's **BOOT** (IO0) button, tap **EN/RESET**, let go of BOOT and run the command with `reset=none`. Some boards (and many CH340 ESP8266 modules) lack the auto-reset transistors. On an ESP32-S3 or C3 plugged in by its native USB port, the reset method is picked automatically from the 303a USB id.

### Batch: flash, then read the boot log, in one session

Simulated boards keep their flash for the life of the process, so to see a flashed sim board run, flash and monitor it in the same batch (or MCP) session:

```sh
# esp.txt
flash.inspect file=app.bin
flash.flash port=sim:esp32 file=app.bin address=0x10000
flash.monitor port=sim:esp32 seconds=0.3 baud=115200
```

```sh
$ wetwaresuite batch esp.txt
...
flashed 1248 bytes to esp32 on sim:esp32, verified (md5)
...
read 312 bytes in 0s
bytes: 312
text: ets Jun  8 2016 00:22:57

rst:0x1 (POWERON_RESET),boot:0x13 (SPI_FAST_FLASH_BOOT)
[sim esp32] booted the image at 0x10000: 1 segments, entry 0x00000000
...
```

### Ask Claude

> Flash build/bootloader/bootloader.bin, build/partition_table/partition-table.bin and build/hello.bin to the ESP32-S3 on /dev/ttyACM0 and show me its boot log.

Claude calls `flash_flash` with `parts=` (using 0x0 for the S3's bootloader, 0x8000, 0x10000), then `flash_monitor` with `seconds=5`.

## Task 5: flash an STM32 through its serial bootloader

Wire a 3.3 V USB-serial adapter to USART1: adapter TX to **PA10**, adapter RX to **PA9**, GND to GND. Set **BOOT0 = 1** (on a Blue Pill, the top jumper to 1), press reset, then:

### CLI

```sh
$ wetwaresuite flash.flash port=sim:stm32 file=blinky-stm32.bin
flashed 6000 bytes to STM32F10x medium density (F103C8/CB "Blue Pill") on sim:stm32, verified (read-back)
board: stm32
protocol: stm32
backend: native
chip: STM32F10x medium density (F103C8/CB "Blue Pill")
bytes: 6000
blocks: 24
verified: true
verify: read-back
regions: 0x08000000 +6000
seconds: 0.01
log: STM32 bootloader answered
bootloader v2.2, STM32F10x medium density (F103C8/CB "Blue Pill")
flash erased
wrote 1536/6000 bytes
wrote 3072/6000 bytes
wrote 4608/6000 bytes
verified by reading 6000 bytes back
told the bootloader to jump to 0x08000000
```

A raw `.bin` goes to 0x08000000; HEX and ELF files carry their own addresses. Mass erase uses the ERASE (0x43) or EXTENDED ERASE (0x44) command, whichever the chip's bootloader lists. After `run=true` the firmware starts, but **put BOOT0 back to 0** or the next power-up lands in the bootloader again. If your adapter's DTR goes to NRST and RTS to BOOT0, `reset=dtr-boot0` does all of that for you.

Read-out protection makes the bootloader refuse writes (NACK). flash doesn't remove it, because removing it erases the chip; use STM32CubeProgrammer for that.

On a Nucleo or Discovery board, or with an ST-Link dongle, the ST-Link talks SWD, not the serial bootloader:

```sh
$ wetwaresuite flash.command port=swd file=blinky.bin backend=st-flash
st-flash --reset write blinky.bin 0x08000000
program: st-flash
installed: false
note: st-flash isn't installed (not on PATH)
```

and `flash.flash` with the same arguments runs it (st-flash finds the probe by itself, so `port=` is only a label there).

### Batch

```sh
# stm32.txt
flash.detect port=/dev/ttyUSB0 only=stm32
flash.flash port=/dev/ttyUSB0 board=stm32 file=build/blinky.bin
flash.monitor port=/dev/ttyUSB0 baud=115200 seconds=3
```

### Ask Claude

> I've set BOOT0 high on my Blue Pill on COM7; flash Debug/blinky.bin.

Claude calls `flash_flash` with `board=stm32`, and reminds you to set BOOT0 back.

## Task 6: talk to the running sketch

### CLI

```sh
$ wetwaresuite flash.send port=sim:uno text="status?" newline=lf read_seconds=0.3
sent 8 bytes, read 24 back
sent: 8
reply: [sim uno] got: status?
```

(The simulated boards echo each line they get, which is what the reply shows.) Escapes work in `text`: `\n`, `\r`, `\t`, `\\`, `\0` and `\xNN`, e.g. `text="AT\r\n"` for an AT modem, or `newline=crlf`. `read_seconds=0` sends without waiting.

`flash.monitor` reads for `seconds=` (default 3) at `baud=` (default 115200; match your `Serial.begin`) and returns the text, with non-text bytes as `\xNN`. It doesn't reset the board, except that opening the port of an Uno/Nano resets it anyway through the same DTR auto-reset; `reset=true` forces a reset to catch the boot messages. An ESP8266 prints its ROM boot messages at 74880 baud; the OS here doesn't offer that rate on Linux, only standard ones (9600 ... 921600, plus any rate on macOS and Windows).

### Batch

```sh
# chat.txt
flash.send port=/dev/ttyACM0 text="led on" newline=lf read_seconds=0.5
flash.send port=/dev/ttyACM0 text="temp?" newline=lf read_seconds=0.5
```

### Ask Claude

> My sketch on /dev/ttyACM0 answers "temp?" at 9600 baud. Ask it the temperature every few seconds, three times.

Claude calls `flash_send` three times with `baud=9600 newline=lf` and reads the replies.

## Using the external tools

```sh
$ wetwaresuite flash.tools
no external flashing tools found (the native backend needs none)

$ wetwaresuite flash.command port=/dev/ttyUSB0 board=uno backend=avrdude file=blink.hex
avrdude -p atmega328p -c arduino -P /dev/ttyUSB0 -b 115200 -D -U flash:w:blink.hex:a
program: avrdude
installed: false
note: avrdude isn't installed (not on PATH)

$ wetwaresuite flash.command port=/dev/ttyUSB0 board=esp32 backend=esptool file=app.bin address=0x10000
esptool --chip esp32 --port /dev/ttyUSB0 --baud 115200 write_flash --verify 0x00010000 app.bin
program: esptool
installed: false
note: esptool isn't installed (not on PATH)
```

`flash.flash ... backend=avrdude` (or `esptool`, `stm32prog` for STM32_Programmer_CLI, `st-flash`, `dfu-util`) runs that command and returns its output. The tools are run directly, never through a shell, so file names can't be misread as commands. Simulated ports only work with the native backend.

## Parameter reference

### flash.flash

| Parameter | Default | Meaning |
| --- | --- | --- |
| `port` | (required) | `/dev/ttyUSB0`, `COM3`, `/dev/cu.usbserial-0001`, or `sim:<board>` |
| `file` | | `.hex`, `.bin` or `.elf` (detected from the contents) |
| `parts` | | ESP: `0x1000=a.bin,0x8000=b.bin,0x10000=c.bin` |
| `board` | `auto` | a preset from `flash.boards` (aliases: `mega`, `nodemcu`, `d1-mini`, `bluepill`, `esp32s3`...) |
| `protocol` | the preset's | `stk500v1`, `stk500v2`, `avr109`, `esp`, `stm32` |
| `baud` | the preset's | bootloader baud |
| `flash_baud` | 0 (stay) | ESP: switch to this baud after connecting |
| `reset` | the preset's | `arduino`, `1200bps`, `esp-classic`, `esp-usb-jtag`, `dtr-boot0`, `none` |
| `address` | the preset's | where a raw `.bin` goes |
| `verify` | `true` | read back / MD5 |
| `erase` | `true` | STM32 mass erase first |
| `run` | `true` | start the firmware afterwards |
| `flash_size` | `4m` | ESP: flash size told to the ROM |
| `backend` | `native` | `avrdude`, `esptool`, `stm32prog`, `st-flash`, `dfu-util` |

### The other commands

| Command | Parameters | What it does |
| --- | --- | --- |
| `flash.ports` | `usb=false`, `include_sim=true` | list serial ports with USB identification |
| `flash.detect` | `port`, `probe=true`, `only=auto` | identify the board from its USB id and by probing bootloaders |
| `flash.boards` | `board` | the presets and their defaults |
| `flash.inspect` | `file`, `address`, `hex=false`, `preview=false` | describe a firmware file |
| `flash.command` | `port`, `file`/`parts`, `board`, `backend`, `address`, `baud`, `verify` | print the external tool's command line |
| `flash.tools` | | which external tools are installed |
| `flash.monitor` | `port`, `seconds=3`, `baud=115200`, `reset=false`, `max_bytes=65536` | read the serial output |
| `flash.send` | `port`, `text`, `baud=115200`, `read_seconds=1`, `newline=none` | write text, read the reply |

Over MCP the same commands are the tools `flash_ports`, `flash_detect`, `flash_boards`, `flash_inspect`, `flash_command`, `flash_tools`, `flash_flash`, `flash_monitor` and `flash_send`.

## Limitations

- Only flash memory is written: no EEPROM, fuses or lock bits on AVR, no option bytes on STM32, no eFuses on ESP. Burning a bootloader onto a blank AVR needs an ISP programmer and avrdude.
- STK500v1 addresses up to 128 KB; boards with more flash on STK500v1 (an ATmega1284P with optiboot) need `backend=avrdude`.
- ESP uploads go through the ROM loader without compression or the RAM stub, so they're several times slower than esptool's defaults. The flash size isn't read from the chip (`flash_size=` sets what the ROM is told), flash encryption and Secure Boot aren't supported, and ELF files must be converted to a `.bin` first (`esptool elf2image`, or use the `.bin` the build made).
- The ESP8266 ROM can't compute an MD5, so ESP8266 writes are not verified.
- The STM32 bootloader is used only on its USART at 8E1; STM32 USB DFU, CAN and I2C bootloaders, read-out protection removal and option bytes are out of scope (use `backend=dfu-util` or `stm32prog`). Mass erase is used rather than per-page erase.
- On macOS ports are identified by name, not by VID:PID.
- On Linux only standard baud rates work (no 74880 for the ESP8266 boot log).
- Detect probing resets the board and may need the BOOT button held on boards without auto-reset; boards running a sketch that holds the port can't be probed.
- The simulated boards model the classic ESP auto-reset circuit only (not the USB-Serial-JTAG peripheral) and their flash lasts only as long as the process.

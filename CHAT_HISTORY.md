# Development chat history – LE-01MR v2 monitor

Session with Claude Code (Claude Opus 5.5), 2026-11-03.
Starting point: commit `b0df6e3 first_commit` (working Modbus-RTU skeleton).

---

## 1. Request

> This directory contain a C++20 application with a skeleton of Modbus-RTU communication, which
> works already. It should communicate with LE-01MRv2 electrical energy meter, to read, rescale to
> physical values, display and store readouds, regarding to more detailed description left in
> comments between line 250 and 280 in main.cpp . At this moment the application must works in
> console (also via SSH). You can use any exteral library You want, until it is available in debian
> repo. If something is missing I can install it manually.

The comments in `main.cpp` asked for:

- an htop-like console pseudo-GUI: horizontal bar graphs with a description on the left and the
  value on the right for Voltage, Current, Active/Apparent/Reactive Power, Frequency and Power
  factor; all other parameters as plain label – value with unit,
- a CSV output file opened for appending; if none is given on the command line, a new one named
  after the current date and time,
- a loop: wait 1 s, read all holding registers from the register map (both in raw and libmodbus
  mode), decode to physical units, update the display, store in CSV.

## 2. Analysis of the skeleton

Files reviewed: `main.cpp`, `raw_mode.*`, `serial_io_raw.*`, `serial_io_interface.h`, `makefile`,
`modbus-registers-map.*`.

Installed libraries checked: g++ 13.3, spdlog 1.12, fmt 9.1, libmodbus 3.1.10, boost
program_options 1.83, ncurses 6.4 (`libncurses-dev`) – nothing had to be installed.

Problems found in the existing code:

- raw mode always requested 8 registers from address 0 (left over from another project) and
  ignored the `startregister`/`num` arguments; buffers were limited to 32 bytes / 16 registers,
- `--raw-skip-bytes` was parsed as `uint8_t`, so `3` became the character `'3'` (51),
- only `unknown_option` was caught while parsing options, other errors crashed the program,
- `modbus_set_debug(TRUE)` writes to stdout, which would corrupt a full-screen UI,
- the `while (true)` loop had no exit path, so cleanup code was unreachable,
- `make clean` removed `modbus2aprs` instead of `le-01mr`.

## 3. Implementation

New / rewritten modules:

| File | Purpose |
|---|---|
| `serial_io_interface.h` | `SerialSettings`, `ModbusException` (slave exception responses), abstract `SerialIoIf::readHoldingRegisters()` |
| `serial_io_libmodbus.*` | libmodbus backend (exceptions mapped from `EMBX*` errno values, configurable response timeout) |
| `serial_io_raw.*`, `raw_mode.*` | raw backend: generic 0x03 request builder, incremental response parser (frame length + CRC, skip-zeros / skip-bytes kept), port kept open and reopened after I/O errors, optional RTS toggling in RS485 mode |
| `le01mr.hpp/.cpp` | register map table (moved from `main.cpp`), read planning, INT16/INT32 decoding and scaling, optional `--swap-words` |
| `csv_writer.*` | CSV appending, header for new/empty files, flush after every row, locale independent numbers |
| `console_ui.*` | ncurses UI: header/status, coloured htop-style bars, energy counters (1 or 2 columns), log message pane from an spdlog ring buffer, `q` quit, `Ctrl+L` redraw, resize handling |
| `main.cpp` | option parsing, logging setup, main loop with fixed-rate timing, signal handling (SIGINT/SIGTERM/SIGHUP) |

Other details:

- logging: spdlog `dist_sink` → stderr (removed while the UI is active), ring buffer (shown in the
  UI) and optional `--log-file`,
- `--no-ui` (also used automatically when not on a terminal) prints one line per reading,
- new options: `--slave`, `--baud`, `--parity`, `--stop-bits`, `--interval`, `--timeout`,
  `--read-mode`, `--swap-words`, `--max-current`, `--max-power`, `--verbose`, `--log-file`,
  `--modbus-debug`, `--help`,
- makefile: new objects, `-lncursesw`, `-Wall`, dependency files included, `clean` fixed.

## 4. Testing with a simulator

A Python Modbus slave simulating the meter was connected through a `socat` virtual serial pair,
and the UI was rendered in a pseudo-terminal with `pyte`.

Issues found and fixed:

- in `--no-ui` mode the final log line was printed twice (stderr sink added a second time),
- **`q` was ignored when one read cycle took longer than the interval** – the wait loop exited
  before polling the keyboard; it now always polls at least once per cycle,
- garbled rendering turned out to be a `pyte` limitation (no support for the `rep` terminal
  capability used by ncurses with `xterm-256color`); with `TERM=screen` the layout was correct,
- at 80×24 the footer overwrote the last line → added a compact layout (no separators/headings,
  port and CSV path on one line), so everything fits in 80×24,
- the "-" unit of the power factor is no longer displayed.

## 5. Testing on real hardware

> if You need to test this on real hardware, this energy meter is connected to /dev/ttyUSB0 with
> settings of 9600 8N1

Results:

- readings correct and plausible (≈241 V, 25 W, 31 VA, PF 0.80 = P/S), word order confirmed,
- the meter rejected one 120-register request (`illegal data address`); the automatic fallback
  to 12 contiguous groups worked, but a cycle took ≈1.4 s (> 1 s interval).

Read-only probing (function 0x03 only) showed:

- reads spanning unmapped registers between parameters are accepted,
- **the meter accepts at most 100 registers per request** (101+ → exception 0x02),
- latency ≈ 60 ms for a short request, ≈ 400 ms for 100 registers.

Changes made after that:

- `block` read mode now merges all parameters into as few requests as possible, limited by
  `--max-registers` (default 100, never splitting a 32-bit value) → 2 requests (96 + 16 registers),
  ≈0.5 s per cycle, steady 1 reading/s,
- `auto` mode falls back to contiguous groups if the merged requests are rejected,
- default stop bits changed from 2 to 1 to match the meter,
- `.gitignore` contained `le*`, which also ignored the new `le01mr.cpp/.hpp`; replaced with
  `le01mr_*.csv` (the binary is still ignored by its own `le-01mr` line).

Verified on the meter: libmodbus mode, raw mode (`-R`), `--read-mode grouped`, the automatic
fallback (`--max-registers 120`), the CSV contents and the full-screen UI.

Not tested: RTS toggling in raw mode with `--mode-485` (the adapter doesn't need it).

## 6. Open ideas

- a key for resetting the auxiliary energy counters (Write Single Register 0x2002, values
  0x2000 / 0x4000 / 0x6000) – not implemented, because it writes to the device; it should come
  with a confirmation prompt.

## 7. Request

> put a history of this chat in a markdown file in the source code directory

→ this file.

## 8. Request

> default serial port settings are 9600 8N1 and modbus address 1, so modify parseArgs to use them
> if user will not type these manually

No change was needed – `parseArgs` already defaulted to 9600 baud, parity N, 1 stop bit
(changed from 2 in step 5) and slave address 1; data bits are always 8. Only the serial port
remains mandatory.

## 9. Request

> add CSV file rotate mechanism, if the file will grow to about 5MB in size. The mechanism should
> work differently if a user has given a file name or not. In case of auto-generated name, the
> application should simply close an old file, then regenerate a filename once again and create a
> new file, with new timestamp. If the user has chosen a filename, the file should be closed and
> then renamed by adding a prefix with a timestamp when it was rotated. Then a new file with the
> same name given by the user should be created

Implemented in `CsvWriter`:

- the file size is tracked (initial size of an existing file + bytes written); when it reaches the
  limit, rotation happens before the next row is written,
- auto-generated name: close, generate `le01mr_YYYYmmdd_HHMMSS.csv` again, open the new file,
- name from `--out`: close, rename to `<name>_YYYYmmdd_HHMMSS.<ext>` in the same directory (time of
  rotation), create a new file with the original name,
- every new file gets the CSV header; an existing name gets a `_1`, `_2`… suffix instead of being
  overwritten,
- if the rotation fails (e.g. rename not permitted), an error is logged, writing continues to the
  old file and the next attempt is made after it grows by another 10%,
- new option `--csv-max-size` (MB, default 5, 0 disables rotation),
- the UI shows the name of the file currently written.

Tested on the real meter with `--csv-max-size 0.002` in both modes: the auto-named file
continued in a new file with a new timestamp; `mine.csv` was renamed to
`20261103_205629_mine.csv` and a new `mine.csv` with a header was created.

## 10. Request

> in this case "YYYYmmdd_HHMMSS_<name>"  <name> should be at the begining of a filename

The rotated user file is now named `<name>_YYYYmmdd_HHMMSS.<ext>`, e.g. `mine.csv` →
`mine_20261103_205932.csv` (the extension stays at the end).

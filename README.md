> [!WARNING] 
> This is fully slopped. I didn't read the code, and have no guarantees it will work on your cluster or won't break it or something. It worked for me, but use with caution.

This is a configuration program for the Corsa E instrument cluster using the ESP32 and MCP2515. It lets you edit the configuration of it and change the VIN to do a retrofit of the uplevel cluster to your Corsa. It is heavily based on [CarCluster](https://github.com/r00li/CarCluster).

To use, connect the MCP board to your ESP like described [here](https://github.com/r00li/CarCluster/blob/main/Misc/README_WIRING_JUMPERS.md), then connect the cluster using [this](https://github.com/r00li/CarCluster/blob/main/Misc/README_WIRING_OPEL_CORSA_E.md) guide.

I have tested this with my own cluster on the desk, and after swapping the VIN it fully works in my car.

## Slopped README below

# Corsa E IPC — graphical configuration editor

ESP32 firmware for reading and editing Opel Corsa E instrument cluster
configuration over single-wire GMLAN. It provides a local web interface and USB
serial console, with VIN editing, security unlock, NVS backups and restores,
write verification, and CAN logging.

## Setup

Install the ESP32 Arduino core (the compilation checks use version **3.3.12**).
The MCP_CAN driver is included in this project.

Open **CorsaEIPC.ino** in Arduino IDE, select **ESP32 Dev Module**, and upload.
Connect to Wi-Fi **CorsaE-IPC**, password **corsa-ipc-setup**, then open
**http://192.168.4.1**. USB serial uses **921600 baud**, newline termination.
All browser assets and option descriptions are built into the firmware.

## Security access

Security access uses algorithm 108 in `Key108.h`. Manual key calculation and
one-shot unlock use the same function. Each unlock uses the current seed
returned by the IPC.

## Read, select options, send

1. Select one of the nine named configuration groups and press **Read selected DID**.
   This reads that DID once. Opening the page or changing the selection sends no CAN request.
2. Browse or search the option descriptions. Checkboxes show current on/off bits.
   Multiple changes accumulate locally; searching does not discard them.
3. The **Your changes** panel lists every modified option and its before/after value.
   **Discard local changes** returns to the loaded values without sending CAN traffic.
4. Review the list, tick its confirmation and press **Send changes & verify**.
   The ESP32 checks that the IPC still matches the loaded data, saves a pre-write
   NVS backup, performs one immediate algorithm 108 seed/key exchange, writes the
   complete DID, then reads back and compares every byte.
5. Success updates the editor from verified readback. When finished, press
   **Stop / allow sleep**. Reboot/cycle ignition as appropriate, then re-read to
   check persistence; readback alone does not prove nonvolatile storage.

The editor exposes **670 named controls** covering all **742 named bits** in the
supplied definitions. Multi-bit fields are grouped into dropdowns with raw physical
mask values. Their semantic value labels are not documented, so none are invented.
All **570 unknown bits** remain hidden/preserved and are protected on the ESP32,
not only by the browser. Family tags are shown as documentation, not as a guarantee
that a function exists on the attached cluster. Enabling a feature does not add hardware.

The table below lists the **catalogue layouts**, not an enforced reply length.
The editor uses the IPC's actual returned data length (1–32 bytes for configuration;
VIN stays exactly 17). Options beyond the returned data length are hidden, and
writes and backups preserve that length. A changed length
between loading, live comparison or verification blocks the write/verification;
no extra bytes are padded in or silently truncated.

| DID | Bytes | Configuration group |
| --- | --- | --- |
| 02 | 32 | Messages and warnings |
| 03 | 32 | DEF and extended warnings |
| A1 | 20 | HUD, navigation and menus |
| A2 | 12 | DTC reporting mask |
| A3 | 8 | Display and menus |
| A4 | 11 | Vehicle features |
| A5 | 32 | System configuration |
| A6 | 12 | Indicators |
| A8 | 5 | Units and gauges |

Every loaded DID has a device-side edit token. Sending consumes it before any bus
operation. A failed/uncertain write, stopped session, controller reset or IPC reboot
requires a new read. The server rejects unknown-bit changes, incorrect payload
lengths, stale tokens and data that changed since loading. Repeated HTTP submissions
cannot silently repeat the same write. The pre-write backup can be viewed with
`previous A3` (substitute the DID), and downloaded in the console log.
Saved backups retain their actual length; a restore is blocked if the live DID
has a different length from the saved snapshot.

## VIN and existing console commands

VIN remains separate: **Read VIN**, enter the VIN, **Preview VIN edit**, review the
console preview, **Unlock once · algorithm 108**, then **Write previewed VIN**. The
existing NVS backup, staging and exact readback checks still apply.

Advanced controls retain full backup, saved snapshot, restore preview, CAN status,
trace and console commands. Type `help` for the full list. Numeric arguments are
hexadecimal. `bit`/`mask` remain console compatibility tools; the normal editor no
longer asks for byte indexes, bit positions or masks.

Legacy VIN finalization commands remain explicit console actions. They are not
added automatically to VIN or option writes.

## Quiet bus behaviour

Boot, browser catalogue loading, option editing and console-log polling send no
CAN requests.

`unlock` requests one fresh seed and immediately sends one algorithm 108 key. Zero
seed means already unlocked and produces no key transmission. No tester-present,
optional session setup or repeated key is inserted. The graphical Send workflow
uses this once per write; it never retries an unlock or write.

Manual `seed`, `calc-key` and `key HHHH` are still available. They do not implicitly
enable keepalive. Explicit `session` enables one-second tester present; it stops
on a failed diagnostic request or 30 seconds without a diagnostic command.
`stop` stops future keepalive and clears local staged/loaded edits and
security without sending an IPC command. It cannot interrupt a synchronous request
already executing or recall a frame queued by the MCP2515.

Security uses a two-second initial response timeout. Ordinary reads/writes use
a ten-second initial response timeout. NRC 78 waits
for a final response, without retransmission, within a 30-second overall deadline.
Backup stops on its first failed read. On timeout, preserve the log and read before
another write. NRC 36/37 indicates the IPC attempt limit/delay; its duration is unknown.

## Hardware and logging

Defaults: MCP2515 **8 MHz**, **33.3 kbps**,
standard request/reply IDs **24C/64C**, CS **5**, SCK **18**, MISO **19**, MOSI **23**,
INT **2**. Use an interface compatible with single-wire GMLAN. The MCP2515 alone
cannot reproduce a J2534 SWCAN high-voltage wake flag.

The IPC wiring is IPC pin 3 = GMLAN, 7 = battery +12 V,
8 = ignition +12 V, 19 = ground. Verify connector orientation against the pinout
for the attached cluster.

Trace is on by default: raw TX attempts/results, RX frames, ISO-TP flow control,
consecutive frames and late/unsolicited replies. The unchanged driver has a 2500-us
TX wait, so code 7 can mean a frame remains queued; the firmware awaits its reply
without resending. Hardware retransmissions/error frames are not separately captured.
The browser polls a 32-KiB log ring; **Download log** exports fetched output. USB serial
streams while requests execute. Browser logs arrive after synchronous commands finish.

## Development checks

The included checks use tools already installed on your machine:

- `python3 check_compile.py` validates all 65,536 algorithm 108 keys and configuration
  edit permissions, then builds the firmware using Arduino CLI. It requires Python 3,
  `g++`, Arduino CLI and the ESP32 core. Outputs stay under `build/`.
- `npm test` runs the offline browser checks. It requires Node.js, the Playwright
  dependency declared in `package.json`, and its Chromium browser. Set
  `PLAYWRIGHT_CHANNEL=chrome` to use an installed Chrome instead.

Browser tests use mocked HTTP responses and synthetic configuration data; they
send no CAN traffic. They cover editing, confirmation, unknown-bit preservation,
write failures, restart invalidation, variable payload lengths and mobile layout.

## Validation status

This is experimental firmware. Automated checks cover compilation and offline
logic; hardware timing, option support, backup/restore and persistence across IPC
reboots need validation on the target hardware. Successful readback confirms the
returned bytes, not persistence after power loss.

## Project files

| File | Purpose |
| --- | --- |
| `CorsaEIPC.ino` | Arduino entry point |
| `Firmware.h` | CAN diagnostics, configuration writes, storage and HTTP endpoints |
| `WebUI.h` | Embedded web interface |
| `Options.h`, `OptionCatalogue.h` | Configuration bit definitions and browser catalogue |
| `OptionEdit.h` | Configuration length and edit validation |
| `Key108.h` | Security key calculation |
| `TraceCan.h`, `src/MCP_CAN/` | CAN logging adapter and bundled driver |

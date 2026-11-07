# Anycubic Photon Mono M5s — Internal Serial Interface

Reverse-engineering notes for the UART link between the Wi-Fi/network board and the
motion mainboard. Derived from firmware `1.0.2.7` (`ota-MonoM5s-1.0.2.7-150EEED1-20230713.acf`)
by static analysis of `/usr/sbin/comm` (MIPS32 LE, uClibc) plus live inspection of the
running device.

> Status: **work in progress.** Transport, framing, and the command map are confirmed by
> disassembly. The checksum algorithm, exact `STARTPRINT` field offsets, and response-frame
> layout are not yet fully verified — see [Open questions](#open-questions).

## Architecture

The printer is a two-processor design:

- **Network board** — MediaTek MT7688 (ramips), OpenWrt / Linux 3.18.29. Runs the
  `/usr/sbin/comm` daemon, which bridges the cloud/LAN (MQTT + HTTP) to the mainboard.
  Has no large storage of its own (~7 MB MTD flash for the OS; prints are staged on the
  inserted USB stick at `/tmp/run/mountd/mmcblk0p1`).
- **Motion mainboard** — ChiTu-style ARM controller. Runs the touchscreen UI and motion,
  and owns the actual print pipeline. It is a **pure responder** on the serial link: with
  `comm` paused it transmits nothing, so all exchanges are initiated by the network board.

The two boards are connected by a UART. `comm` drives it from a dedicated receive thread
(`rev_uart_data`) and a transmit queue with retransmission.

## Transport

| Property | Value |
|----------|-------|
| Device   | `/dev/ttyS1` (on the MT7688 / Linux side) |
| Baud     | **115200** |
| Framing  | **8N1** (8 data bits, no parity, 1 stop) |
| Open flags | `0x882` (`O_RDWR | O_NONBLOCK | …`) |

Confirmed in the port-open function at `0x430a14`:

```
open("/dev/ttyS1", 0x882)                         ; fd cached at global 0x53ccc0
SetupSerial(fd, 0x1c200 /*=115200*/, 8, 'N')      ; 115200 8N1
```

`SetupSerial` is at `0x41b808`; it configures termios (cflag data bits from arg 3, parity
from arg 4 = `'N'`/`'O'`/`'E'`).

### Reliability layer

Outgoing messages are not sent raw — they pass through a linked-list queue:

- `insertUartMessageToLink(command, content, len, timestamp)` — enqueue
- `messageQueueOut` — pop/send the next message
- `eraseUartMessageToLink` — dequeue on acknowledgement
- `uartTimeoutMessageHandle` — **retransmits** on timeout (`"timeout msg sendconut++"`,
  compares `curtime` vs `msgtimestap` with a gap threshold)

So the link is an **ACK'd, retried** channel keyed by the command byte, not fire-and-forget.

## Frame format

Recovered from the RX scanner (`0x431d28`) and the frame consumer/dispatcher (`0x431880`).

```
 offset  size  field
 ------  ----  -------------------------------------------------------------
   0      1    0x44  'D'   | magic header "DM"
   1      1    0x4D  'M'   |
   2      2    command, 16-bit little-endian (valid range 0x20–0x47)
                 - low-byte high nibble = class (0x00/0x10/0x20/0x30/0x40)
   4      1    dir: 0 = request to be handled locally;
                    nonzero = response/other direction (forwarded, not dispatched)
   5..    n    payload — length is fixed per command
                 (e.g. STARTPRINT frame total = 0x101 = 257 bytes)
```

Parser behaviour:

1. Slide through the RX buffer looking for `0x44 0x4D` (`"DM"`).
2. Read the 16-bit command at offset 2.
3. If `dir` (offset 4) `== 0`, dispatch `(command - 0x20)` through a 40-entry jump table
   at **`0x45c800`**; otherwise dispatch `(command - 0x21)` through a second 39-entry table
   at **`0x45c8a0`** (the answer/response path — see [Response handlers](#response-handlers-dir--0)).
4. Each command handler knows its fixed frame length and advances the cursor past the
   whole frame before scanning resumes.

## Command map

Jump table `0x45c800`, indexed by `command - 0x20` (commands `0x20`–`0x47`). Names are the
handlers' own debug strings.

| Cmd  | Handler    | Meaning |
|------|------------|---------|
| `0x20` | `0x4318f4` | `SET_PARAMETER` |
| `0x24` | `0x431904` | `SET_PARAMETER` (variant) |
| `0x27` | `0x431938` | `FILELIST_REPORT` |
| `0x28` | `0x431948` | `FILELIST_REPORT` (variant) |
| `0x29` | `0x4319bc` | `STARTPRINT_REQUEST` — struct len `0x101` (257 B) |
| `0x2b` | `0x431a3c` | `Z_MOVE_REQUEST` |
| `0x2c` | `0x431a70` | `EXPOSURE_REQUEST` |
| `0x2d` | `0x4319cc` | `STARTPRINT_REQUEST` (variant, len `0x101`) |
| `0x2e` | `0x4319dc` | `STARTPRINT_REQUEST` (variant, len `0x101`) |
| `0x2f` | `0x4319ec` | `STARTPRINT_REQUEST` (variant, len `0x101`) |
| `0x42` | `0x431aa4` | `SELFCHECK_REQUEST` |
| `0x43` | `0x431ad8` | `RESIDUAL_REQUEST` |
| `0x44` | `0x431b0c` | `RELEASEFILM_REQUEST` |
| `0x45` | `0x431b40` | `MACHINEFUNC_REQUEST` |
| `0x47` | `0x431b74` | `SETMACHINEFREE_REQUEST` |

Unused / shared default handler (`0x431ce0`): `0x21`–`0x23`, `0x25`–`0x26`, `0x2a`,
`0x30`–`0x41`, `0x46`.

### Response handlers (`dir != 0`)

Jump table `0x45c800`'s sibling at **`0x45c8a0`**, indexed by `command - 0x21`:

| Cmd | Case | Handler | Meaning |
|-----|------|---------|---------|
| `0x21` | `0x431bf0` | `0x4165b4` | |
| `0x22` | `0x431c00` | `0x424b44` | |
| `0x23` | `0x431c10` | `0x42615c` | **`do_answer_emmc_change_requet`** — see [eMMC handover](#emmc-handover-command-0x23) |
| `0x25` | `0x431c20` | `0x42a324` | |
| `0x2a` | `0x431c30` | `0x42a374` | |
| `0x2b` | `0x431c40` | `0x42a514` | |
| `0x2c` | `0x431c50` | `0x42a6b0` | |
| `0x43` | `0x431c60` | (inline) | `RESIDUAL_REQUET_COMMAND` |
| `0x44`, `0x45`, `0x47` | `0x431c94`… | | |

## eMMC handover (command `0x23`)

The touchscreen's "Local" storage is an internal eMMC shared by both boards through a mux on
**GPIO0** (`1` = MT7688/Linux side). The mainboard must be asked to let go of it before GPIO0
is raised — raising GPIO0 alone makes the eMMC fail to enumerate
(`mtk-sd: no support for card's volts`, `error -22`).

### Request — `emmc_change_request(int to_linux)` @ `0x414bec`

Builds a **20-byte** frame, `write()`s it straight to the ttyS1 fd (global `0x53ccc0`, under
mutex `0x540750`), then also queues it via `insertUartMessageToLink` for retransmit-until-ACK:

```
offset: 0  1  2  3  4  5  6  7  8 .. 19
bytes : 44 4D 23 00 00 00 0B OO 00 .. 00
        "DM"  cmd   dir    ?  owner  (zero padding — no checksum)
```

- `OO` = **`00` → give the eMMC to Linux** (`to_linux != 0`), **`01` → give it back to the
  mainboard** (`to_linux == 0`).
- Byte 6 is always `0x0B`. Bytes 8–19 are zero (buffer is `memset`; nothing computes a
  checksum), which also answers open question 1 for this frame.

Ready-to-send frames:

```
to Linux     : 44 4D 23 00 00 00 0B 00 00 00 00 00 00 00 00 00 00 00 00 00
to mainboard : 44 4D 23 00 00 00 0B 01 00 00 00 00 00 00 00 00 00 00 00 00
```

### Answer — `do_answer_emmc_change_requet` @ `0x42615c`

Mainboard replies with cmd `0x23`, `dir != 0`:

| `[7]` | `[8]` | comm's action |
|-------|-------|---------------|
| `0` | `0` | `emmc_state = 1` (log `update emmc state:1`) → Linux may mount |
| `0` | `1` | `emmc_state = 0`; re-checks files / stops an in-flight download task |
| `≠0` | `0` | (mainboard-initiated ask) comm re-sends `emmc_change_request(1)` |
| `≠0` | `1` | (mainboard-initiated ask) comm sends `emmc_change_request(0)` |

State lives in globals under rwlock `0x540788`: `emmc_state` `0x53ccce` (byte), `filecheck`
`0x53ccc8`, `otafilecheck` `0x53ccc4`; getter `0x40e13c`, setter `0x40e1c8`.

### Full sequence as comm does it

Attach (`check_emmc_mount` @ `0x4294b8`, called from the cloud-download path):

1. `emmc_change_request(1)` (sent earlier by the job handler `0x427cb8`).
2. Poll `emmc_state` every ~1 s, up to 39 tries, until the answer sets it to 1.
3. `gpio_high(0)` (`0x402390`) → `sleep(3)` → `sync` → `umount -l /dev/mmcblk0p1` →
   `mount -t vfat -o iocharset=cp936 /dev/mmcblk0p1 /tmp/run/mountd/mmcblk0p1` → `df` check.

Release (`0x4296fc`, `0x42ece0`, `0x42f65c`, and at start-up in `0x430a14`):

1. (unmount)
2. `gpio_low(0)` (`0x4023c0`) → `sleep(1–2)`
3. `emmc_change_request(0)`

Observed live (cloud send, 2026-11-08): job arrives → small UART burst → GPIO0=1 ~1.2 s later →
eMMC enumerates 0.5 s after that → … → GPIO0=0 → large UART burst (mainboard re-reads its
file list). See also `docs/http_api.md` § Storage handover.

GPIO helpers: `0x402274` `gpio_write(n, "0"|"1")`, `0x402320` export+direction out+high,
`0x402390` high, `0x4023c0` low.

## Download progress (command `0x26`)

Drives the touchscreen's download progress bar. Built by `0x414cf8 report_download(pct, state)`
and `write()`n directly (12 bytes, **not** queued/retransmitted):

```
offset: 0  1  2  3  4  5  6  7  8  9  10 11
bytes : 44 4D 26 00 00 00 03 PP SS EE 00 00
                             PP = percent 0..100
```

`state` → `(SS, EE)`: 1→`(1,0)` downloading, 2→`(3,0)` done, 3→`(3,1)` MD5 mismatch,
4→`(2,0)`, 5→`(4,0)`, 6→`(5,0)`, 7→`(6,0)` failed/cancelled, 8→`(7,0)`; other → `(0,0)`.

Senders:
- Reporter thread `0x42d204`: while the "downloading" flag (`0x53c2b6`) is set, reads progress
  (`0x53c2b8`), publishes it over MQTT and sends `0x26 (pct, 1)`, then `sleep(5)` — the
  "+2 UART IRQs every ~5 s" seen during a cloud download.
- Cloud job handler `0x42f65c`: `(100, 2)` on success (before releasing the eMMC); 4/5/6/7/8 on
  the various failure paths. `my_download_file` `0x42b310`: `(100,2)` / `(pct,3)` / `(pct,7)`.

Related: `0x4148f4` sends cmd **`0x12`** (95 bytes, `do_ask_report_download_ota_file`) — the
same idea for firmware downloads: `[7]` = flag, `[8]` = status, `[9..]` = firmware name,
`[0x51]` = progress.

### Known payload structs

- **`STARTPRINT_REQUEST` (`0x29`, `0x2d`–`0x2f`)** — the `ptfun_request` struct, 257 bytes:
  `{ cmd, file (filename string), disk (int), taskid (int) }`. Debug strings:
  `"send->ptfun_request.file:%s disk:%d taskid:%d"`. Exact field offsets within the 257
  bytes are not yet mapped.
- **Responses** carry `ptfun_request.code` and `ptfun_request.status`
  (`"pmsg_recv->ptfun_request.code:%02x ... status:%02x"`); the print state machine keys on
  `taskid` (`"do_jug_print_action check_taskid:%d status:0x%02x code:%02x"`).

## Interaction with the print flow

- Files to print live on the inserted USB stick at `/tmp/run/mountd/mmcblk0p1/`
  (cloud/LAN prints are downloaded by `comm` into `.../mmcblk0p1/tmp/`).
- `comm` reports the file list to the mainboard via `FILELIST_REPORT` and starts a job with
  `STARTPRINT_REQUEST` referencing a filename + `taskid`.
- Live current-job metadata on the Linux side: `/usr/sbin/print.json`,
  `/usr/sbin/print_msg.txt` (empty when idle).

## Open questions

1. **Checksum** — the dispatcher does not verify a checksum; it is likely validated in the
   `rev_uart_data` byte-assembly thread before frames reach the parser. Algorithm (XOR /
   sum / none) and position (trailing byte?) still to confirm.
2. **`STARTPRINT_REQUEST` field offsets** inside the 257-byte struct.
3. **`dir` semantics** and full response-frame layout (`code`/`status` placement).
   (Partly answered: `dir != 0` frames go through the response table at `0x45c8a0`.)
4. **Variable-length frames** (e.g. `FILELIST_REPORT`) — where the length field is, if the
   payload isn't fixed.

## Method / reproducing

- Firmware carved from the `.acf` (unsigned container: `HEADER` + CRC32 partition table).
  Partition 0 = uImage (MIPS kernel) with a SquashFS rootfs appended at `+0x129abd`;
  `/usr/sbin/comm` is the relevant binary.
- Static analysis: capstone (MIPS32 LE), base vaddr `0x400000`
  (`.text` @ `0x401f60`, `.rodata` @ `0x4531b0`). Command names come from the handlers'
  `0x434f88(line, fmt, argc, name)` debug-log calls.
- Live device: root over SSH; mainboard is silent unless polled (verified by SIGSTOP-ing
  `comm` and capturing `/dev/ttyS1` — 0 bytes).

### Safe live-probe plan (not yet executed)

To validate the frame on the wire, pause `comm` (keeps the port's termios so baud stays set)
and send a **read-only** command, then capture the reply:

```sh
kill -STOP "$(pidof comm)"      # freeze the owner; port stays configured at 115200 8N1
# write a FILELIST_REPORT (0x27) or SELFCHECK (0x42) frame to /dev/ttyS1, read response
kill -CONT "$(pidof comm)"      # resume
```

Avoid `STARTPRINT` / `Z_MOVE` / `EXPOSURE` until the format is confirmed — those move
hardware.

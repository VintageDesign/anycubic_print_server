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
   at **`0x45c800`**; otherwise forward the frame (response path).
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

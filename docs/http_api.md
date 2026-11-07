# HTTP Print API

A small JSON API served by the printer's stock `uhttpd` (port 80), implemented as Lua CGI
scripts. Source lives in [`device/`](../device); install with `./device/install.sh [host]`.

```
device/printapi.lua            -> /usr/lib/lua/printapi.lua   shared helpers
device/www/cgi-bin/api/*       -> /www/cgi-bin/api/*          endpoints
```

## Install / uninstall

```sh
./device/install.sh              # host defaults to the `anycubic` ssh alias
./device/install.sh --uninstall  # restores the stock uhttpd config, removes files
```

The installer backs up `/etc/config/uhttpd` to `/etc/config/uhttpd.orig` (once), then sets:

| uci key | stock | ours | why |
|---|---|---|---|
| `uhttpd.main.script_timeout` | `60` | `900` | large `.pm5s` uploads over Wi-Fi take longer than 60 s |
| `uhttpd.main.redirect_https` | `1` | `0` | keep plain HTTP; no self-signed cert dance |

LuCI at `http://<printer>/` keeps working.

## Constraints of the stock uhttpd

- **Only `GET`, `POST`, `HEAD` (and `OPTIONS`) are accepted.** `PUT`/`DELETE` are rejected by
  uhttpd itself with an HTML `400 Bad Request` before the script runs, so uploads and deletes
  are `POST`.
- `max_requests 3`: at most 3 concurrent CGI requests (one upload + polling is fine).
- `rfc1918_filter 1`: requests from public IPs to the private address are dropped.
- **No authentication** — anyone on the LAN can upload/delete.

## Conventions

- All responses are JSON with `"ok": true|false`; errors carry `"error": "<message>"`.
- Files live on the printer's internal "Local" eMMC (top level only). The eMMC normally belongs
  to the mainboard; `upload`, `delete` and `files?refresh=1` **borrow it** for the duration of the
  request (see [How storage is borrowed](#how-storage-is-borrowed)). While borrowed, the
  touchscreen's Local tab is unavailable (~10 s plus transfer time).
- `files` without `refresh` serves a **cached listing** (`/tmp/printapi_files.json`), refreshed
  by every upload/delete/refresh, so it is cheap to poll. The cache lives in RAM and is empty
  after a printer reboot until the first refresh.
- Allowed filenames: letters, digits, space and `. _ - ( ) [ ] + ,`; no leading `.`; max 200 chars;
  extension `.pm5s`, `.pwmb` or `.pmx2`.
- CORS: `Access-Control-Allow-Origin: *`, so a browser-based UI can call the API directly.
- Storage-borrowing requests return **`503`** with a reason when the storage can't be borrowed:
  a cloud job is in progress, another request holds it, or the mainboard refused (e.g. printing).

## Endpoints

Below, `H=http://<printer>/cgi-bin/api`.

### `GET /status`

Cheap; never touches the eMMC.

```sh
curl -s $H/status
```
```json
{"cloud_job":false,"files_updated":1794074119,"local_print":null,"msg":null,"ok":true,"print":null,
 "storage":{"busy":false,"mounted":false,"owner":"mainboard"},"uptime":140993.7}
```

| field | source |
|---|---|
| `cloud_job` | `true` while `/usr/sbin/print_msg.txt` is non-empty (comm is receiving a cloud job) |
| `print` | `/usr/sbin/print.json` (embedded as JSON if it looks like JSON, else a string; `null` if missing) |
| `local_print` | `/usr/sbin/local_print.json` (same rules) |
| `msg` | `/usr/sbin/print_msg.txt` (same rules) |
| `storage.owner` | `"mainboard"` or `"linux"` (GPIO0) |
| `storage.busy` | an API request currently holds the storage |
| `storage.mounted` | eMMC is mounted on the Linux side right now |
| `files_updated` | epoch of the cached listing (`null` if none); poll this to know when to re-fetch `files` |
| `uptime` | seconds, from `/proc/uptime` |

`status` also self-heals: it resumes `comm` if a request died while it was paused, and releases
storage held by a request that died more than 20 min ago.

The print files are written by Anycubic's `comm` daemon and are passed through unmodified; their
format is not documented yet (see [Samples](#samples-todo)).

### `GET /files[?refresh=1]`

```sh
curl -s $H/files              # cached, instant
curl -s "$H/files?refresh=1"  # borrows the eMMC, ~10 s
```
```json
{"ok":true,"refreshed":false,"files":[{"mtime":1794073110,"name":"Unnamed(4).pm5s","size":41616298}],
 "free":3785261568,"total":3878502912,"updated":1794074119}
```

Newest first. `free`/`total` are bytes; `updated` is when the listing was taken. With no cache
yet, `files` is `[]` and `updated`/`free`/`total` are `null`.

### `POST /upload?name=<file>[&overwrite=1]`

Body is the **raw file bytes** (not multipart). Borrows the eMMC for the whole transfer.

```sh
curl -s --data-binary @benchy.pm5s -H 'Content-Type: application/octet-stream' \
     "$H/upload?name=benchy.pm5s"
```
```json
{"bytes":48211022,"name":"benchy.pm5s","ok":true}
```

The server spends ~8 s switching the storage before it starts reading the body (the client's
data waits in the socket buffer meanwhile). The body is streamed to `.<name>.part`, the byte
count is checked against `Content-Length`, then it is synced and renamed into place, so a
half-finished upload never appears under the real name.

| status | meaning |
|---|---|
| `201` | stored |
| `400` | bad name / extension, or body shorter than `Content-Length` |
| `405` | not `POST` |
| `409` | file exists (pass `overwrite=1`) |
| `411` | missing `Content-Length` |
| `503` | storage could not be borrowed (reason in `error`) |
| `507` | not enough free space (checked against the cache first, then the real filesystem) |
| `500` | write/rename failure |

### `POST /delete?name=<file>[&name=<file2>...]`

Several files are deleted in a single storage handover.

```sh
curl -s -X POST "$H/delete?name=benchy.pm5s&name=old.pm5s"
```
```json
{"deleted":["benchy.pm5s"],"failed":[],"missing":["old.pm5s"],"ok":true}
```

`200` if anything was deleted, `404` if every name was missing, `500` if a removal failed,
`400`/`503` as above.

### How storage is borrowed

Implemented in `device/printapi.lua` (`with_storage`), mirroring comm's own sequence
(details in [serial_interface.md § eMMC handover](serial_interface.md#emmc-handover-command-0x23)):

1. Refuse if a cloud job is running or GPIO0 is already high; take `/tmp/printapi.lock`
   (waits up to 30 s for a concurrent request).
2. **Pause `comm`** (SIGSTOP), send the 0x23 "give eMMC to Linux" frame on `/dev/ttyS1`, read
   the mainboard's answer, resume `comm`. Only an answer of `[7]=0,[8]=0` ("released")
   continues — so a mainboard that is printing from the eMMC can refuse and nothing is switched.
3. Raise GPIO0, wait for `/dev/mmcblk0p1` (≈1 s), let mountd mount it (or mount it ourselves),
   then wait 2 s for the kernel to finish probing.
4. Run the operation, snapshot the listing into the cache.
5. `sync`, `umount`, lower GPIO0, wait 2 s, send the "give eMMC back" frame (comm paused again
   while the answer is read), release the lock.

The kernel logs an I/O error on `mmcblk0boot1`/`boot0` at removal time; those are hidden boot
partitions we never touch, and comm's own handovers produce similar noise.

## Samples (TODO)

Capture `print.json`, `local_print.json` and `print_msg.txt` during a real print (poll
`/status`) and paste them here so `status` can grow parsed fields (progress, layer, ETA).

## Storage handover (observed from a cloud send)

The touchscreen's **Local** storage is an internal 3.64 GiB eMMC (`4FTE4R`, FAT, with
`boot0`/`boot1` partitions). It is **shared** between the motion mainboard and the MT7688
Linux board through a mux on **GPIO0** (`/sys/class/gpio/gpio0/value`, `1` = Linux side).
The front USB port is wired to the mainboard only; Linux never sees the USB stick.

Most of the time the mainboard owns the eMMC, so `files`/`upload`/`delete` return
`503 storage not mounted`. Captured during a cloud send (printer uptime clock, seconds):

| t | event |
|---|---|
| 140003.7 | `comm` writes `/usr/sbin/print_msg.txt` (job id); small burst of UART IRQs on the mainboard link |
| 140004.9 | GPIO0 → 1 |
| 140005.4 | eMMC enumerates (`mmc0: new high speed MMC card`), mountd mounts `mmcblk0p1` |
| 140008–9 | unmount + remount (comm's own `mount -t vfat -o iocharset=cp936 ...`) |
| 140009–053 | download written to `mmcblk0p1/tmp/<name>.pm5s`, then moved to the root; UART shows only a ~5 s heartbeat |
| 140053.9 | `print_msg.txt` cleared |
| 140055.2 | GPIO0 → 0, eMMC removed |
| 140056.9 | large UART burst (~410 IRQs), presumably the mainboard re-reading the file list |

Raising GPIO0 by hand **without** comm's preceding serial exchange fails
(`mtk-sd: no support for card's volts`, `error -22`), so the mainboard must be asked to release
the eMMC first — that request (serial cmd 0x23) is what the API now sends itself; see
[How storage is borrowed](#how-storage-is-borrowed).

Contents seen on the eMMC: `*.pm5s` print files, `tmp/` (in-progress cloud downloads),
`history.txt`, `otapackage/`, and an OTA image `ota-MonoM5s-1.0.4.5-*.acf`.

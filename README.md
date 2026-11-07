# Anycubic Print Server
Provides a locally accessible print server for the Anycubic M5s.

## Installation
There are two parts: a small HTTP API that runs on the printer's network board (`device/`), and a
Qt desktop app that talks to it (`app/`).

### Prerequisites
- Root SSH access to the printer with your public key installed.

### Printer API
From the repository root:
```
./device/install.sh root@<printer ip>   # or any other ssh destination
```

To check it later:
```
curl http://<printer ip>/cgi-bin/api/status
```
To remove the API and restore the stock uhttpd config:
```
./device/install.sh --uninstall [host]
```
The endpoints are documented in [docs/http_api.md](docs/http_api.md).

### Desktop app
Requirements: CMake 3.19 or newer, a C++17 compiler and Qt 6.2 or newer (Widgets and Network).
On Debian/Ubuntu:
```
sudo apt install cmake g++ qt6-base-dev
```
Build and run:
```
cmake -S app -B app/build
cmake --build app/build
./app/build/print-monitor
```
Enter the printer's IP or hostname in the host field; it is saved for next time. The app uploads
`.pm5s`, `.pwmb` and `.pmx2` files.

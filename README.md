# 📡 dsfemon

> A terminal-based DVB frontend monitor for Linux. It can be used as a more capable alternative to the basic `femon` tool from the `dvb-apps` package, especially on systems with many DVB frontends. `dsfemon` is designed for quick operational overview of tuner state, signal quality, tuning parameters, and service information in multistream backend environments, including IPTV backends.

![dsfemon main window](docs/screenshots/dsfemon-main.png)

## 🎯 Overview

`dsfemon` is a fork and modernized continuation of the original Femon DVB frontend monitor developed by David Seidl in 2012.

It shows a compact live overview of DVB tuner state in a terminal UI and is intended for quickly checking frontend lock state, signal quality, DVB tuning parameters, and basic demux/service information from Linux DVB devices.

## ✨ Features

- scans Linux DVB frontend devices under `/dev/dvb`
- displays frontend type, name, and lock/status flags
- shows signal and CNR/SNR values with terminal bars
- reads modern DVBv5 properties when available
- keeps legacy DVB ioctl fallbacks for signal and SNR bars
- displays DVB delivery system, frequency, bandwidth, symbol rate, FEC, and modulation
- reads PAT/PMT/NIT/SDT demux information and keeps multi-section NIT/SDT data stable
- supports paging when many frontends are present
- provides keyboard navigation with first/last item jumps
- provides a configurable monitor refresh interval
- shows a scrollable demux service detail screen with service type, audio/teletext/subtitle languages, provider, PCR PID, CA, and PMT stream information

## ⌨️ Controls

| Key | Action |
| --- | --- |
| `Up` / `Down` | select frontend detail row or service row |
| `Home` / `End` | jump to first or last frontend/service |
| `PgUp` / `PgDn` | switch page or scroll detail page |
| `Enter` | open demux detail |
| `Esc` | return from detail view |
| `q` | quit |

## 📦 Requirements

- Linux with DVB device support
- C++ compiler with C++11-era system headers
- `make`
- `pkg-config`
- `libncurses-dev` / `ncursesw`
- Linux DVB headers, usually provided by the system libc/kernel headers package

Debian/Ubuntu example:

```bash
sudo apt install build-essential pkg-config libncurses-dev
```

## 🔨 Building

```bash
make clean
make
```

The resulting binary is:

```text
./dsfemon
```

Useful maintenance checks:

```bash
make format-check
make -B
```

Optional local install:

```bash
sudo make install
```

## ▶️ Running

Start the monitor with the default broad adapter scan:

```bash
./dsfemon
```

Additional commands and useful arguments:

```bash
# Show command-line help
./dsfemon --help

# Show version
./dsfemon --version

# Scan only selected adapters
./dsfemon --adapters 0,2

# Limit the number of frontends scanned per adapter
./dsfemon --subadapters 1

# Change the monitor refresh interval in milliseconds, allowed range 100-60000
./dsfemon --interval 1000
```

The refresh interval controls both the ncurses redraw cadence and the background frontend status collection cadence. Demux/SI data is still read continuously by the per-device demux reader threads.

## 🗂️ Project Structure

```text
/
├── docs/
│   └── screenshots/
│       └── dsfemon-main.png      # main application screenshot
├── dsfemon.cpp                   # main ncurses loop, paging, and keyboard handling
├── command_line.*                # command-line options
├── device_discovery.*            # DVB device scanning and lifecycle
├── frontend_monitor.*            # DVBv5 property collection
├── frontend_status_cache.*       # background frontend status cache
├── frontend_status.*             # frontend status snapshot collection
├── frontend_view.*               # frontend/status rendering
├── demux_reader.cpp              # background PAT/PMT/NIT/SDT section reader
├── demux_detail_view.*           # fullscreen demux detail rendering and service navigation
├── demux_snapshot.cpp            # stable demux data copied for UI rendering
├── demux_view.*                  # demux/service summary rendering
├── si_parser.cpp                 # PSI/SI parser helpers
├── *_table.h                     # small PSI/SI table constants
├── ui_helpers.*                  # shared ncurses rendering helpers
├── ncurses_present.*             # terminal bar helpers
└── color.*                       # ncurses color pairs/macros
```

## Status

- ✅ modern build against `ncursesw`
- ✅ default adapter scan starts at adapter `0`
- ✅ DVBv5 properties are read individually for better compatibility with older drivers
- ✅ paging and keyboard navigation are implemented
- ✅ `Home` / `End` jump to first/last frontend or service
- ✅ demux detail shows a scrollable service table with service type, PMT/PCR PIDs, streams, audio/teletext/subtitle languages, CA, provider, and running status
- ✅ demux detail keeps the last valid snapshot visible during transient SI/PMT refresh gaps
- ✅ multi-section NIT/SDT caches keep network and service tables stable when broadcasts split them across sections

## 🙏 Acknowledgements

Special thanks to David Seidl, author of the original Femon DVB frontend monitor from 2012.

This project is based on his original work and continues it as a modernized open-source version for current Linux DVB systems.

## License

Permission was granted to continue and publish the project under the condition that open-source principles are preserved.

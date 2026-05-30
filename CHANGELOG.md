# Changelog

This summary describes the main user-visible and architectural changes made while reviving and modernizing the original 2012 femon/dsfemon codebase.

- Restored the project on current Linux systems and modernized the build to use `ncursesw`, `pkg-config`, a cleaner Makefile, strict compiler warnings, and the `dsfemon` binary name to avoid collisions with the distro `femon` package.

- Switched the default scan back to adapter `0` and added practical command-line options for daily use: `--help`, `--version`, `--adapters`, `--subadapters`, and `--interval`.

- Added clearer runtime diagnostics for missing DVB frontend devices and scan selections, plus safer shutdown through `q`/`Q`, Ctrl+C/SIGTERM handling, cursor restoration, and explicit frontend/demux cleanup.

- Modernized frontend monitoring around DVBv5 `FE_GET_PROPERTY` reads, including delivery system, frequency, symbol rate, bandwidth, FEC, modulation, signal/CNR statistics, BER/error counters, and compatibility handling for older drivers that reject some DVBv5 properties.

- Kept proven legacy signal/SNR ioctl reads as the source for stable percentage bars while displaying newer DVBv5 dB/statistic values alongside them when available.

- Reworked the main terminal UI with a fixed header, footer, page counter, frontend counter, refresh spinner, highlighted keyboard hints, hidden cursor, and paging support for systems with many frontends.

- Added fast keyboard navigation with `Up`/`Down`, `PgUp`/`PgDn`, `Home`/`End`, `Enter`, `ESC`, and `Q`, including support for terminals that report raw Home/End escape sequences.

- Improved the main service-list row so long channel lists are clipped safely, rotate in page-sized circular slices, avoid empty service-name gaps, and keep the terminal row filled instead of leaving unused space at the end.

- Added a fullscreen demux detail view with a scrollable service table showing service ID, type, PMT PID, stream count, audio languages, CA state, running status, and service name.

- Extended demux detail metadata with provider name, PCR PID, selected-service stream summaries, audio/teletext/subtitle languages, CA system/PID summaries, and readable labels for common DVB service and stream types.

- Added stable demux snapshots so UI rendering works from locked copies of NIT/SDT/PMT data rather than walking live demux buffers directly.

- Added multi-section NIT and SDT caches so network and service tables remain stable when broadcasters split metadata across several sections.

- Replaced endian-dependent PSI/SI bitfield parsing with explicit byte parsing for PAT, PMT, NIT, and SDT sections, including safer section-length, PID, descriptor-loop, and CRC-aware bounds handling.

- Hardened demux parsing against partial or missing PAT/PMT/NIT/SDT data, invalid PIDs, empty service names, and transient SI refresh gaps.

- Replaced detached worker threads with explicit start/stop/join lifecycles, added a background frontend status worker, and synchronized demux data access to avoid UI/thread races.

- Reduced memory usage by allocating large PID tables only for opened demux devices instead of every possible DVB slot.

- Refactored the old monolithic source into focused modules for command-line parsing, device discovery, frontend status collection, frontend rendering, demux reading, demux snapshots, SI parsing, detail rendering, shared UI helpers, and color/presentation helpers.

- Standardized formatting and naming across the codebase, removed stale legacy helpers, added concise file/function comments around non-obvious DVB and UI behavior, and introduced `clang-format`/`format-check` support.

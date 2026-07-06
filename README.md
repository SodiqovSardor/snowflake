<div align="center">

# ❄ snowflake

**Zero-dependency reverse-tunnel file sharing — one binary, no setup**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)
![Size](https://img.shields.io/badge/binary-51%20KB-brightgreen)
![Platform](https://img.shields.io/badge/platform-Linux%20|%20macOS-lightgrey)

<br>

```bash
snowflake send ./secret.doc serve once
# → http://yourip:8080/  ← open on any device, grab file, binary self-destructs
```

</div>

---

## ✨ Features

<table>
<tr>
<td width="50%">

**🔌 Zero Setup**  
Single binary, no dependencies, no config files, no npm install.  
`chmod +x` and go.

**⚡ Blazing Fast**  
Native POSIX sockets, 4096-byte streaming chunks, zero-copy relay.  
File data never touches disk on the relay.

**🔒 PIN Gate**  
`lock` mode generates a random 4-digit PIN. Visitors see a clean  
entry screen — no PIN, no files.

**💥 Ephemeral Mode**  
`once` mode automatically terminates the binary the nanosecond  
a file download completes. Perfect for one-shot sharing.

</td>
<td width="50%">

**📁 Single File or Directory**  
`snowflake send ./video.mp4 serve` — serves one file.  
`snowflake send ./docs serve` — serves a full directory listing.

**🎨 Clean Dark UI**  
Dark theme (#0d1117), monospace fonts, animated ❄ spinner,  
pulsing green status dot. File-type SVG icons.  
PIN entry card when locked.

**🌐 Dual Mode**  
• **Standalone** — embedded HTTP server, no relay needed  
• **Relay** — connect through `relay.js` for remote access

**📦 Tiny Binary**  
51 KB stripped. Zero external dependencies.  
Compiled with `g++ -std=c++17 -Os -s`.

</td>
</tr>
</table>

---

## 🚀 Quick Start

### Install

```bash
# Build from source (requires g++ ≥8)
git clone https://github.com/yourname/snowflake.git
cd snowflake
make size                    # 51 KB dynamic binary
cp build/snowflake ~/.local/bin/

# Or grab a pre-built release from the releases page
```

### Share a file

```bash
# Terminal 1 — serve a single file, melt after one download
snowflake send ./report.pdf serve once

# Open http://192.168.1.219:8080 on your phone → download → binary exits
```

### Share a directory

```bash
snowflake send ./photos serve
# http://yourip:8080/ — full directory listing with download buttons
```

### PIN-protected

```bash
snowflake send ./secrets serve lock
# [lock] PIN: 7391
# http://yourip:8080/ — enter PIN to unlock file list
```

### Quiet + locked + ephemeral (combo)

```bash
snowflake send ./docs serve lock once hide
# Silent, PIN-protected, self-destructs after first download
```

### Via relay (for remote access)

```bash
# On a public server
node relay.js

# On your machine behind NAT
snowflake send ./file serve --host public-server.com
# Visitors hit http://public-server.com:8080/
```

---

## 📖 Command Reference

```
snowflake send <PATH> [modifiers] [options]

Required:
  send <PATH>   file or directory to share

Modifiers (combinable in any order):
  serve    Start HTTP server or connect to relay
  once     Melt after first file download
  lock     4-digit PIN protects the web interface
  hide     Suppress all stdout output

Options:
  --host <addr>  Relay address (omit for standalone server)
  --port <num>   HTTP port (default: 8080)
  -h, --help     Show help
```

### Examples at a glance

| Command | Effect |
|---------|--------|
| `snowflake send . serve` | Serve current directory |
| `snowflake send file.iso serve once` | Single file, melt after first download |
| `snowflake send . serve lock` | PIN-protected directory |
| `snowflake send . serve lock once hide` | Silent, PIN, melt on first download |
| `snowflake send . serve --host example.com` | Relay mode (requires `relay.js`) |

---

## 🏗 Architecture

```
┌─────────────────┐       ┌─────────────────┐       ┌──────────────┐
│  Phone/Tablet   │──────►│  snowflake      │──────►│  Your Files   │
│  (browser)      │◄──────│  (C++ binary)   │◄──────│  (CWD)        │
└─────────────────┘  8080 └─────────────────┘  raw  └──────────────┘
                                │  TCP
                                ▼
                         ┌──────────────┐
                         │  relay.js     │ (optional, for remote)
                         │  :9000 / :8080│
                         └──────────────┘
```

In **standalone mode** (default), the C++ binary embeds a full HTTP server.  
In **relay mode** (`--host`), it connects to `relay.js` which forwards browser requests.

---

## ⚙️ Build Options

```bash
make size      # 51 KB — optimized for size (default)
make perf      # 67 KB — optimized for speed (-O3 -flto)
make static    # ~800 KB — fully static, portable to any Linux
make debug     # ~2 MB — debug symbols, -O0
```

Compiler: `g++ -std=c++17 -Os -s -o snowflake src/client.cpp`

---

## 🧪 Test Suite

```bash
bash test.sh
# 36 tests, 0 failures
```

Tests cover: standalone directory serving, single file mode, once mode  
(self-destruct), lock mode (PIN entry, 403 without PIN), hide mode  
(silent operation), combined modifiers, relay mode, path traversal  
blocking, 404 handling, and CLI error cases.

---

## 🔐 Security

- **Path traversal blocked**: any `..` or `/` in download paths returns `403 Forbidden`
- **PIN gate**: all content hidden behind a 4-digit PIN in `lock` mode
- **Zero disk writes on relay**: data streams in memory through the tunnel
- **Zero external dependencies**: no libcurl, no OpenSSL, no npm packages
- **SIGINT clean shutdown**: Ctrl+C terminates gracefully

---

## 📄 License

MIT © 2026 Sardor Sodiqov

See [LICENSE](LICENSE) for full text.

---

<div align="center">

**❄ snowflake — share files like they're nothing**

</div>

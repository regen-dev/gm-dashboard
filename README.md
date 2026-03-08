# GM Dashboard

A sci-fi "Good Morning" dashboard for Linux desktops. Written in C++17 with Qt6 and a plugin architecture — each card is an independent `.so` loaded via `dlopen()`, running in a sandboxed child process with Landlock + seccomp.

```
┌─────────────────────────────────────────────────────────────────┐
│                  GOOD AFTERNOON, RΞGΞN                          │
│              SATURDAY, MARCH 08, 2026                           │
│                    15:30:42 -03                                  │
│─────────────────────────────────────────────────────────────────│
│  ◈ WEATHER      │  ◈ GITHUB         │  ◈ SYSTEM                │
│                  │                    │                          │
│      ⛅          │  Repos       5     │  CPU    52°C             │
│    26°C          │  Stars       0     │  GPU    34°C             │
│  Partly cloudy   │  Forks       0     │  DDR5   46°C            │
│   Goiania        │  Followers   6     │  NVMe0  44°C            │
│                  │  Following   3     │  NVMe1  39°C            │
│  Humidity  70%   │  ──────────────    │  NVMe2  39°C            │
│  Feels     29°C  │  Contributions 13  │  ──────────────         │
│  Wind      4km/h │                    │  RAM 24/123 GB          │
│                  │                    │  ████░░░░░░░░░░  19%    │
│                  │                    │  Uptime  0d 7h 58m      │
│                  │                    │  Load  1.19/0.90/0.74   │
└─────────────────────────────────────────────────────────────────┘
```

Rendered in a custom frameless Qt6 WebEngine window with native system move/resize, position memory, and a QWebChannel bridge for the title bar.

## Features

- **Plugin system** — each card is an independent `.so` with a 4-function ABI
- **Ed25519 signing** — plugins are verified before `dlopen()` (blocks malicious constructors)
- **Landlock sandbox** — per-plugin filesystem + network ACLs based on declared capabilities
- **seccomp-BPF** — syscall blocklist per capability (undeclared = SIGKILL)
- **Process isolation** — `fork()` per plugin with 30s timeout
- **Encrypted config** — Argon2id + XSalsa20-Poly1305 vault for sensitive settings
- **Custom viewer** — frameless Qt6 WebEngine window with drag/resize/minimize via QWebChannel

## Quick Start

### Dependencies

```bash
# Ubuntu/Debian
sudo apt install g++ libsodium-dev libseccomp-dev libcurl4-openssl-dev

# Qt6 (one of):
#   Option A: System packages (Ubuntu 24.04+)
sudo apt install qt6-base-dev qt6-webengine-dev qt6-tools-dev qt6-webchannel-dev qt6-positioning-dev
#   Option B: Qt online installer (https://www.qt.io/download-qt-installer)
#   Installs to ~/Qt/<version>/gcc_64 — the Makefile default
```

### Build & Run

```bash
git clone https://github.com/user/gm-dashboard.git
cd gm-dashboard

# If using system Qt6, set QTDIR:
# export QTDIR=/usr/lib/x86_64-linux-gnu/qt6  # or wherever qmake6 lives

# First time: generate signing keypair
make build
./gm-sign keygen

# Build, sign plugins, install to ~/.local/, launch viewer
make run
```

After initial setup, `make run` is all you need.

### Makefile Variables

| Variable | Default | Override |
|----------|---------|---------|
| `QTDIR` | `~/Qt/6.10.0/gcc_64` | `make QTDIR=/usr/lib/qt6` |
| `PREFIX` | `~/.local` | `make PREFIX=/usr/local` |
| `ARCH` | `native` | `make ARCH=x86-64-v3` |
| `CXX` | `g++` | `make CXX=clang++` |

## How It Works

```
gm-dashboard (C++/Qt6)
  │
  ├─ Load marketplace public key (~/.config/gm-dashboard/marketplace.pub)
  ├─ Scan ~/.local/lib/gm-dashboard/*.so
  │   ├─ Verify Ed25519 signature (BEFORE dlopen — blocks malicious constructors)
  │   ├─ dlopen → extract metadata (gm_info)
  │   └─ Sort by display order
  │
  ├─ For each plugin (fork per plugin):
  │   ├─ Child: PR_SET_NO_NEW_PRIVS
  │   ├─ Child: Apply Landlock (filesystem + network ACLs based on caps)
  │   ├─ Child: Apply seccomp (syscall blocklist based on caps)
  │   ├─ Child: init(config_dir) → fetch() → write JSON to pipe → exit
  │   └─ Parent: read JSON from pipe (30s timeout, SIGKILL on timeout)
  │
  └─ Assemble HTML: base.html + card divs + plugin scripts + JSON data
     └─ Write /dev/shm/gm-dashboard.html → exec gm-viewer (Qt6 WebEngine)
```

## Security Model

Four layers of defense for safe third-party plugin loading:

| Layer | Technology | Purpose |
|-------|-----------|---------|
| **Signing** | Ed25519 (libsodium) | Only marketplace-signed `.so` files are loaded |
| **Landlock** | LSM (kernel ABI v7) | Per-plugin filesystem + network restrictions |
| **seccomp** | BPF syscall filter | Block dangerous syscalls per capability |
| **Isolation** | `fork()` per plugin | Crash/violation in one plugin doesn't affect others |

### Capabilities

Each plugin declares what it needs in `gm_info.caps_required`:

| Flag | Value | Grants |
|------|-------|--------|
| `GM_CAP_NETWORK` | `0x01` | Outbound TCP, DNS resolution, TLS |
| `GM_CAP_PROC` | `0x02` | Read `/proc` filesystem |
| `GM_CAP_SHM` | `0x04` | Read `/dev/shm` |
| `GM_CAP_EXEC` | `0x08` | Execute external programs |
| `GM_CAP_HOME_READ` | `0x10` | Read files under `$HOME` |

Undeclared capabilities are **enforced denied** — a plugin without `GM_CAP_NETWORK` that tries to open a socket gets `SIGKILL`.

## Plugins

| Plugin | Order | Caps | Source |
|--------|-------|------|--------|
| weather | 10 | `NETWORK` | [Open-Meteo API](https://open-meteo.com/) + Nominatim geocoding (libcurl) |
| github | 20 | `EXEC`, `NETWORK` | `gh api graphql` via popen |
| system | 90 | `PROC`, `SHM` | `/proc/uptime`, `/proc/meminfo`, `/dev/shm/argos-thermal` |

## Creating a Plugin

Each plugin is a `.cpp` file compiled to `.so` + a `.html` card renderer. The ABI is C-compatible (`extern "C"`), but plugins are written in C++/Qt6:

```cpp
// plugins/myplugin.cpp
#include "gm-plugin.h"
#include <QJsonDocument>
#include <QJsonObject>

static QByteArray jsonBuf;

extern "C" {

struct gm_info gm_info(void) {
    return {
        .api_version      = GM_API_VERSION,
        .id               = "myplugin",
        .name             = "MY PLUGIN",
        .version          = "1.0.0",
        .author           = "you",
        .order            = 50,
        .caps_required    = GM_CAP_NETWORK,
        .caps_description = "Network: fetch data from example.com",
    };
}

int gm_init(const char *config_dir) {
    // Read ~/.config/gm-dashboard/myplugin.conf if needed
    (void)config_dir;
    return 0;
}

const char *gm_fetch(void) {
    QJsonObject obj;
    obj["status"] = "ok";
    jsonBuf = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return jsonBuf.constData();
}

void gm_cleanup(void) {
    jsonBuf.clear();
}

} // extern "C"
```

```html
<!-- plugins/myplugin.html -->
<script>
(function() {
  var d = (GM.plugins || {}).myplugin;
  var el = document.getElementById('p-myplugin');
  if (!d) { el.innerHTML = '<div class="ph"><div class="ico">⊘</div><p>OFFLINE</p></div>'; return; }
  el.innerHTML = R('Status', d.status, 'cy');
})();
</script>
```

Build, sign, install:
```bash
g++ -O2 -fPIC -shared -o plugins/myplugin.so plugins/myplugin.cpp \
    $(pkg-config --cflags --libs Qt6Core) -I.
gm-sign sign plugins/myplugin.so
install -m755 plugins/myplugin.so ~/.local/lib/gm-dashboard/
install -m644 plugins/myplugin.so.sig ~/.local/lib/gm-dashboard/
install -m644 plugins/myplugin.html ~/.local/lib/gm-dashboard/
```

## Encrypted Configuration

Sensitive values (API keys, tokens) are stored in an encrypted vault:

```bash
# Initialize vault
gm-config init

# Store a value
gm-config set github.token ghp_xxxxxxxxxxxx

# Unlock (decrypts to /dev/shm/gm-cfg/ — tmpfs, never on disk)
gm-config unlock

# Lock (secure-wipes decrypted files)
gm-config lock
```

The dashboard auto-detects: checks `/dev/shm/gm-cfg/` first, falls back to plaintext config files.

## Configuration

Global config: `~/.config/gm-dashboard/config`
```
NAME=YourName
```

Per-plugin config: `~/.config/gm-dashboard/<id>.conf`
```
# weather.conf — pick one:
CITY=London               # Geocoded via Nominatim
# LAT=51.5074             # Direct coordinates (takes priority)
# LON=-0.1278
```

## File Layout

```
Source tree:
├── gm-dashboard.cpp      Main binary (plugin loader, sandbox, HTML assembler)
├── gm-viewer.cpp          Qt6 WebEngine frameless viewer
├── gm-sign.cpp            Ed25519 signing tool
├── gm-config.cpp          Encrypted config vault manager
├── gm-plugin.h            Plugin ABI header (v2, C-compatible)
├── base.html              HTML/CSS/JS template
├── Makefile               Build system
├── plugins/
│   ├── {weather,github,system}.cpp
│   └── {weather,github,system}.html
└── tests/                 QTest suites (207 tests, 100% coverage)

Installed to ~/.local/:
├── bin/{gm-dashboard,gm-viewer,gm-sign,gm-config}
├── lib/gm-dashboard/{*.so,*.so.sig,*.html}
├── share/gm-dashboard/base.html
└── share/icons/hicolor/.../gm-dashboard.{svg,png}

Runtime:
├── ~/.config/gm-dashboard/{config,*.conf,vault.enc,marketplace.*}
└── /dev/shm/{gm-dashboard.html,gm-window-pos,gm-cfg/}
```

## Testing

```bash
make test       # Build + run all 207 tests (9 QTest suites)
make coverage   # Full coverage report (target: >= 99%, current: 100%)
```

## System Requirements

- Linux with kernel >= 5.13 (Landlock ABI v1; v4+ for network restrictions)
- Qt 6.5+ (6.10 recommended) with WebEngine module
- libsodium >= 1.0.18, libseccomp >= 2.5, libcurl >= 7.80

## GNOME Autostart

A `.desktop` file is included but **not** installed to autostart by default:
```bash
cp gm-dashboard.desktop ~/.config/autostart/
```

## License

[MIT](LICENSE)

# Contributing to GM Dashboard

## Development Setup

### Prerequisites

```bash
# Ubuntu/Debian build dependencies
sudo apt install g++ libsodium-dev libseccomp-dev libcurl4-openssl-dev

# Qt6 — choose one:
# A) System packages (Ubuntu 24.04+, Fedora 40+, Arch)
sudo apt install qt6-base-dev qt6-webengine-dev qt6-tools-dev \
                 qt6-webchannel-dev qt6-positioning-dev

# B) Qt online installer (https://www.qt.io/download-qt-installer)
#    Installs to ~/Qt/<version>/gcc_64 — Makefile default
```

### Building

```bash
git clone https://github.com/user/gm-dashboard.git
cd gm-dashboard

# If using system Qt6:
export QTDIR=/usr/lib/x86_64-linux-gnu/qt6  # adjust for your distro

# First time only
make build
./gm-sign keygen

# Build + sign + install + run
make run
```

### Overriding Build Variables

```bash
make QTDIR=/usr      # System Qt6 location
make PREFIX=/usr     # Install prefix
make ARCH=x86-64-v3  # CPU architecture (default: native)
make CXX=clang++     # Compiler
```

## Test-Driven Development (TDD)

**Tests are mandatory.** No code change is accepted without a corresponding test.

### Workflow

1. **Write a failing test** — it MUST fail (red)
2. **Write minimum code** — just enough to pass (green)
3. **Refactor** — clean up, keeping tests green
4. **Verify coverage** — `make coverage` must stay >= 99%

### Running Tests

```bash
make test       # Quick: build + run all 207 tests
make coverage   # Full: clean build with gcov, HTML report in coverage-report/
```

### Test Rules

- **No mocks, no fakes.** Tests use real plugins, real crypto, real syscalls.
- **No network abuse.** Cache external API responses in `tests/fixtures/`. One live test per service max.
- **GUI-safe.** Tests that touch the viewer must `unset DISPLAY`.
- **Coverage floor: 99%.** Use `LCOV_EXCL` only for genuinely untestable paths (tty I/O, fork+Landlock children, OOM) — always with a justification comment.

### Test Suites

| Suite | What it covers |
|-------|----------------|
| test_sign | Ed25519 keygen, sign, verify, CLI |
| test_weather | ABI, WMO codes, wind dir, config, cached fixtures, live fetch |
| test_system | Plugin ABI, /proc parsing, RAM, thermal |
| test_github | Plugin ABI, gh CLI fetch, JSON fields |
| test_dashboard | CLI flags, JSON output, HTML generation |
| test_location | Nominatim geocoding, city preservation |
| test_config | gm-config CLI (init, set, get, del, lock, unlock, migrate) |
| test_sandbox | Landlock, seccomp, sig verify, loadPlugins, sandboxedFetch |
| test_vault | Vault internals, key derivation, corruption, all cmd* paths |

## Creating a Plugin

See the "Creating a Plugin" section in [README.md](README.md). Key points:

1. Write tests first (`tests/test_<id>.cpp`)
2. Use C++/Qt6 with `extern "C"` ABI (see `gm-plugin.h`)
3. Declare capabilities honestly — undeclared caps = SIGKILL
4. Add build rules to Makefile
5. `make test` + `make coverage` before submitting

## Code Style

- C++17 with Qt6 (QJsonDocument, QFile, QString — no json-c, no FILE*, no char[])
- `gm-plugin.h` stays C-compatible (extern "C" guards) for the plugin ABI
- Compiler warnings are errors in CI: `-Wall -Wextra`
- No unnecessary abstractions — simple and direct

## Security Considerations

- Never commit secrets, API keys, or `.env` files
- Plugin signatures must be regenerated after any `.cpp` change (`make sign`)
- The signing keypair (`marketplace.sec`) must NEVER be committed
- Any new syscall usage must be evaluated against the seccomp filter
- New filesystem access must be evaluated against Landlock rules

# Contributing to Thunder

Thanks for your interest in contributing! This guide covers everything you need to get started.

---

## Development Environment

**Requirements:**

| Tool | Version | Notes |
|:---|:---|:---|
| GCC | 12+ | or Clang 15+ |
| CMake | 3.20+ | |
| Docker | 24+ | + Compose v2 |
| Python | 3.10+ | for pytest |
| Linux | 5.1+ (x86_64) | io_uring requires 5.1+ |

**One-time setup:**

```bash
git clone <repo-url>
cd thunder
git submodule update --init --recursive
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --target thirdparty_deploy -j1   # ~20 min first time
```

> ⚠️ Always use `-j1` for third-party builds — parallel compilation stalls on disk I/O.

---

## Build & Test Cycle

### Build

```bash
cmake --build build -j1 && cmake --install build
```

Optional build flags:

```bash
-DTHUNDER_IO_ASIO_URING=ON    # Enable asio io_uring backend
-DTHUNDER_LUAJIT=ON           # Enable LuaJIT scripting
```

### Test

**Before pushing any change, run all three layers:**

```bash
./deploy.sh test unit       # C++ gtest + Python pytest, ~45s, zero deps
./deploy.sh test e2e        # Docker Compose integration, ~3 min
./deploy.sh test            # unit + e2e
```

> ❌ **Any red test = stop and fix.** Never push with failing tests.
> `u`nit test passes ≠ `e`2e passes. They cover different things. Run both.

### Building plugins

```bash
./deploy.sh build-so all                     # Build all SO plugins
./deploy.sh build-so HelloHttp_ModuleHello   # Build a single plugin
```

---

## Commit Conventions

| Rule | Why |
|:---|:---|
| **Merge only, no rebase** | Rebase rewrites history and drops commits |
| **No force push** | Never `--force` to shared branches |
| **Resolve conflicts manually** | No `--skip` / `--abort` |
| **Verify after conflict resolution** | Full build + smoke test |

Commit message format:

```
type(scope): short description

Optional body with details.
```

Types: `feat`, `fix`, `chore`, `docs`, `refactor`, `test`, `perf`

Examples:
```
feat(net): add asio_uring backend for TLS batching
fix(worker): prevent double-free in GracefulRestart drain
docs(architecture): add io_uring design document
```

---

## Code Guidelines

### Simplicity First

- 50 lines > 200 lines. Always prefer the shorter solution.
- Don't add "flexibility" or "configurability" nobody asked for.
- Don't handle error scenarios that can't happen.
- Don't pre-build for future requirements that don't exist yet.

### Surgical Changes

- Only change what the task requires. Don't "clean up" adjacent code.
- Match the existing code style, even if you prefer something different.
- If you see an unrelated issue, mention it — don't fix it in the same PR.

### C++ Style

- C++20 standard
- Follow existing naming conventions in the codebase:
  - Classes: `PascalCase` (e.g., `StepCo20`, `HttpMsg`)
  - Functions: `PascalCase` (e.g., `SendToClient`, `AnyMessage`)
  - Members: `snake_case_` with trailing underscore
  - Files: `PascalCase` for headers, matching `.cpp`
- Prefer `//` comments over `/* */`
- Keep headers self-contained (include what you use)

### Third-Party Libraries

> ❌ **Never upgrade submodules without explicit approval.**

Third-party libraries in `code/3party/` have locked versions. Upgrading one requires:
1. A separate feature branch
2. Full regression test (C++ gtest + Python pytest + E2E)
3. A standalone PR

---

## Pull Request Process

1. **Create a feature branch** from `dev`:
   ```bash
   git checkout dev && git pull
   git checkout -b feat/my-feature
   ```

2. **Make your changes**, following the guidelines above.

3. **Run the full test suite:**
   ```bash
   ./deploy.sh test            # unit + e2e
   ./tests/test_smoke.sh       # smoke test (requires Docker cluster running)
   ```

4. **Update documentation** if your change affects:
   - API / plugin interface → update relevant `docs/architecture/` doc
   - Performance characteristics → add/update `docs/performance/` benchmark
   - Build / deploy flow → update `QUICKSTART.md`

5. **Commit and push:**
   ```bash
   git add .
   git commit -m "feat(scope): description"
   git push origin feat/my-feature
   ```

6. **Open a PR** targeting `dev`. In the PR description:
   - What problem does this solve?
   - How did you test it?
   - Screenshots / logs if applicable.

---

## Testing Standards

| Layer | Command | Requires | Coverage |
|:---|:---|:---|:---|
| C++ gtest | `ctest --test-dir build/code/test` | Nothing | ~330 tests |
| Python unit | `python3 -m pytest tests/unit/` | Nothing | ~130 tests |
| E2E | `./deploy.sh test e2e` | Docker | 25+ integration tests |
| Smoke | `./tests/test_smoke.sh` | Running cluster | HTTP/HTTPS/WS/WSS/etcd |

**Acceptance criteria for any PR:**
- Build: 0 errors
- C++ gtest: 100% pass
- Python unit: 100% pass
- E2E: 100% pass (allowed: 1 pre-existing etcd route skip)
- Smoke: 0 failures

---

## Where to Find Things

| What | Where |
|:---|:---|
| Architecture overview | [`docs/architecture/00-overview.md`](docs/architecture/00-overview.md) |
| Design FAQ | [`docs/FAQ.md`](docs/FAQ.md) |
| Build & deploy quickstart | [`QUICKSTART.md`](QUICKSTART.md) |
| K8s operations | [`k8s/k8s-manual.md`](k8s/k8s-manual.md) |
| Performance benchmarks | [`docs/performance/`](docs/performance/) |
| All issues | [`issus-list.md`](issus-list.md) |

---

## Getting Help

- **Design questions** → Start with [`docs/FAQ.md`](docs/FAQ.md)
- **Build issues** → Check [`QUICKSTART.md`](QUICKSTART.md) troubleshooting
- **Bug reports** → Open a GitHub Issue with reproduction steps

---

> **Golden rule**: if a newcomer can't understand your change from the PR description + commit message, you haven't documented it enough.

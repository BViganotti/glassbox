# Portability

Glassbox targets POSIX systems. The code is verified on macOS (primary) and
Linux (Ubuntu 24.04, arm64 container).

## Platform matrix

| Feature | macOS | Linux |
|---|---|---|
| Build (CMake + Ninja, C++20) | Apple Clang 21, verified | GCC 13, verified |
| Trace record/read/analysis/workbench | yes | yes |
| Opt-in `new`/`delete` hooks | yes | yes (plain builds; sanitizers excluded) |
| Manual stack sampling | yes | yes |
| Mach thread-state sampler | yes | **no** (returns `Unsupported`; use `ManualSampler`) |
| Symbolization/demangling | `dladdr` + `__cxa_demangle` | same |
| Browser launch | `open` | `xdg-open` |
| Quit / HTTP / SSE | yes | yes |
| App bundle + ad-hoc signing | `make bundle` | not applicable |
| Sanitizer builds | ASan/UBSan/TSan verified | not part of the verified run |

## Linux verification

`tools/linux_test.sh` builds a Docker image (`tools/Dockerfile.linux`, Ubuntu
24.04 + cmake/ninja/g++) and runs the full release test suite inside it. The
source directory is mounted; artifacts stay in the container.

```sh
tools/linux_test.sh
```

Verified result: clean build, `ctest` 7/7 passed (unit, hooks, and all five fuzz
modes). This is a real compile/run check, not a cross-compile claim.

## Platform seam

`src/platform/platform.hpp` isolates the small OS-specific pieces:

- thread naming (`pthread_setname_np` signatures differ);
- random token bytes (`arc4random_buf` vs `getrandom` with `/dev/urandom`
  fallback);
- no-SIGPIPE socket behavior (`SO_NOSIGPIPE` vs `MSG_NOSIGNAL`);
- browser command (`open` vs `xdg-open`).

The Mach sampler is compiled only on Apple and has a validating stub elsewhere
so the public API remains source-compatible. On Linux, use `ManualSampler`
(explicit captures) or portable external sampling front-ends.

## Not supported

- Windows is out of scope (the project is built on POSIX APIs).
- Linux packaging is source-only; there is no `.deb`, Flatpak or AppImage.
- The macOS app bundle is ad-hoc signed unless a Developer ID is supplied; see
  [packaging.md](packaging.md).

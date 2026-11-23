# Packaging

## macOS application bundle

`make bundle` creates `dist/Glassbox.app` from the built launcher:

```sh
make build
make bundle
open dist/Glassbox.app
```

The script (`tools/make_bundle.sh`) creates the standard `Contents/MacOS` and
`Contents/Resources` layout, renders `packaging/Info.plist.in` with the binary's
version, and ad-hoc signs the bundle (`codesign --sign -`) when `codesign` is
available. Ad-hoc signatures are enough for local launch on the same machine;
they are **not** a substitute for Developer ID signing/notarization.

To sign with a real identity:

```sh
GBX_CODESIGN_IDENTITY="Developer ID Application: ..." make bundle
```

Notarization and stapling require an Apple Developer account and credentials
that are not part of this repository. The bundle is intentionally unsigned for
distribution beyond local use; download recipients may see Gatekeeper warnings.
No auto-update, pkg installer, or universal binary (arm64 + x86_64) is
produced. Building on an Intel Mac produces an x86_64 binary; there is no
cross-architecture merge tool.

The launcher is a normal executable; when started from Finder it starts the
loopback server and opens the default browser. Quit from the header power
button, or use the Dock/Force Quit.

## Linux

Linux builds from source (`tools/linux_test.sh` verifies the build/test in an
Ubuntu container). There is no packaged artifact. Use the built
`build/glassbox` launcher; it invokes `xdg-open` for the browser.

## Release checklist

`tools/release_check.sh` performs the local release checks:

1. configure + build (Release, warnings-as-errors);
2. unit/hooks/fuzz ctest suite;
3. quick benchmarks;
4. headless UI smoke (if Chrome is installed);
5. app bundle build + version check (macOS);
6. optional Linux container test.

There is no CI pipeline by project rule; this script is the reproducible local
substitute.

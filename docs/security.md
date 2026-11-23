# Security and threat model

Glassbox runs on the developer's machine and treats the local user as trusted.
Its job is to keep recorded application data off the network and away from
web pages that did not start it.

## Assets

- Trace contents: heap addresses, function names, logs, counters and state.
- Trace files on disk (`.gbt`).
- The local UI session.
- The process's ability to read and write files under the trace roots.

## Boundaries

1. **Network**: the server binds `127.0.0.1` only. Binding any other address is
   rejected by `HttpServer::listen`. There is no outbound network access at
   runtime and no remote APIs.
2. **Browser**: the UI is served from the same loopback origin; all assets are
   embedded. The CSP forbids external scripts, styles, fonts, frames and forms.
3. **Filesystem**: the UI can only list/open regular `.gbt` files under the
   configured trace roots. Paths are canonicalized, symlinks are resolved,
   traversal segments are rejected, and the open call is a POST body rather
   than a URL path.
4. **Process**: the allocation hooks and Mach sampler only affect the host
   process the library is linked into. Nothing is injected into other
   processes.

## Authentication

A 128-bit token is generated per process with `arc4random_buf`. Requests must
present it as:

- `X-Glassbox-Token: <token>`,
- `?t=<token>`, or
- the `gb_token` cookie set by the first navigation.

The initial launch URL carries `?t=...`. The index response sets an HttpOnly,
SameSite=Strict cookie and the SPA never needs to read the cookie. The token is
compared in constant time. Static assets are protected too; a request without
a valid token gets `403`, including SSE (`reject(403, ...)` before stream
headers). The token is not logged by the server; the launcher prints the URL to
stderr for convenience.

## Cross-site protections

- `Host` must be `127.0.0.1` or `localhost` (optional port). This blocks DNS
  rebinding, where an attacker domain resolves to loopback.
- If an `Origin` header is present it must be `http://127.0.0.1:<port>` or
  `http://localhost:<port>`. Cross-origin POSTs are rejected.
- The cookie is `SameSite=Strict` and HttpOnly.
- Responses use `Cache-Control: no-store`, `X-Content-Type-Options: nosniff`,
  `Referrer-Policy: no-referrer` and a restrictive CSP.
- State-changing endpoints are POST only (`/api/open`, `/api/demo`,
  `/api/shutdown`).

## Resource limits

The server rejects or closes oversized/malformed requests before allocating:
32 KiB headers, 1 MiB bodies, 100 headers, 32 query parameters, 64 concurrent
connections (503 beyond that), 32 MiB responses. Trace files over 512 MiB are
rejected. The live feed is bounded at 1,024 events x 16 KiB. Analysis endpoints
cap their lists (objects 5,000, spans 20,000, flame nodes 30,000, events 1,000).
Dimensions and headers are validated before being reflected into a response.

## Unauthenticated surfaces

None on the HTTP server. The launcher's stderr prints the tokenized URL (the
developer already has local access). The trace files themselves are ordinary
files with default permissions, written with mode 0644; use directory
permissions if traces are sensitive.

## Risks that remain

- Any local process running as the same user can read the trace files, inspect
  the browser's history/cookies, or connect to loopback if it knows the token.
  Glassbox does not defend against a compromised local user session.
- There is no TLS because traffic never leaves loopback. A local packet capture
  can still see it.
- Trace content is untrusted input to the UI. JSON strings are escaped and DOM
  text is inserted with `textContent`, never `innerHTML`, but a malicious trace
  can still display misleading text or consume bounded resources.
- The allocation hooks are process-global and only as safe as the host's
  allocator usage; see [recorder.md](recorder.md).
- Mach sampling reads the process's own memory. Invalid frames are guarded with
  `mach_vm_read_overwrite` and validation, but sampling remains observational,
  not a security boundary.

## Reporting

This is a local developer tool with no network service. If you find a crash or
memory-safety issue, include the trace or a minimized reproduction; the fuzz
harnesses under `fuzz/` are the intended way to produce one.

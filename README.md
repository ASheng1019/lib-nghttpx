# nghttpx 1.70.0 Unikraft port

This port uses the nghttp2 release archive, alongside the independent nghttp2
C-library port. Keeping a second extracted tree gives the service its own
private patches/configuration and avoids changing the C library's sources.
Unikraft `fetch` and `patch` hooks prepare it; all target sources are compiled
through ordinary `LIBNGHTTPX_SRCS-y` rules. `Library.uk` identifies the upstream
source, not a published port repository.

`Config.uk` declares dependencies. The source list includes upstream nghttpx,
its HTTP/QUIC/TLS helpers and bundled URL/HTTP parsers. Private `config.h` is
not exported. The service port owns profile validation, event-loop lifecycle
hooks and read-only worker counters in `native_runtime.cc`. Clock providers,
network configuration and management endpoints belong to the calling app.
Selecting LIBNGHTTPX adds no clock wrappers or management socket.

The patch series separates generic fixes from platform adaptation:

- `0001-preserve-retry-result.patch`: generic Retry classification (nghttp2#2933).
- `0002-drop-truncated-datagrams.patch`: reject MSG_TRUNC/MSG_CTRUNC before
  QUIC parsing; the independent upstream branch also carries its native tests.
- `0003-service-lifecycle.patch`: entrypoint, one-worker HTTP/3 profile,
  process-free execution, optional lifecycle callbacks and resource cleanup.
  The application may request upstream graceful shutdown; its drain deadline
  and UDP control protocol are maintained outside this patch.
- `0004-quic-datagram-io.patch`: individual datagram TX/backpressure, packet
  metadata, IPv4 DF setup on the UDP listener, ECN/PMTUD controls and bounded
  stack use. GSO/GRO remain disabled in the qualified profile.

Normal HTTP/3 stream handling, backend flow control and retaining response
storage until QUIC ACK are upstream implementations. New service glue
is maintained directly in this port; it is not another upstream source patch.
The receive array is shared only because the supported event loop is single
worker. The LLVM parser workaround disables NEON intrinsics for C translation
units because the freestanding include path has no arm_neon.h; it does not
disable ARM64 CPU SIMD or OpenSSL assembly. Non-native buffering is unchanged.

The public C Interface is `<nghttpx/unikraft.h>`:
`uk_nghttpx_run(options)` runs once and blocks until shutdown. NULL options
selects conservative datagram settings and no management hooks. The caller
initializes time/network first. The paired optional on_ready/on_close callbacks
run on the event-loop thread. Their opaque control handle permits idempotent
`uk_nghttpx_shutdown()` and `uk_nghttpx_get_stats()` without exporting nghttpx's
private C++ objects. It must not outlive on_close or cross threads.

There is no implicit /native-boot.txt or /native-runtime.conf contract in this
library. /nghttpx.conf remains the upstream service config. Realtime and C++
system_clock must be available and consistent; invalid time fails startup.
The app's optional trusted epoch adapter is separate from the core clock.
`entry.cc` owns invocation and crypto cleanup. Private config/runtime headers
remain local includes; the port root is not exported globally.

MIT applies to upstream nghttp2/nghttpx (`COPYING.nghttp2`); new glue is marked
BSD-3-Clause. Review the workspace README for qualified dependencies and limits.

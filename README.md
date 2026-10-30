# nghttpx 1.70.0 Unikraft port

This port uses the nghttp2 release archive, alongside the independent nghttp2
C-library port. Keeping a second extracted tree gives the service its own
private patches/configuration and avoids changing the C library's sources.
Unikraft `fetch` and `patch` hooks prepare it; all target sources are compiled
through ordinary `LIBNGHTTPX_SRCS-y` rules. `Library.uk` identifies the upstream
source, not a published port repository.

`Config.uk` declares dependencies. The source list includes upstream nghttpx,
its HTTP/QUIC/TLS helpers and bundled URL/HTTP parsers. Private `config.h` is
not exported. The service port owns `native_runtime.*` and `native_network.*`;
only their startup interface is used by the thin app. The clock wrappers are
part of the trusted boot epoch contract and apply at final link.

The patch series separates generic fixes from platform adaptation:

- `0001-preserve-retry-result.patch`: generic Retry classification (nghttp2#2933).
- `0002-drop-truncated-datagrams.patch`: reject MSG_TRUNC/MSG_CTRUNC before
  QUIC parsing; the independent upstream branch also carries its native tests.
- `0003-service-lifecycle.patch`: entrypoint, one-worker HTTP/3 profile,
  process-free execution, bounded shutdown and resource cleanup.
- `0004-quic-datagram-io.patch`: individual datagram TX/backpressure, packet
  metadata, IPv4 DF setup on the UDP listener, ECN/PMTUD controls and bounded
  stack use. GSO/GRO remain disabled in the qualified profile.

Normal HTTP/3 stream handling, backend flow control and retaining response
storage until QUIC ACK are upstream implementations. New runtime/network glue
is maintained directly in this port; it is not another upstream source patch.
The receive array is shared only because the supported event loop is single
worker. The LLVM parser workaround disables NEON intrinsics for C translation
units because the freestanding include path has no arm_neon.h; it does not
disable ARM64 CPU SIMD or OpenSSL assembly. Non-native buffering is unchanged.

The public C API is `<nghttpx/unikraft.h>` / `uk_nghttpx_run()`. It owns startup,
event-loop execution and cleanup, returning the nghttpx exit status; it is
called once per boot. The thin app does not include libev, TLS or private
runtime headers. `entry.cc` implements this interface. `private/config.h`,
`private/native_runtime.h` and `private/native_network.h` are local includes;
the port root is not exported globally. Clock wrappers at final link still
affect the whole unikernel, and the trusted boot-epoch contract is required.

MIT applies to upstream nghttp2/nghttpx (`COPYING.nghttp2`); new glue is marked
BSD-3-Clause. Review the workspace README for qualified dependencies, initrd
contract and limitations.

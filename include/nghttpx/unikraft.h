/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_NGHTTPX_UNIKRAFT_H
#define UK_NGHTTPX_UNIKRAFT_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
struct ev_loop;
struct uk_nghttpx_control;
struct uk_nghttpx_stats {
	uint64_t connections;
	uint64_t close_waits;
};
struct uk_nghttpx_options {
	unsigned max_udp_payload; /* 0 means 1200; otherwise 1200..1452. */
	int pmtud;
	int ecn;
	/* Optional paired hooks, called on the event-loop thread. on_ready
	 * returns 0 on success; on failure it must undo its own partial
	 * initialization. The control handle is valid until on_close returns.
	 * Neither hook may retain it or use it from another thread. user is
	 * owned by the caller.
	 */
	int (*on_ready)(struct ev_loop *, struct uk_nghttpx_control *,
			void *user);
	void (*on_close)(struct ev_loop *, void *user);
	void *user;
};
/* Called only from the event-loop thread between ready/close hooks. Shutdown
 * is idempotent and requests upstream graceful drain. No socket is created.
 */
void uk_nghttpx_shutdown(struct uk_nghttpx_control *);
int uk_nghttpx_get_stats(const struct uk_nghttpx_control *,
			 struct uk_nghttpx_stats *);
/* Run once from the application entry thread; reads /nghttpx.conf and requires
 * usable realtime. NULL options selects conservative I/O and no hooks. Caller
 * configures time/network before entry. Owns event-loop execution and cleanup;
 * returns exit status, or 1 on invalid boot config. Fatal errors halt the VM.
 */
int uk_nghttpx_run(const struct uk_nghttpx_options *);
#ifdef __cplusplus
}
#endif
#endif

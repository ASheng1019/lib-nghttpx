/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef UK_NGHTTPX_UNIKRAFT_H
#define UK_NGHTTPX_UNIKRAFT_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Run once from the application entry thread. Reads /nghttpx.conf, the
 * trusted /native-boot.txt contract and optional /native-runtime.conf.
 * Owns initialization, event-loop execution and cleanup. Returns the
 * nghttpx exit status; fatal faults can terminate the unikernel directly.
 *
 * @return nghttpx exit status, or 1 when boot configuration is invalid.
 */
int uk_nghttpx_run(void);

#ifdef __cplusplus
}
#endif
#endif /* UK_NGHTTPX_UNIKRAFT_H */

// SPDX-License-Identifier: BSD-3-Clause
#pragma once
[[noreturn]] void native_fatal_exit(int status);
#include <ev.h>
struct uk_nghttpx_options;
bool native_boot_init(const uk_nghttpx_options *);
bool native_service_ready(struct ev_loop *, void (*)(void *), void *);
void native_service_stopped(struct ev_loop *);

bool native_ecn_enabled();
bool native_pmtud_enabled();
unsigned native_max_payload();

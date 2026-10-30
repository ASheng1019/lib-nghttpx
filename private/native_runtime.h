// SPDX-License-Identifier: BSD-3-Clause
#pragma once
[[noreturn]] void native_fatal_exit(int status);
#include <ev.h>
bool native_boot_init();
bool native_watch_stop(struct ev_loop *, void (*)(void *), void *);
void native_unwatch_stop(struct ev_loop *);

bool native_ecn_enabled();
bool native_pmtud_enabled();
unsigned native_max_payload();

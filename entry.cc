// SPDX-License-Identifier: BSD-3-Clause
#include <nghttpx/unikraft.h>
#include <cstdio>
#include "native_runtime.h"
#include <ngtcp2/ngtcp2_crypto_ossl.h>
extern int nghttpx_main(int, char **);
extern "C" unsigned native_openssl_armcap_detected(void) __attribute__((weak));
extern "C" unsigned native_openssl_armcap_active(void) __attribute__((weak));
int uk_nghttpx_run(const uk_nghttpx_options *options)
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	setvbuf(stderr, nullptr, _IONBF, 0);
	if (!native_boot_init(options)) {
		std::puts(
		    "NATIVE_NGHTTPX FAIL profile or realtime configuration");
		return 1;
	}
	if (native_openssl_armcap_detected && native_openssl_armcap_active)
		printf("NATIVE_NGHTTPX CRYPTO detected=%u active=%u\n",
		       native_openssl_armcap_detected(),
		       native_openssl_armcap_active());
	std::puts("NATIVE_NGHTTPX START");
	char name[] = "nghttpx", conf[] = "--conf=/nghttpx.conf";
	char *args[] = {name, conf, nullptr};
	int result = nghttpx_main(2, args);
	ngtcp2_crypto_ossl_free();
	printf("NATIVE_NGHTTPX EXIT result=%d\n", result);
	return result;
}

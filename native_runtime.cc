// SPDX-License-Identifier: BSD-3-Clause
// Trusted boot epoch is an application contract, not a replacement for a
// platform RTC.
#include "native_runtime.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <sys/time.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdint>
#include <string>
#include <map>
#include <cstdlib>
#include <limits>
#include "native_network.h"
#include <fcntl.h>
#include <uk/alloc.h>
#include <uk/pm.h>
#include "shrpx_connection_handler.h"
#include "shrpx_worker.h"
namespace
{
int64_t epoch_offset;
bool anchored;
char stop_token[65];
ev_io stop_watcher;
int stop_fd = -1;
sockaddr_in control_addr{};
unsigned drain_seconds = 15;
ev_timer drain_timer;
bool draining = false;
bool ecn_enabled = false, pmtud_enabled = false;
unsigned max_payload = 1200;
std::string ipv6_address;

void (*stop_callback)(void *);
void *stop_arg;
void drain_expired(struct ev_loop *, ev_timer *, int)
{
	std::puts("NATIVE_NGHTTPX DRAIN_DEADLINE immediate_vm_shutdown");
	native_fatal_exit(2);
}
void receive_stop(struct ev_loop *loop, ev_io *, int)
{
	char message[128];
	sockaddr_in peer{};
	socklen_t peer_len = sizeof(peer);
	auto count = recvfrom(stop_fd, message, sizeof(message), 0,
			      reinterpret_cast<sockaddr *>(&peer), &peer_len);
	if (count == 70 && !memcmp(message, "STATS ", 6) &&
	    !memcmp(message + 6, stop_token, 64)) {
		auto handler =
		    static_cast<shrpx::ConnectionHandler *>(stop_arg);
		auto worker = handler->get_single_worker();
		auto stat = worker->get_worker_stat();
		long long used = -1, peak = -1, enomem = -1;
#if CONFIG_LIBUKALLOC_IFSTATS_GLOBAL
		uk_alloc_stats allocator{};
		uk_alloc_stats_get_global(&allocator);
		used = allocator.cur_mem_use;
		peak = allocator.max_mem_use;
		enomem = allocator.nb_enomem;
#endif
		int descriptors = 0;
		for (int fd = 0; fd < 4096; ++fd)
			if (fcntl(fd, F_GETFD) >= 0)
				++descriptors;
		char response[512];
		int length =
		    snprintf(response, sizeof(response),
			     "{\"heap_used\":%lld,\"heap_peak\":%lld,"
			     "\"available\":%llu,\"enomem\":%lld,"
			     "\"connections\":%zu,\"close_waits\":%zu,\"fds\":%"
			     "d,\"fd_scan_limit\":4096}",
			     used, peak,
			     static_cast<unsigned long long>(
				 uk_alloc_availmem(uk_alloc_get_default())),
			     enomem, stat->num_connections,
			     stat->num_close_waits, descriptors);
		if (length > 0 &&
		    static_cast<size_t>(length) < sizeof(response))
			sendto(stop_fd, response, length, 0,
			       reinterpret_cast<sockaddr *>(&peer), peer_len);
		return;
	}
	if (count == 64 && !memcmp(message, stop_token, 64)) {
		std::puts("NATIVE_NGHTTPX STOP authenticated");
		if (draining)
			return;
		draining = true;
		ev_io_stop(loop, &stop_watcher);
		ev_timer_start(loop, &drain_timer);
		stop_callback(stop_arg);
	}
}
} // namespace
[[noreturn]] void native_fatal_exit(int status)
{
	// This profile has one application process per VM. Upstream _Exit kills
	// a process on Linux; here it must end the VM, including helper
	// threads.
	printf("NATIVE_NGHTTPX FATAL_EXIT status=%d immediate_vm_shutdown\n",
	       status);
	fflush(nullptr);
	uk_pm_shutdown(UK_PM_SHUTDOWN_OP_SYSHALT);
}
extern "C" int __real_clock_gettime(clockid_t, timespec *);
extern "C" int __wrap_clock_gettime(clockid_t clock, timespec *ts)
{
	if (!anchored ||
	    (clock != CLOCK_REALTIME && clock != CLOCK_REALTIME_COARSE))
		return __real_clock_gettime(clock, ts);
	if (__real_clock_gettime(CLOCK_MONOTONIC, ts))
		return -1;
	int64_t n = static_cast<int64_t>(ts->tv_sec) * 1000000000 +
		    ts->tv_nsec + epoch_offset;
	ts->tv_sec = n / 1000000000;
	ts->tv_nsec = n % 1000000000;
	return 0;
}
extern "C" int __wrap_gettimeofday(timeval *tv, void *)
{
	timespec ts;
	if (__wrap_clock_gettime(CLOCK_REALTIME, &ts))
		return -1;
	if (tv) {
		tv->tv_sec = ts.tv_sec;
		tv->tv_usec = ts.tv_nsec / 1000;
	}
	return 0;
}
extern "C" time_t __wrap_time(time_t *out)
{
	timespec ts;
	if (__wrap_clock_gettime(CLOCK_REALTIME, &ts))
		return -1;
	if (out)
		*out = ts.tv_sec;
	return ts.tv_sec;
}
namespace
{
bool number(const std::string &value, unsigned long long low,
	    unsigned long long high, unsigned long long *out = nullptr)
{
	if (value.empty() ||
	    value.find_first_not_of("0123456789") != std::string::npos)
		return false;
	errno = 0;
	char *end = nullptr;
	auto parsed = strtoull(value.c_str(), &end, 10);
	if (errno || !end || *end || parsed < low || parsed > high)
		return false;
	if (out)
		*out = parsed;
	return true;
}
bool endpoint(const std::string &value, sockaddr_in &address,
	      bool wildcard = false)
{
	auto comma = value.find(',');
	unsigned long long port;
	address = {};
	address.sin_family = AF_INET;
	if (comma == std::string::npos ||
	    inet_pton(AF_INET, value.substr(0, comma).c_str(),
		      &address.sin_addr) != 1 ||
	    (!wildcard && address.sin_addr.s_addr == INADDR_ANY) ||
	    !number(value.substr(comma + 1), 1, 65535, &port))
		return false;
	address.sin_port = htons(static_cast<uint16_t>(port));
	return true;
}
bool read_options(const char *path,
		  std::multimap<std::string, std::string> &options,
		  bool optional = false)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return optional && errno == ENOENT;
	char *line = nullptr;
	size_t capacity = 0;
	bool valid = true;
	while (getline(&line, &capacity, f) >= 0) {
		std::string value = line;
		auto begin = value.find_first_not_of(" \t\r\n");
		if (begin == std::string::npos || value[begin] == '#')
			continue;
		value = value.substr(begin, value.find_last_not_of(" \t\r\n") -
						begin + 1);
		auto equals = value.find('=');
		if (value.size() > 4096 || equals == std::string::npos ||
		    (options.contains(value.substr(0, equals)) &&
		     value.substr(0, equals) != "frontend" &&
		     value.substr(0, equals) != "backend" &&
		     value.substr(0, equals) != "tls-ticket-key-file")) {
			valid = false;
			break;
		}
		options.emplace(value.substr(0, equals),
				value.substr(equals + 1));
	}
	if (ferror(f))
		valid = false;
	free(line);
	fclose(f);
	return valid;
}
bool check_profile()
{
	std::multimap<std::string, std::string> options;
	if (!read_options("/nghttpx.conf", options)) {
		std::puts("NATIVE_NGHTTPX CONFIG parse error");
		return false;
	}
	const std::map<std::string, std::string> required = {
	    {"workers", "1"},
	    {"single-process", "yes"},
	    {"single-thread", "yes"},
	    {"no-quic-bpf", "yes"}};
	for (const auto &[key, value] : required) {
		if (!options.contains(key) ||
		    options.find(key)->second != value) {
			printf("NATIVE_NGHTTPX CONFIG rejected key=%s\n",
			       key.c_str());
			return false;
		}
	}
	sockaddr_in frontend{};
	for (const auto &[key, value] : options) {
		if (required.contains(key))
			continue;
		bool valid = true;
		if (key == "frontend") {
			const std::string suffix = ";quic";
			valid = value.ends_with(suffix);
			auto address =
			    value.substr(0, value.size() - suffix.size());
			auto comma = address.find(',');
			unsigned long long port = 0;
			auto host = address.substr(0, comma);
			in6_addr ip6{};
			if (host.starts_with("[") && host.ends_with("]"))
				host = host.substr(1, host.size() - 2);
			sockaddr_in candidate{};
			bool v4 = endpoint(address, candidate, true);
			if (v4)
				frontend = candidate;
			valid =
			    valid && (v4 || (comma != std::string::npos &&
					     inet_pton(AF_INET6, host.c_str(),
						       &ip6) == 1 &&
					     number(address.substr(comma + 1),
						    1, 65535, &port)));

		} else if (key == "backend") {
			// Backend address/pattern/parameters are parsed by
			// upstream nghttpx. Restrict host syntax and expose
			// only the verified parameter subset.
			auto semi = value.find(';');
			auto address = value.substr(0, semi);
			auto comma = address.rfind(',');
			unsigned long long port;
			valid =
			    comma != std::string::npos && comma > 0 &&
			    number(address.substr(comma + 1), 1, 65535, &port);
			if (valid)
				valid =
				    address.substr(0, comma).find_first_not_of(
					"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKL"
					"MNOPQRSTUVWXYZ0123456789.-:[]_") ==
				    std::string::npos;
			while (valid && semi != std::string::npos) {
				auto next = value.find(';', semi + 1);
				auto param =
				    value.substr(semi + 1, next - semi - 1);
				valid =
				    param.empty() || param == "/" ||
				    param == "dns" || param == "tls" ||
				    param == "proto=h2" ||
				    param == "proto=http/1.1" ||
				    (param.starts_with("sni=") &&
				     param.size() > 4 &&
				     param.substr(4).find_first_not_of(
					 "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK"
					 "LMNOPQRSTUVWXYZ0123456789.-") ==
					 std::string::npos);
				semi = next;
			}
		} else if (key == "private-key-file" ||
			   key == "certificate-file" || key == "cacert" ||
			   key == "tls-ticket-key-file") {
			valid = !value.empty() && value[0] == '/';
		} else if (key == "errorlog-file") {
			valid = value == "/dev/stderr";
		} else if (key == "accesslog-file") {
			valid = value == "/dev/stdout";
		} else if (key == "log-level") {
			valid = value == "NOTICE" || value == "INFO";
		} else if (key == "worker-frontend-connections") {
			valid = number(value, 1, 65535);
		} else if (key == "frontend-quic-idle-timeout" ||
			   key == "backend-read-timeout" ||
			   key == "backend-connect-timeout" ||
			   key == "backend-keep-alive-timeout" ||
			   key == "dns-cache-timeout" ||
			   key == "dns-lookup-timeout") {
			// Upstream validates duration syntax; process errors
			// terminate this VM.
		} else if (key == "dns-max-try") {
			valid = number(value, 1, 10);
		} else {
			valid = false;
		}
		if (!valid) {
			printf("NATIVE_NGHTTPX CONFIG rejected key=%s\n",
			       key.c_str());
			return false;
		}
	}
	for (auto key :
	     {"frontend", "backend", "private-key-file", "certificate-file"}) {
		if (!options.contains(key)) {
			printf("NATIVE_NGHTTPX CONFIG missing key=%s\n", key);
			return false;
		}
	}
	control_addr = frontend;
	control_addr.sin_port = htons(ntohs(frontend.sin_port) == 65535
					  ? 4434
					  : ntohs(frontend.sin_port) + 1);
	std::multimap<std::string, std::string> runtime;
	if (!read_options("/native-runtime.conf", runtime, true))
		return false;
	for (const auto &[key, value] : runtime) {
		if (key == "control") {
			if (!endpoint(value, control_addr))
				return false;
		} else if (key == "ipv6-address") {
			in6_addr address;
			if (inet_pton(AF_INET6, value.c_str(), &address) != 1)
				return false;
			ipv6_address = value;
		} else if (key == "ecn" || key == "pmtud") {
			if (value != "yes" && value != "no")
				return false;
			(key == "ecn" ? ecn_enabled : pmtud_enabled) =
			    value == "yes";
		} else if (key == "max-udp-payload") {
			unsigned long long parsed;
			if (!number(value, 1200, 1452, &parsed))
				return false;
			max_payload = parsed;
		} else if (key == "drain-seconds") {
			unsigned long long parsed;
			if (!number(value, 1, 300, &parsed))
				return false;
			drain_seconds = parsed;
		} else {
			printf("NATIVE_NGHTTPX RUNTIME rejected key=%s\n",
			       key.c_str());
			return false;
		}
	}
	if (control_addr.sin_addr.s_addr == INADDR_ANY)
		return false;
	if (control_addr.sin_addr.s_addr == frontend.sin_addr.s_addr &&
	    control_addr.sin_port == frontend.sin_port)
		return false;
	std::puts("NATIVE_NGHTTPX CONFIG profile=single-worker-h3 PASS");
	return true;
}
} // namespace
bool native_boot_init()
{
	if (!check_profile())
		return false;
	if (native_network_init(ipv6_address.c_str()))
		return false;
	FILE *f = fopen("/native-boot.txt", "r");
	unsigned long long epoch = 0;
	char epoch_text[32], extra;
	if (!f)
		return false;
	int count = fscanf(f, "%31s %64s %c", epoch_text, stop_token, &extra);
	fclose(f);
	if (count != 2 ||
	    !number(epoch_text, 1704067200, 4102444800ULL, &epoch) ||
	    strlen(stop_token) != 64 ||
	    strspn(stop_token, "0123456789abcdefABCDEF") != 64)
		return false;
	timespec mono;
	if (__real_clock_gettime(CLOCK_MONOTONIC, &mono))
		return false;
	epoch_offset = epoch * 1000000000LL -
		       static_cast<int64_t>(mono.tv_sec) * 1000000000 -
		       mono.tv_nsec;
	anchored = true;
	auto system_epoch =
	    std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch())
		.count();
	if (system_epoch < epoch || system_epoch > epoch + 1)
		return false;
	printf(
	    "NATIVE_NGHTTPX TIME trusted_boot_epoch=%llu system_epoch=%lld\n",
	    epoch, static_cast<long long>(system_epoch));
	return true;
}
bool native_watch_stop(struct ev_loop *loop, void (*callback)(void *),
		       void *arg)
{
	stop_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
	if (stop_fd < 0)
		return false;
	if (bind(stop_fd, reinterpret_cast<sockaddr *>(&control_addr),
		 sizeof(control_addr))) {
		close(stop_fd);
		stop_fd = -1;
		return false;
	}
	stop_callback = callback;
	stop_arg = arg;
	ev_io_init(&stop_watcher, receive_stop, stop_fd, EV_READ);
	ev_timer_init(&drain_timer, drain_expired, drain_seconds, 0);
	ev_io_start(loop, &stop_watcher);
	printf("NATIVE_NGHTTPX READY control=%u drain_seconds=%u\n",
	       ntohs(control_addr.sin_port), drain_seconds);
	return true;
}
void native_unwatch_stop(struct ev_loop *loop)
{
	ev_timer_stop(loop, &drain_timer);
	if (stop_fd >= 0) {
		ev_io_stop(loop, &stop_watcher);
		close(stop_fd);
		stop_fd = -1;
	}
}

bool native_ecn_enabled()
{
	return ecn_enabled;
}
bool native_pmtud_enabled()
{
	return pmtud_enabled;
}
unsigned native_max_payload()
{
	return max_payload;
}

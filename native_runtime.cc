// SPDX-License-Identifier: BSD-3-Clause
#include "native_runtime.h"
#include "shrpx_connection_handler.h"
#include "shrpx_worker.h"
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <nghttpx/unikraft.h>
#include <string>
#include <unistd.h>
// Include core C headers after C++ templates (__true/__false are core macros).
#include <uk/pm.h>
struct uk_nghttpx_control {
	void (*shutdown)(void *);
	void *arg;
	bool stopping;
};
namespace
{
uk_nghttpx_options settings{};
uk_nghttpx_control control{};
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
	std::puts("NATIVE_NGHTTPX CONFIG profile=single-worker-h3 PASS");
	return true;
}
} // namespace
bool native_boot_init(const uk_nghttpx_options *options)
{
	settings = options ? *options : uk_nghttpx_options{};
	if (!settings.max_udp_payload)
		settings.max_udp_payload = 1200;
	if (settings.max_udp_payload < 1200 ||
	    settings.max_udp_payload > 1452 ||
	    (!!settings.on_ready != !!settings.on_close) || !check_profile())
		return false;
	timespec now;
	if (clock_gettime(CLOCK_REALTIME, &now) || now.tv_sec < 1704067200 ||
	    now.tv_sec > 4102444800LL) {
		std::puts("NATIVE_NGHTTPX TIME unavailable: supply a "
			  "platform/deployment clock");
		return false;
	}
	auto system_epoch =
	    std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch())
		.count();
	if (system_epoch < now.tv_sec - 1 || system_epoch > now.tv_sec + 1) {
		std::puts("NATIVE_NGHTTPX TIME inconsistent C/C++ realtime");
		return false;
	}
	return true;
}
bool native_service_ready(struct ev_loop *loop, void (*shutdown)(void *),
			  void *arg)
{
	control = {shutdown, arg, false};
	if (settings.on_ready &&
	    settings.on_ready(loop, &control, settings.user))
		return false;
	std::printf("NATIVE_NGHTTPX READY management=%s\n",
		    settings.on_ready ? "external" : "disabled");
	return true;
}
void native_service_stopped(struct ev_loop *loop)
{
	if (settings.on_close)
		settings.on_close(loop, settings.user);
	control = {};
}
extern "C" void uk_nghttpx_shutdown(uk_nghttpx_control *handle)
{
	auto *c = handle;
	if (c != &control || !c->shutdown || c->stopping)
		return;
	c->stopping = true;
	c->shutdown(c->arg);
}
extern "C" int uk_nghttpx_get_stats(const uk_nghttpx_control *handle,
				    uk_nghttpx_stats *out)
{
	const auto *c = handle;
	if (c != &control || !c->arg || !out)
		return -1;
	auto stat = static_cast<shrpx::ConnectionHandler *>(c->arg)
			->get_single_worker()
			->get_worker_stat();
	out->connections = stat->num_connections;
	out->close_waits = stat->num_close_waits;
	return 0;
}

bool native_ecn_enabled() { return settings.ecn != 0; }
bool native_pmtud_enabled() { return settings.pmtud != 0; }
unsigned native_max_payload() { return settings.max_udp_payload; }

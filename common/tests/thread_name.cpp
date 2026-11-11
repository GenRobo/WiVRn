// utils::thread_name / utils::named_thread (GRID #3095): names are cut to the 15 bytes the
// kernel keeps, and a named_thread carries its name in its own comm before the body runs.
#include "utils/named_thread.h"

#include <iostream>
#include <string>
#include <string_view>

namespace
{
int failures = 0;
#define CHECK(cond)                                                                   \
	do                                                                                \
	{                                                                                 \
		if (!(cond))                                                                  \
		{                                                                             \
			std::cerr << __FILE__ << ":" << __LINE__ << ": FAIL " #cond << std::endl; \
			++failures;                                                               \
		}                                                                             \
	} while (0)
} // namespace

int main()
{
	const struct
	{
		std::string_view name;
		std::string_view comm;
	} cases[] = {
	        {"", ""},
	        {"decoder-0", "decoder-0"},
	        {"network_thread", "network_thread"},
	        {"tracking_thread", "tracking_thread"}, // exactly 15
	        {"application_thread", "application_thr"},
	        {"marker-detector-pool", "marker-detector"},
	};
	for (const auto & c: cases)
		CHECK(utils::thread_name(c.name) == c.comm);
	CHECK(utils::thread_name(std::string(100, 'x')).size() == utils::thread_name_max);

	// The new thread sees its own name (not the parent's) as the first thing it does.
	std::string seen;
	utils::named_thread("application_thread", [&](int arg) {
		char buf[16] = {};
		pthread_getname_np(pthread_self(), buf, sizeof(buf));
		seen = buf;
		seen += ':' + std::to_string(arg);
	}, 42).join();
	CHECK(seen == "application_thr:42");

	if (failures)
		return 1;
	std::cout << "thread_name: all checks passed" << std::endl;
}

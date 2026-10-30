// Temporary scaffolding for GRID #2834 milestone 1: lets the health tests build and run
// against an implementation that does nothing, so they fail for the right reason.
// Removed in milestone 2, when server/driver/health_rules.cpp provides the real one.
#include "driver/health_rules.h"

namespace wivrn::health
{
size_t log_histogram::bucket(double)
{
	return 0;
}
void log_histogram::observe(double) {}
double log_histogram::percentile(double) const
{
	return 0;
}
uint64_t log_histogram::total() const
{
	return 0;
}
void log_histogram::clear() {}

std::string_view name(rule_id)
{
	return "unimplemented";
}
std::optional<rule_id> rule_from_name(std::string_view)
{
	return std::nullopt;
}

thresholds thresholds::defaults()
{
	return {};
}
thresholds thresholds::with(const std::map<std::string, float> &)
{
	return {};
}
float thresholds::get(std::string_view) const
{
	return 0;
}

std::span<const rule> rules()
{
	return {};
}

std::vector<rule_state::event> rule_state::step(std::span<const sample>, const thresholds &)
{
	return {};
}
std::vector<rule_id> rule_state::active() const
{
	return {};
}

std::string to_json_line(const sample &, std::span<const rule_id>)
{
	return {};
}
std::optional<sample> from_json_line(std::string_view)
{
	return std::nullopt;
}
} // namespace wivrn::health

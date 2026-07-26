// ecs/diag/sink.hpp
//
// The output seam shared by the schedule diagnostics observers
// (schedule_report.hpp, schedule_trace.hpp): a line sink they hand formatted
// lines to. This is the "configurable logger" -- wrap std::cout, a
// std::ofstream, or any logging backend. No engine dependency.

#pragma once

#include <functional>
#include <ostream>
#include <string_view>

namespace ecs::diag {

// A line sink: called with one formatted line at a time (no trailing newline of
// its own -- the sink adds line breaks if it wants them).
using Sink = std::function<void(std::string_view)>;

// A sink that writes each line to an ostream (std::cout, a std::ofstream, ...),
// one per line. The stream must outlive the sink.
inline Sink to_stream(std::ostream& os) {
    return [&os](std::string_view line) { os << line << '\n'; };
}

} // namespace ecs::diag

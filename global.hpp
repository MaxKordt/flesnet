#pragma once
/**
 * \file global.hpp
 *
 * 2012, 2013, Jan de Cuveland <cmail@cuveland.de>
 */

/// Overloaded output operator for STL vectors.
template <class T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& v)
{
    copy(v.begin(), v.end(), std::ostream_iterator<T>(os, " "));
    return os;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "einhard.hpp"
#pragma GCC diagnostic pop

extern einhard::Logger<static_cast<einhard::LogLevel>(MINLOGLEVEL), true> out;

class Parameters;
extern std::unique_ptr<Parameters> par;

extern std::vector<pid_t> child_pids;

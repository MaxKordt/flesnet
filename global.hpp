#pragma once
/**
 * \file global.hpp
 *
 * 2012, 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#include "einhard.hpp"

extern einhard::Logger<(einhard::LogLevel) MINLOGLEVEL, true> out;

class Parameters;
extern std::unique_ptr<Parameters> par;

extern std::vector<pid_t> child_pids;

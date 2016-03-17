// Copyright 2014 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include <boost/log/common.hpp>
#include <boost/log/utility/manipulators/to_log.hpp>

enum severity_level { trace, debug, info, warning, error, fatal };

namespace logging
{
// Attribute value tag type
struct severity_with_color_tag;
} // namespace logging

std::ostream& operator<<(std::ostream& strm, severity_level level);

boost::log::formatting_ostream& operator<<(
    boost::log::formatting_ostream& strm,
    boost::log::to_log_manip<severity_level,
                             logging::severity_with_color_tag> const& manip);

BOOST_LOG_GLOBAL_LOGGER(g_logger,
                        boost::log::sources::severity_logger_mt<severity_level>)

namespace logging
{
void add_console(severity_level minimum_severity);
void add_file(std::string filename, severity_level minimum_severity);
} // namespace logging

#define L_(severity) BOOST_LOG_SEV(g_logger::get(), severity)

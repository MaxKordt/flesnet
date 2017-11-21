// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include "Benchmark.hpp"
#include "Parameters.hpp"
#include "Sink.hpp"
#include "TimesliceSource.hpp"
#include "log.hpp"
#include <boost/iostreams/stream.hpp>
#include <chrono>
#include <iostream>
#include <memory>
#include <vector>

/// %Application base class.
class Application
{
public:
    explicit Application(Parameters const& par);

    Application(const Application&) = delete;
    void operator=(const Application&) = delete;

    ~Application();

    void run();

private:
    Parameters const& par_;

    std::unique_ptr<fles::TimesliceSource> source_;
    std::vector<std::unique_ptr<fles::TimesliceSink>> sinks_;
    std::unique_ptr<Benchmark> benchmark_;

    uint64_t count_ = 0;

    boost::iostreams::stream_buffer<logging::StatusSink> log_sink_;
    std::ostream logstream_;

    std::chrono::high_resolution_clock::time_point time_begin_;

    void rate_limit_delay() const;
};

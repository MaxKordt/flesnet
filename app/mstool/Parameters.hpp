// Copyright 2012-2015 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include <stdexcept>
#include <cstdint>
#include <string>

/// Run parameters exception class.
class ParametersException : public std::runtime_error
{
public:
    explicit ParametersException(const std::string& what_arg = "")
        : std::runtime_error(what_arg)
    {
    }
};

/// Global run parameters.
struct Parameters {
    Parameters(int argc, char* argv[]) { parse_options(argc, argv); }
    void parse_options(int argc, char* argv[]);

    // general options
    uint64_t maximum_number = UINT64_MAX;

    // source selection
    uint32_t pattern_generator_type = 0;
    bool use_pattern_generator = false;
    size_t shared_memory_channel = 0;
    bool use_shared_memory = false;
    std::string input_archive;

    // sink selection
    bool analyze = false;
    std::string output_archive;
    std::string output_shm_identifier;
};

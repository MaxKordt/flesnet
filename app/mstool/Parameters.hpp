// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include <stdexcept>
#include <cstdint>
#include <string>

/// Run parameter exception class.
class ParametersException : public std::runtime_error
{
public:
    explicit ParametersException(const std::string& what_arg = "")
        : std::runtime_error(what_arg)
    {
    }
};

/// Global run parameter class.
class Parameters
{
public:
    Parameters(int argc, char* argv[]) { parse_options(argc, argv); }

    Parameters(const Parameters&) = delete;
    void operator=(const Parameters&) = delete;

    uint32_t pattern_generator() const { return pattern_generator_; }

    std::string input_archive() const { return input_archive_; }

    std::string output_archive() const { return output_archive_; }

    uint64_t maximum_number() const { return maximum_number_; }

private:
    void parse_options(int argc, char* argv[]);

    uint32_t pattern_generator_ = 0;
    std::string input_archive_;
    std::string output_archive_;
    uint64_t maximum_number_ = UINT64_MAX;
};

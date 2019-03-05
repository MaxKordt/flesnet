// Copyright 2012-2015 Jan de Cuveland <cmail@cuveland.de>
// Copyright 2016 Pierre-Alain Loizeau <p.-a.loizeau@gsi.de>
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

/// Run parameters exception class.
class ParametersException : public std::runtime_error {
public:
  explicit ParametersException(const std::string& what_arg = "")
      : std::runtime_error(what_arg) {}
};

/// Global run parameters.
struct Parameters {
  Parameters(int argc, char* argv[]) { parse_options(argc, argv); }
  void parse_options(int argc, char* argv[]);

  // general options
  uint64_t maximum_number = UINT64_MAX;

  // source selection
  size_t shm_channel = 0;
  std::string input_shm;
  std::string input_archive;

  // Operation mode
  bool debugger = false;
  bool sorter = false;
  uint32_t epoch_per_ms = 1;
  bool sortmesg = false;

  // sink selection
  size_t dump_verbosity = 0;
  std::string output_shm;
  std::string output_archive;
};

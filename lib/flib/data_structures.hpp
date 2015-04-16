/**
 * @file
 * @author Dirk Hutter <hutter@compeng.uni-frankfurt.de>
 * @author Dominic Eschweiler<dominic.eschweiler@cern.ch>
 *
 */
#pragma once

#include <cstdint>
#include <unistd.h>

#include <string>
#include <stdexcept>

typedef uint64_t sys_bus_addr;

// TODO
// move structs to apropriate files

namespace flib {
class FlibException : public std::runtime_error {
public:
  explicit FlibException(const std::string& what_arg = "")
      : std::runtime_error(what_arg) {}
};

struct mc_desc {
  uint64_t nr;
  volatile uint64_t* addr;
  uint32_t size; // bytes
  volatile uint64_t* rbaddr;
};

struct __attribute__((__packed__)) hdr_config {
  uint16_t eq_id;  // "Equipment identifier"
  uint8_t sys_id;  // "Subsystem identifier"
  uint8_t sys_ver; // "Subsystem format version"
};

struct ctrl_msg {
  uint32_t words; // num 16 bit data words
  uint16_t data[32];
};
}

// has to be 256 Bit, this is hard coded in hw
struct __attribute__((__packed__)) MicrosliceDescriptor {
  uint8_t hdr_id;  // "Header format identifier" DD
  uint8_t hdr_ver; // "Header format version"    01
  uint16_t eq_id;  // "Equipment identifier"
  uint16_t flags;  // "Status and error flags"
  uint8_t sys_id;  // "Subsystem identifier"
  uint8_t sys_ver; // "Subsystem format version"
  uint64_t idx;    // "Microslice index"
  uint32_t crc;    // "CRC32 checksum"
  uint32_t size;   // "Size bytes"
  uint64_t offset; // "Ofsset in event buffer"
};

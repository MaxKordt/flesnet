#pragma once

#include <cstdint>
#include <boost/interprocess/sync/interprocess_mutex.hpp>
#include <boost/interprocess/sync/interprocess_condition.hpp>

using namespace boost::interprocess;

class shm_device {

public:

  shm_device() {}
  
  ~shm_device() {}

  void inc_num_channels() {
    ++m_num_channels;
  }

  void dec_num_channels() {
    --m_num_channels;
  }

  size_t num_channels() { return m_num_channels; }
  
  //interprocess_mutex& mutex() { return m_mutex; }

  //interprocess_condition& cond_req() { return m_cond_req; }

  
  interprocess_mutex m_mutex;
  interprocess_condition m_cond_req;

private:

  size_t m_num_channels = 0;
  
};

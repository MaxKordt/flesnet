
#include <cstdint>
#include <boost/interprocess/managed_shared_memory.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>

#include "flib_link.hpp"

#include "shm_channel.hpp"

using namespace boost::interprocess;
using namespace flib;

class shm_channel_server {

public:

  shm_channel_server(managed_shared_memory* shm,
                     size_t index,
                     flib_link* flib_link,
                     size_t data_buffer_size_exp,
                     size_t desc_buffer_size_exp)
    : m_shm(shm), m_index(index), m_flib_link(flib_link),
      m_data_buffer_size_exp(data_buffer_size_exp),
      m_desc_buffer_size_exp(desc_buffer_size_exp)
  {
    // allocate buffers
    m_data_buffer = alloc_buffer(m_data_buffer_size_exp);
    m_desc_buffer = alloc_buffer(m_desc_buffer_size_exp);

    // constuct channel exchange object in sharde memory
    std::string channel_name = "shm_channel_" +
      boost::lexical_cast<std::string>(m_index);
    m_shm_ch = m_shm->construct<shm_channel>(channel_name.c_str())
      (m_shm, m_data_buffer, m_data_buffer_size_exp,
       m_desc_buffer, m_desc_buffer_size_exp);

    // initialize flib DMA engine
    m_flib_link->init_dma(m_data_buffer, m_data_buffer_size_exp,
                          m_desc_buffer, m_desc_buffer_size_exp);

    // TODO set start index and enable packer at matching position
  }

  ~shm_channel_server() {
    m_flib_link->deinit_dma();
    // TODO
    // destroy channel object and deallocate buffers if it is worth to do
  }


  bool check_pending_req(scoped_lock<interprocess_mutex>& lock) {
    assert(lock); // ensure mutex is realy owned
    return m_shm_ch->req_ptr(lock) || m_shm_ch->req_offset(lock);
  }


  void try_handle_req(scoped_lock<interprocess_mutex>& lock) {
    assert(lock); // ensure mutex is realy owned
    
    if (m_shm_ch->req_ptr(lock)) {
      m_shm_ch->read_ptrs(lock, m_data_ptr_cached, m_desc_ptr_cached);
      // reset req before releasing lock ensures not to miss last req
      m_shm_ch->set_req_ptr(lock, false);
      lock.unlock();
      // TODO do fancy HW stuff here
      lock.lock();
    }
    
    if (m_shm_ch->req_offset(lock)) {
      m_shm_ch->set_req_offset(lock, false);
      lock.unlock();
      // TODO fetch information from HW
      // set m_data_offset_cached
      lock.lock();
      m_shm_ch->set_offsets(lock, m_data_offset_cached, m_desc_offset_cached);
    }
  }
  
private:

  void* alloc_buffer(size_t size_exp) {
    return m_shm->allocate_aligned(UINT64_C(1) << size_exp, sysconf(_SC_PAGESIZE));
  }
  
  
  managed_shared_memory* m_shm;
  size_t m_index;
  flib_link* m_flib_link;
  
  shm_channel* m_shm_ch;
  void* m_data_buffer;
  void* m_desc_buffer;
  size_t m_data_buffer_size_exp; // TODO do not double data, already in shm
  size_t m_desc_buffer_size_exp;
  
  uint64_t m_data_ptr_cached = 0;
  uint64_t m_desc_ptr_cached = 0;
  uint64_t m_data_offset_cached = 0;
  uint64_t m_desc_offset_cached = 0;
  
};

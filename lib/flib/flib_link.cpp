/**
 * @file
 * @author Dirk Hutter <hutter@compeng.uni-frankfurt.de>
 * @author Dominic Eschweiler<dominic.eschweiler@cern.ch>
 *
 */

#include <cstring>
#include <string>
#include <iostream>
#include <sstream>
#include <cassert>
#include <memory>
#include <stdexcept>

#include <pda/device.hpp>
#include <pda/dma_buffer.hpp>
#include <pda/pci_bar.hpp>
#include <pda/data_structures.hpp>

#include <flib_link.hpp>
#include <dma_channel.hpp>
#include <flib_device.hpp>
#include <flib_link.hpp>
#include <register_file.hpp>
#include <register_file_bar.hpp>

using namespace pda;

namespace flib {
flib_link::flib_link(size_t link_index, device* dev, pci_bar* bar)
    : m_link_index(link_index), m_device(dev) {
  m_base_addr = (m_link_index + 1) * RORC_CHANNEL_OFFSET;
  // register file access
  m_rfpkt = std::unique_ptr<register_file_bar>(
      new register_file_bar(bar, m_base_addr));
  m_rfgtx = std::unique_ptr<register_file_bar>(
      new register_file_bar(bar, (m_base_addr + (1 << RORC_DMA_CMP_SEL))));
  m_rfglobal =
      std::unique_ptr<register_file_bar>(new register_file_bar(bar, 0));
  // create DMA channel and bind to register file,
  // no HW initialization is done here
  m_channel = std::unique_ptr<dma_channel>(new dma_channel(m_rfpkt.get(), dev));
}

flib_link::~flib_link() {
  stop();
}

void flib_link::init_dma(register_only_t,
                        void* ebuf,
                        size_t log_ebufsize,
                        void* dbuf,
                        size_t log_dbufsize) {
  m_log_ebufsize = log_ebufsize;
  m_log_dbufsize = log_dbufsize;
  m_data_buffer =
    std::unique_ptr<dma_buffer>(new dma_buffer(m_device, ebuf, (1ull << log_ebufsize), (2*m_link_index+0)));
  m_desc_buffer =
    std::unique_ptr<dma_buffer>(new dma_buffer(m_device, dbuf, (1ull << log_dbufsize), (2*m_link_index+1)));
  init_hardware();
  m_dma_initialized = true;
}

void flib_link::init_dma(create_only_t,
                         size_t log_ebufsize,
                         size_t log_dbufsize) {
  m_log_ebufsize = log_ebufsize;
  m_log_dbufsize = log_dbufsize;
  m_data_buffer =
    std::unique_ptr<dma_buffer>(new dma_buffer(m_device, (1ull << log_ebufsize), (2*m_link_index+0)));
  m_desc_buffer =
    std::unique_ptr<dma_buffer>(new dma_buffer(m_device, (1ull << log_dbufsize), (2*m_link_index+1)));
  init_hardware();
  m_dma_initialized = true;
}
   
void flib_link::init_dma(open_only_t,
                         size_t log_ebufsize,
                         size_t log_dbufsize) {
  m_log_ebufsize = log_ebufsize;
  m_log_dbufsize = log_dbufsize;
  m_data_buffer = std::unique_ptr<dma_buffer>(new dma_buffer(m_device, (2*m_link_index+0)));
  m_desc_buffer = std::unique_ptr<dma_buffer>(new dma_buffer(m_device, (2*m_link_index+1)));
  init_hardware();
  m_dma_initialized = true;
}

/*** MC access funtions ***/

std::pair<mc_desc, bool> flib_link::mc() {
  struct mc_desc mc;
  if (m_db[m_index].idx > m_mc_nr) { // mc_nr counts from 1 in HW
    m_mc_nr = m_db[m_index].idx;
    mc.nr = m_mc_nr;
    mc.addr =
        m_eb +
        (m_db[m_index].offset & ((1 << m_log_ebufsize) - 1)) / sizeof(uint64_t);
    mc.size = m_db[m_index].size;
    mc.rbaddr = (uint64_t*)&m_db[m_index];

    // calculate next rb index
    m_last_index = m_index;
    if (m_index < m_dbentries - 1) {
      m_index++;
    } else {
      m_wrap++;
      m_index = 0;
    }
    return std::make_pair(mc, true);
  }

  return std::make_pair(mc, false);
}

int flib_link::ack_mc() {

  // TODO: EB pointers are set to begin of acknoledged entry, pointers are one
  // entry delayed
  // to calculate end wrapping logic is required
  uint64_t eb_offset = m_db[m_last_index].offset & ((1 << m_log_ebufsize) - 1);
  // each rbenty is 32 bytes, this is hard coded in HW
  uint64_t rb_offset = m_last_index * sizeof(struct MicrosliceDescriptor) &
                       ((1 << m_log_dbufsize) - 1);

  //_ch->setEBOffset(eb_offset);
  //_ch->setRBOffset(rb_offset);

  m_channel->setOffsets(eb_offset, rb_offset);

#ifdef DEBUG
  printf("index %d EB offset set: %ld, get: %ld\n", m_last_index, eb_offset,
         m_channel->getEBOffset());
  printf("index %d RB offset set: %ld, get: %ld, wrap %d\n", m_last_index,
         rb_offset, m_channel->getRBOffset(), m_wrap);
#endif

  return 0;
}

/*** Configuration and control ***/

void flib_link::set_start_idx(uint64_t index) {
  // set reset value
  // TODO replace with _rfgtx->set_mem()
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG_IDX_L,
                   (uint32_t)(index & 0xffffffff));
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG_IDX_H, (uint32_t)(index >> 32));
  // reste mc counter
  // TODO implenet edge detection and 'pulse only' in HW
  uint32_t mc_gen_cfg = m_rfgtx->reg(RORC_REG_GTX_MC_GEN_CFG);
  // TODO replace with _rfgtx->set_bit()
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg | 1));
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg & ~(1)));
}

void flib_link::rst_pending_mc() {
  // Is also resetted with datapath reset
  // TODO implenet edge detection and 'pulse only' in HW
  uint32_t mc_gen_cfg = m_rfgtx->reg(RORC_REG_GTX_MC_GEN_CFG);
  // TODO replace with _rfgtx->set_bit()
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg | (1 << 1)));
  m_rfgtx->set_reg(RORC_REG_GTX_MC_GEN_CFG, (mc_gen_cfg & ~(1 << 1)));
}

void flib_link::rst_cnet_link() {
  m_rfgtx->set_bit(RORC_REG_GTX_DATAPATH_CFG, 3, true);
  usleep(1000);
  m_rfgtx->set_bit(RORC_REG_GTX_DATAPATH_CFG, 3, false);
}

void flib_link::enable_cbmnet_packer(bool enable) {
  m_rfgtx->set_bit(RORC_REG_GTX_MC_GEN_CFG, 2, enable);
}

void flib_link::enable_cbmnet_packer_debug_mode(bool enable) {
  m_rfgtx->set_bit(RORC_REG_GTX_MC_GEN_CFG, 3, enable);
}

/*** CBMnet control interface ***/

int flib_link::send_dcm(const struct ctrl_msg* msg) {

  assert(msg->words >= 4 && msg->words <= 32);

  // check if send FSM is ready (bit 31 in r_ctrl_tx = 0)
  if ((m_rfgtx->reg(RORC_REG_GTX_CTRL_TX) & (1 << 31)) != 0) {
    return -1;
  }

  // copy msg to board memory
  size_t bytes = msg->words * 2 + (msg->words * 2) % 4;
  m_rfgtx->set_mem(RORC_MEM_BASE_CTRL_TX, (const void*)msg->data, bytes >> 2);

  // start send FSM
  uint32_t ctrl_tx = 0;
  ctrl_tx = 1 << 31 | (msg->words - 1);
  m_rfgtx->set_reg(RORC_REG_GTX_CTRL_TX, ctrl_tx);

  return 0;
}

int flib_link::recv_dcm(struct ctrl_msg* msg) {

  int ret = 0;
  uint32_t ctrl_rx = m_rfgtx->reg(RORC_REG_GTX_CTRL_RX);
  msg->words = (ctrl_rx & 0x1F) + 1;

  // check if a msg is available
  if ((ctrl_rx & (1 << 31)) == 0) {
    return -1;
  }
  // check if received words are in boundary
  // append or truncate if not
  if (msg->words < 4 || msg->words > 32) {
    msg->words = 32;
    ret = -2;
  }

  // read msg from board memory
  size_t bytes = msg->words * 2 + (msg->words * 2) % 4;
  m_rfgtx->mem(RORC_MEM_BASE_CTRL_RX, (void*)msg->data, bytes >> 2);

  // acknowledge msg
  m_rfgtx->set_reg(RORC_REG_GTX_CTRL_RX, 0);

  return ret;
}

void flib_link::prepare_dlm(uint8_t type, bool enable) {
  uint32_t reg = 0;
  if (enable) {
    reg = (1 << 4) | (type & 0xF);
  } else {
    reg = (type & 0xF);
  }
  m_rfgtx->set_reg(RORC_REG_GTX_DLM, reg);
  // dummy read on GTX regfile to ensure reg written
  // before issuing send pulse in send_dlm()
  m_rfgtx->reg(RORC_REG_GTX_DLM);
}

void flib_link::send_dlm() {
  // TODO: hack to overcome SW issues
  // DLM send is also avialable in link
  // will send DLMs on all prepared links

  // TODO; HW link could unprepare automatically
  // after one DLM is sent

  // global send
  // m_rfglobal->set_reg(RORC_REG_DLM_CFG, 1);
  // local HW send, not well tested
  m_rfgtx->set_bit(RORC_REG_GTX_DLM, 30, true);
}

uint8_t flib_link::recv_dlm() {
  // get dlm rx reg
  uint32_t reg = m_rfgtx->reg(RORC_REG_GTX_DLM);
  uint8_t type = (reg >> 5) & 0xF;
  // clear dlm rx reg
  m_rfgtx->set_bit(RORC_REG_GTX_DLM, 31, true);
  return type;
}

/*** SETTER ***/
void flib_link::set_data_sel(data_sel_t rx_sel) {
  uint32_t dp_cfg = m_rfgtx->reg(RORC_REG_GTX_DATAPATH_CFG);
  switch (rx_sel) {
  case rx_disable:
    m_rfgtx->set_reg(RORC_REG_GTX_DATAPATH_CFG, (dp_cfg & ~3));
    break;
  case rx_link:
    m_rfgtx->set_reg(RORC_REG_GTX_DATAPATH_CFG, ((dp_cfg | (1 << 1)) & ~1));
    break;
  case rx_pgen:
    m_rfgtx->set_reg(RORC_REG_GTX_DATAPATH_CFG, (dp_cfg | 3));
    break;
  case rx_emu:
    m_rfgtx->set_reg(RORC_REG_GTX_DATAPATH_CFG, ((dp_cfg | 1)) & ~(1 << 1));
    break;
  }
}

void flib_link::set_hdr_config(const struct hdr_config* config) {
  m_rfgtx->set_mem(RORC_REG_GTX_MC_GEN_CFG_HDR, (const void*)config,
                   sizeof(hdr_config) >> 2);
}

/*** GETTER ***/
uint64_t flib_link::pending_mc() {
  // TODO replace with _rfgtx->get_mem()
  uint64_t pend_mc = m_rfgtx->reg(RORC_REG_GTX_PENDING_MC_L);
  pend_mc =
      pend_mc | ((uint64_t)(m_rfgtx->reg(RORC_REG_GTX_PENDING_MC_H)) << 32);
  return pend_mc;
}

uint64_t flib_link::mc_index() {
  // TODO replace with _rfgtx->get_mem()
  uint64_t mc_index = m_rfgtx->reg(RORC_REG_GTX_MC_INDEX_L);
  mc_index =
      mc_index | ((uint64_t)(m_rfgtx->reg(RORC_REG_GTX_MC_INDEX_H)) << 32);
  return mc_index;
}

uint64_t flib_link::mc_offset() {
  // TODO replace with _rfpkt->get_mem()
  uint64_t mc_offset = m_rfpkt->reg(RORC_REG_EBDM_OFFSET_L);
  mc_offset =
      mc_offset | ((uint64_t)(m_rfpkt->reg(RORC_REG_EBDM_OFFSET_H)) << 32);
  return mc_offset;
}

flib_link::data_sel_t flib_link::data_sel() {
  uint32_t dp_cfg = m_rfgtx->reg(RORC_REG_GTX_DATAPATH_CFG);
  return static_cast<data_sel_t>(dp_cfg & 0x3);
}

  // CBMnet diagnostics
uint32_t flib_link::diag_pcs_startup() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_PCS_STARTUP);
}

uint32_t flib_link::diag_ebtb_code_err() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_EBTB_CODE_ERR);
}

uint32_t flib_link::diag_ebtb_disp_err() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_EBTB_DISP_ERR);
}

uint32_t flib_link::diag_crc_error() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_CRC_ERROR);
}

uint32_t flib_link::diag_packet() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_PACKET);
}

uint32_t flib_link::diag_packet_err() {
  return m_rfgtx->reg(RORC_REG_GTX_DIAG_PACKET_ERR);
}

flib_link::diag_flags_t flib_link::diag_flags() {
  uint32_t reg =  m_rfgtx->reg(RORC_REG_GTX_DIAG_FLAGS);
  diag_flags_t flags;
  flags.pcs_startup   = (reg & (1));
  flags.ebtb_code_err = (reg & (1 << 1));
  flags.ebtb_disp_err = (reg & (1 << 2));
  flags.crc_error     = (reg & (1 << 3));
  flags.packet        = (reg & (1 << 4));
  flags.packet_err    = (reg & (1 << 5));
  flags.rx_clk_stable = (reg & (1 << 6));
  flags.tx_clk_stable = (reg & (1 << 7));
  flags.ebtb_detect   = (reg & (1 << 8));
  flags.serdes_ready  = (reg & (1 << 9));
  flags.link_active   = (reg & (1 << 10));
  return flags;
}

void flib_link::diag_clear() {
  m_rfgtx->set_reg(RORC_REG_GTX_DIAG_CLEAR, 0xFFFFFFFF);
}

std::string flib_link::data_buffer_info() {
  return print_buffer_info(m_data_buffer.get());
}

std::string flib_link::desc_buffer_info() {
  return print_buffer_info(m_desc_buffer.get());
}

void* flib_link::data_buffer() const {
  return reinterpret_cast<void*>(m_data_buffer->mem());
}

void* flib_link::desc_buffer() const {
  return reinterpret_cast<void*>(m_desc_buffer->mem());
}

dma_channel* flib_link::channel() const { return m_channel.get(); }

register_file_bar* flib_link::register_file_packetizer() const { return m_rfpkt.get(); }

register_file_bar* flib_link::register_file_gtx() const { return m_rfgtx.get(); }

flib_link::link_status_t flib_link::link_status() {
  uint32_t sts = m_rfgtx->reg(RORC_REG_GTX_DATAPATH_STS);

  struct link_status_t link_status;
  link_status.link_active = (sts & (1));
  link_status.data_rx_stop = (sts & (1 << 1));
  link_status.ctrl_rx_stop = (sts & (1 << 2));
  link_status.ctrl_tx_stop = (sts & (1 << 3));

  return link_status;
}

/*** PROTECTED ***/
  
void flib_link::reset_channel() {
  // datapath reset, will also cause hw defaults for
  // - pending mc  = 0
  m_rfgtx->set_bit(RORC_REG_GTX_DATAPATH_CFG, 2, true);
  // rst packetizer fifos
  m_channel->setDMAConfig(0X2);
  // release datapath reset
  m_rfgtx->set_bit(RORC_REG_GTX_DATAPATH_CFG, 2, false);
}

void flib_link::stop() {
  if (m_channel && m_dma_initialized) {
    // disable packer
    enable_cbmnet_packer(false);
    // disable DMA Engine
    m_channel->enableEB(0);
    // wait for pending transfers to complete (dma_busy->0)
    while (m_channel->isDMABusy()) {
      usleep(100);
    }

    // disable RBDM
    m_channel->enableRB(0);
    // reset
    reset_channel();
  }
}

void flib_link::init_hardware() {
  // disable packer if still enabled
  enable_cbmnet_packer(false);
  // reset everything to ensure clean startup
  reset_channel();
  set_start_idx(1);

  /** prepare EventBufferDescriptorManager
   * and ReportBufferDescriptorManage
   * with scatter-gather list
   **/
  if (m_channel->prepareEB(m_data_buffer.get()) < 0) {
    throw FlibException("Preparing data buffer failed");
  }

  if (m_channel->prepareRB(m_desc_buffer.get()) < 0) {
    throw FlibException("Preparing descriptor buffer failed");
  }

  if (m_channel->configureChannel(m_data_buffer.get(), m_desc_buffer.get(), 128) <
      0) {
    throw FlibException("Configuring DMA channel failed");
  }

  // clear eb for debugging
  memset(m_data_buffer->mem(), 0, m_data_buffer->mappingSize());
  // clear rb for polling
  memset(m_desc_buffer->mem(), 0, m_desc_buffer->mappingSize());

  m_eb = (uint64_t*)m_data_buffer->mem();
  m_db = (struct MicrosliceDescriptor*)m_desc_buffer->mem();

  m_dbentries = m_desc_buffer->physicalSize() /
    sizeof(struct MicrosliceDescriptor);

  // Enable desciptor buffers and dma engine
  m_channel->enableEB(1);
  m_channel->enableRB(1);
  m_channel->setDMAConfig(m_channel->DMAConfig() | 0x01);
}

std::string flib_link::print_buffer_info(dma_buffer* buf) {
  std::stringstream ss;
  ss << "start address = " << buf->mem() << ", "
     << "physical size = " << (buf->physicalSize() >> 20) << " MByte, "
     << "mapping size = " << (buf->mappingSize() >> 20) << " MByte, "
     << std::endl << "  end address = "
     << (void*)((uint8_t*)buf->mem() + buf->physicalSize()) << ", "
     << "num SG entries = " << buf->numberOfSGEntries();
  return ss.str();
}
}

/**
 * @file
 * @author Dirk Hutter <hutter@compeng.uni-frankfurt.de>
 *
 */

#include <cassert>
#include <memory>
#include <arpa/inet.h> // ntohl
#include <iomanip>

#include <flim.hpp>

namespace flib {

flim::flim(flib_link_flesin* link) : m_parrent_link(link) {
  m_rfflim = std::unique_ptr<register_file>(new register_file_bar(
      link->bar(), (link->base_addr() + (1 << RORC_DMA_CMP_SEL) + (1 << 5))));
  if (hardware_id() != 0x4844) {
    std::stringstream msg;
    msg << "FLIM not reachable; found ID: " << std::hex << hardware_id();
    throw FlibException(msg.str());
  }
  if (hardware_ver() != 2) {
    std::stringstream msg;
    msg << "FLIM hardware version not supported; found ver: " << hardware_ver();
    throw FlibException(msg.str());
  }
}

flim::~flim() {}
// Do not reset on destruction to prevnet loss of hw state after config.

//////*** FLIM Configuration and Status***//////

void flim::reset_datapath() {
  m_rfflim->set_bit(RORC_REG_LINK_FLIM_CFG, 2, true);
  m_rfflim->set_bit(RORC_REG_LINK_FLIM_CFG, 2, false);
}

void flim::set_ready_for_data(bool enable) {
  m_rfflim->set_bit(RORC_REG_LINK_FLIM_CFG, 0, enable);
}
void flim::set_data_source(flim::data_source_t sel) {
  m_rfflim->set_bit(RORC_REG_LINK_FLIM_CFG, 1, sel);
}

void flim::set_start_idx(uint64_t idx) {
  m_rfflim->set_mem(RORC_REG_LINK_MC_PACKER_CFG_IDX_L, &idx, 2);
  m_rfflim->set_bit(RORC_REG_LINK_FLIM_CFG, 31, true); // pulse bit
}

uint64_t flim::get_mc_idx() {
  uint64_t idx;
  m_rfflim->get_mem(RORC_REG_LINK_MC_INDEX_L, &idx, 2);
  return idx;
}

bool flim::get_pgen_present() {
  return m_rfflim->get_bit(RORC_REG_LINK_FLIM_STS, 0);
}

flim::sts_t flim::get_sts() {
  uint32_t reg = m_rfflim->get_reg(RORC_REG_LINK_FLIM_STS);
  flim::sts_t sts;
  sts.hard_err = (reg & (1 << 1));
  sts.soft_err = (reg & (1 << 2));
  return sts;
}

void flim::set_pgen_mc_size(uint32_t size) {
  m_rfflim->set_reg(RORC_REG_LINK_MC_PGEN_MC_SIZE, size);
}

void flim::set_pgen_ids(uint16_t eq_id) {
  m_rfflim->set_reg(RORC_REG_LINK_MC_PGEN_IDS, eq_id);
}

void flim::set_pgen_rate(float val) {
  assert(val >= 0);
  assert(val <= 1);
  uint16_t reg_val =
      static_cast<uint16_t>(static_cast<float>(UINT16_MAX) * (1.0 - val));
  m_rfflim->set_reg(RORC_REG_LINK_MC_PGEN_CFG,
                    static_cast<uint32_t>(reg_val) << 16,
                    0xFFFF0000);
}

void flim::set_pgen_enable(bool enable) {
  m_rfflim->set_bit(RORC_REG_LINK_MC_PGEN_CFG, 0, enable);
}

void flim::reset_pgen_mc_pending() {
  m_rfflim->set_bit(RORC_REG_LINK_MC_PGEN_CFG, 1, true); // pulse bit
}

uint32_t flim::get_pgen_mc_pending() {
  return m_rfflim->get_reg(RORC_REG_LINK_MC_PGEN_MC_PENDING);
}

//////*** FLIM Test and Debug ***//////

void flim::set_testreg(uint32_t data) {
  m_rfflim->set_reg(RORC_REG_LINK_TEST, data);
}

uint32_t flim::get_testreg() { return m_rfflim->get_reg(RORC_REG_LINK_TEST); }

void flim::set_debug_out(bool enable) {
  m_rfflim->set_bit(RORC_REG_LINK_TEST, 0, enable);
}

//////*** FLIM Hardware Info ***//////

uint16_t flim::hardware_ver() {
  return (static_cast<uint16_t>(m_rfflim->get_reg(0) >> 16));
  // RORC_REG_LINK_FLIM_HW_INFO
}

uint16_t flim::hardware_id() {
  return (static_cast<uint16_t>(m_rfflim->get_reg(0)));
  // RORC_REG_LINK_FLIM_HW_INFO
}

time_t flim::build_date() {
  time_t time = (static_cast<time_t>(
      m_rfflim->get_reg(RORC_REG_FLIM_BUILD_DATE_L) |
      (static_cast<uint64_t>(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_DATE_H))
       << 32)));
  return time;
}

std::string flim::build_host() {
  uint32_t host[4];
  host[0] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_HOST_3));
  host[1] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_HOST_2));
  host[2] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_HOST_1));
  host[3] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_HOST_0));
  return std::string(reinterpret_cast<const char*>(host));
}

std::string flim::build_user() {
  uint32_t user[4];
  user[0] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_USER_3));
  user[1] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_USER_2));
  user[2] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_USER_1));
  user[3] = ntohl(m_rfflim->get_reg(RORC_REG_FLIM_BUILD_USER_0));
  return std::string(reinterpret_cast<const char*>(user));
}

flim::build_info_t flim::build_info() {
  flim::build_info_t info;

  info.date = build_date();
  info.host = build_host();
  info.user = build_user();
  info.rev[0] = m_rfflim->get_reg(RORC_REG_FLIM_BUILD_REV_0);
  info.rev[1] = m_rfflim->get_reg(RORC_REG_FLIM_BUILD_REV_1);
  info.rev[2] = m_rfflim->get_reg(RORC_REG_FLIM_BUILD_REV_2);
  info.rev[3] = m_rfflim->get_reg(RORC_REG_FLIM_BUILD_REV_3);
  info.rev[4] = m_rfflim->get_reg(RORC_REG_FLIM_BUILD_REV_4);
  info.hw_ver = hardware_ver();
  info.clean = (m_rfflim->get_reg(RORC_REG_FLIM_BUILD_FLAGS) & 0x1);
  info.repo = (m_rfflim->get_reg(RORC_REG_FLIM_BUILD_FLAGS) & 0x6) >> 1;
  return info;
}

std::string flim::print_build_info() {
  flim::build_info_t build = build_info();

  // TODO: hack to overcome gcc limitation, for c++11 use:
  // std::put_time(std::localtime(&build.date), "%c %Z")
  char mbstr[100];
  std::strftime(
      mbstr, sizeof(mbstr), "%c %Z UTC%z", std::localtime(&build.date));

  std::stringstream ss;
  ss << "FLIM Info:" << std::endl << "Build Date:     " << mbstr << std::endl
     << "Build Source:   " << build.user << "@" << build.host << std::endl;
  switch (build.repo) {
  case 1:
    ss << "Build from a git repository" << std::endl
       << "Repository Revision: " << std::hex << std::setfill('0')
       << std::setw(8) << build.rev[4] << std::setfill('0') << std::setw(8)
       << build.rev[3] << std::setfill('0') << std::setw(8) << build.rev[2]
       << std::setfill('0') << std::setw(8) << build.rev[1] << std::setfill('0')
       << std::setw(8) << build.rev[0] << std::endl;
    break;
  case 2:
    ss << "Build from a svn repository" << std::endl
       << "Repository Revision: " << std::dec << build.rev[0] << std::endl;
    break;
  default:
    ss << "Build from a unknown repository" << std::endl;
    break;
  }
  if (build.clean) {
    ss << "Repository Status:   clean " << std::endl;
  } else {
    ss << "Repository Status:   NOT clean " << std::endl;
  }
  ss << "Hardware Version:    " << std::dec << build.hw_ver;
  return ss.str();
}

} // namespace

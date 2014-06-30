#pragma once

#include <iostream>
#include <iomanip>
#include <string>
#include <boost/program_options.hpp>
#include <boost/numeric/conversion/cast.hpp>
#include <fstream>
#include <flib.h>

namespace po = boost::program_options;

static const size_t _num_flib_links = 4;  

struct link_config {
  flib::flib_link::data_rx_sel rx_sel;
  flib::hdr_config hdr_config;
};

class parameters {

public:

  parameters(int argc, char* argv[]) { parse_options(argc, argv); }
  
  parameters(const parameters&) = delete;
  void operator=(const parameters&) = delete;

  uint32_t mc_size() const { return _mc_size; }
  
  struct link_config link_config(size_t i) const { return _link_config.at(i); }

private:
  
  void parse_options(int argc, char* argv[]) {
    
    po::options_description generic("Generic options");
    generic.add_options()
      ("help,h", "produce help message")
      ;

    po::options_description config("Configuration (flib.cfg or cmd line)");
    config.add_options()
      ("mc-size,t", po::value<uint32_t>(),
       "global size of microslices in units of 8 ns (31 bit wide)")
            
      ("l0_source", po::value<std::string>(), 
       "Link 0 data source <disable|link|pgen|emu>")
      ("l0_sys_id", po::value<std::string>(),
       "Subsystem identifier of link 0 data source (8 Bit)")
      ("l0_sys_ver", po::value<std::string>(),
       "Subsystem format version of link 0 data source (8 Bit)")
      
      ("l1_source", po::value<std::string>(), 
       "Link 1 data source <disable|link|pgen|emu>")
      ("l1_sys_id", po::value<std::string>(),
       "Subsystem identifier of link 1 data source (8 Bit)")
      ("l1_sys_ver", po::value<std::string>(),
       "Subsystem format version of link 1 data source (8 Bit)")
      
      ("l2_source", po::value<std::string>(), 
       "Link 2 data source <disable|link|pgen|emu>")
      ("l2_sys_id", po::value<std::string>(),
       "Subsystem identifier of link 2 data source (8 Bit)")
      ("l2_sys_ver", po::value<std::string>(),
       "Subsystem format version of link 2 data source (8 Bit)")
      
      ("l3_source", po::value<std::string>(), 
       "Link 3 data source <disable|link|pgen|emu>")
      ("l3_sys_id", po::value<std::string>(),
       "Subsystem identifier of link 3 data source (8 Bit)")
      ("l3_sys_ver", po::value<std::string>(),
       "Subsystem format version of link 3 data source (8 Bit)")
      ;

    po::options_description cmdline("Allowed options");
    cmdline.add(generic).add(config);

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, cmdline), vm);
    std::ifstream ifs("flib.cfg");
    po::store(po::parse_config_file(ifs, config), vm);
    po::notify(vm);    
    
    if (vm.count("help")) {
      std::cout << cmdline << "\n";
      exit(EXIT_SUCCESS);
    }
    
    if (vm.count("mc-size")) {
      _mc_size = vm["mc-size"].as<uint32_t>();
      if (_mc_size > 2147483647) { // 31 bit check
        std::cout << "Microslice size out of range" << std::endl;
        exit(EXIT_SUCCESS);
      } else {
        std::cout << "Microslice size set to " 
           << _mc_size << " * 8 ns.\n";  
      }
    } else {
      std::cout << "Microslice size set to default.\n";
    }

    for (size_t i = 0; i < _num_flib_links; ++i) {
      std::cout << "Link " << i << " config:" << std::endl;
      
      if (vm.count("l" + std::to_string(i) + "_source")) { // set given parameters
        std::string source = vm["l" + std::to_string(i) + "_source"].as<std::string>();
        
        if ( source == "link" ) {
          _link_config.at(i).rx_sel = flib::flib_link::link;
          std::cout << " data source: link" << std::endl;
          if (vm.count("l" + std::to_string(i) + "_sys_id") && vm.count("l" + std::to_string(i) + "_sys_ver")) {
            _link_config.at(i).hdr_config.sys_id = boost::numeric_cast<uint8_t>
              (std::stoul(vm["l" + std::to_string(i) + "_sys_id"].as<std::string>(),nullptr,0));
            _link_config.at(i).hdr_config.sys_ver = boost::numeric_cast<uint8_t>
              (std::stoul(vm["l" + std::to_string(i) + "_sys_ver"].as<std::string>(),nullptr,0));
            std::cout << std::hex <<
              " sys_id:      0x" << (uint32_t)_link_config.at(i).hdr_config.sys_id << "\n" <<
              " sys_ver:     0x" << (uint32_t)_link_config.at(i).hdr_config.sys_ver << std::endl;
          } else {
            std::cout << 
              " If reading from 'link' please provide sys_id and sys_ver.\n";
            exit(EXIT_SUCCESS);
          }
        
        } else if (source == "pgen") {
          _link_config.at(i).rx_sel = flib::flib_link::pgen;
          std::cout << " data source: pgen" << std::endl;
          _link_config.at(i).hdr_config.sys_id = 0xF0;
          _link_config.at(i).hdr_config.sys_ver = 0x01;
        
        } else if (source == "disable") {
          _link_config.at(i).rx_sel = flib::flib_link::disable;
          std::cout << " data source: disable" << std::endl;
          _link_config.at(i).hdr_config.sys_id = 0xF0;
          _link_config.at(i).hdr_config.sys_ver = 0x01;
        
        } else if (source == "emu") {
          _link_config.at(i).rx_sel = flib::flib_link::emu;
          std::cout << " data source: emu" << std::endl;
          _link_config.at(i).hdr_config.sys_id = 0xF1;
          _link_config.at(i).hdr_config.sys_ver = 0x01;
        
        } else {
          std::cout << " No valid arg for data source." << std::endl;
          exit(EXIT_SUCCESS);
        }
      
      } else { // set default parameters
        _link_config.at(i).rx_sel = flib::flib_link::disable;
        std::cout << " data source: disable (default)" << std::endl;
        _link_config.at(i).hdr_config.sys_id = 0xF0;
        _link_config.at(i).hdr_config.sys_ver = 0x01;        
      }
    
    } // end loop over links
  }
    
  uint32_t _mc_size = 125; // 1 us
  std::array<struct link_config, _num_flib_links> _link_config;

};

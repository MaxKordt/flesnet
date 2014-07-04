#include<iostream>
#include<iomanip>
#include<array>
#include<vector>
#include<string>
#include<sstream>
#include<memory>

#include<flib/flib_device.hpp>
#include<flib/rorcfs_device.hh>


namespace flib
{

    flib_device::flib_device(int device_nr)
    {
        // init device class
        m_device = std::unique_ptr<rorcfs_device>(new rorcfs_device());
        if (m_device->init(device_nr) == -1)
        { throw RorcfsException("Failed to initialize device"); }

        // bind to BAR1
        m_bar = std::unique_ptr<rorcfs_bar>(new rorcfs_bar(m_device.get(), 1));
        if ( m_bar->init() == -1 )
        { throw RorcfsException("BAR1 init failed"); }

        // register file access
        m_register_file = std::unique_ptr<register_file_bar>(new register_file_bar(m_bar.get(), 0));

        // enforce correct hw version
        if (!check_hw_ver() | !check_magic_number())
        { throw FlibException("Error in magic number or hardware version! \n Try to rescan PCIe bus and reload kernel module."); }

        // create link objects
        uint8_t num_links = get_num_hw_links();
        for (size_t i=0; i<num_links; i++)
        {
            link.push_back
            (
                std::unique_ptr<flib_link>
                (
                    new flib_link(i, m_device.get(), m_bar.get())
                )
            );
        }
    }



    bool
    flib_device::check_hw_ver()
    {
        uint16_t hw_ver = m_register_file->get_reg(0) >> 16; // RORC_REG_HARDWARE_INFO;
        bool match = false;

        // check if version of hardware is part of suported versions
        for(auto it = hw_ver_table.begin(); it != hw_ver_table.end() && match == false; ++it)
        {
            if(hw_ver == *it)
            { match = true; }
        }

        // check if version of hardware matches exactly version of header
        if (hw_ver != RORC_C_HARDWARE_VERSION)
        { match = false; }
        return match;
    }



    std::vector<flib_link*>
    flib_device::get_links()
    {
        std::vector<flib_link*> links;
        for(auto& l : link)
        { links.push_back(l.get()); }
        return links;
    }



    void
    flib_device::set_mc_time(uint32_t time)
    {
        // time: 31 bit wide, in units of 8 ns
        uint32_t reg = m_register_file->get_reg(RORC_REG_MC_CNT_CFG);
        reg = (reg & ~0x7FFFFFFF) | (time & 0x7FFFFFFF);
        m_register_file->set_reg(RORC_REG_MC_CNT_CFG, reg);
    }



    boost::posix_time::ptime
    flib_device::get_build_date()
    {
        time_t time =
            (static_cast<time_t>(m_register_file->get_reg(RORC_REG_BUILD_DATE_L)) |
            (static_cast<uint64_t>(m_register_file->get_reg(RORC_REG_BUILD_DATE_H))<<32));
        boost::posix_time::ptime t = boost::posix_time::from_time_t(time);
        return t;
    }



    std::string
    flib_device::print_build_info()
    {
        build_info build = get_build_info();
        std::stringstream ss;
        ss << "Build Date:     " << build.date << std::endl
           << "Repository Revision: " << std::hex
           << build.rev[4] << build.rev[3] << build.rev[2]
           << build.rev[1] << build.rev[0] << std::endl;

        if (build.clean)
        { ss << "Repository Status:   clean " << std::endl; }
        else
        {
            ss << "Repository Status:   NOT clean " << std::endl
               << "Hardware Version:    " << build.hw_ver;
        }
        return ss.str();
    }



    std::string
    flib_device::get_devinfo()
    {
        std::stringstream ss;
        ss << " Bus  " << static_cast<uint32_t>(m_device->getBus())
           << " Slot " << static_cast<uint32_t>(m_device->getSlot())
           << " Func " << static_cast<uint32_t>(m_device->getFunc());
        return ss.str();
    }
}

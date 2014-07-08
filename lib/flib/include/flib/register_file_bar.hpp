// Copyright 2014 Dirk Hutter, Dominic Eschweiler
// Interface class to access register files via bar memory transfers
#ifndef REGISTER_FILE_BAR_HPP
#define REGISTER_FILE_BAR_HPP

#include <sys/mman.h>

#include <flib/register_file.hpp>
#include <flib/pci_bar.hpp>


namespace flib
{

    class register_file_bar : public register_file
    {

    public:
        register_file_bar(pci_bar* bar, sys_bus_addr base_addr);

        int
        get_mem(sys_bus_addr addr, void *dest, size_t dwords) override;

        int
        set_mem(sys_bus_addr addr, const void *source, size_t dwords) override;

    protected:
        uint32_t*    m_bar; // 32 bit addressing
        size_t       m_bar_size;
        sys_bus_addr m_base_addr;
    };
}
#endif /** REGISTER_FILE_BAR_HPP */

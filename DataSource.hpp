/**
 * \file DataSource.hpp
 *
 * 2012, 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef DATASOURCE_HPP
#define DATASOURCE_HPP

/// Abstract FLES data source class.
class DataSource
{
public:
    virtual uint64_t wait_for_data(uint64_t min_mcNumber) = 0;

    virtual void update_ack_pointers(uint64_t new_acked_data, uint64_t new_acked_mc) = 0;

    virtual RingBufferView<>& data_buffer() = 0;

    virtual RingBufferView<MicrosliceDescriptor>& desc_buffer() = 0;
};
    

#endif /* DATASOURCE_HPP */

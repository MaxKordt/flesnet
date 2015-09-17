// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include "RingBufferView.hpp"
#include "MicrosliceDescriptor.hpp"

/// Abstract FLES data source class.
class RingBufferReadInterface
{
public:
    virtual ~RingBufferReadInterface() {}

    virtual void proceed() {}

    virtual uint64_t written_desc() = 0;
    virtual uint64_t written_data() = 0;

    virtual void update_ack_pointers(uint64_t new_acked_data,
                                     uint64_t new_acked_desc) = 0;

    virtual RingBufferView<volatile uint8_t>& data_buffer() = 0;

    virtual RingBufferView<volatile fles::MicrosliceDescriptor>&
    desc_buffer() = 0;

    virtual RingBufferView<volatile uint8_t>& data_send_buffer()
    {
        return data_buffer();
    }

    virtual RingBufferView<volatile fles::MicrosliceDescriptor>&
    desc_send_buffer()
    {
        return desc_buffer();
    }

    virtual void copy_to_data_send_buffer(std::size_t /* start */,
                                          std::size_t /* count */)
    {
    }

    virtual void copy_to_desc_send_buffer(std::size_t /* start */,
                                          std::size_t /* count */)
    {
    }
};

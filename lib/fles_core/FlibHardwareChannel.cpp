// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "FlibHardwareChannel.hpp"

FlibHardwareChannel::FlibHardwareChannel(std::size_t data_buffer_size_exp,
                                         std::size_t desc_buffer_size_exp,
                                         flib2::flib_link* flib_link)
    : _data_send_buffer(data_buffer_size_exp),
      _desc_send_buffer(desc_buffer_size_exp),
      _data_send_buffer_view(_data_send_buffer.ptr(), data_buffer_size_exp),
      _desc_send_buffer_view(_desc_send_buffer.ptr(), desc_buffer_size_exp),
      _flib_link(flib_link)
{
    constexpr std::size_t microslice_descriptor_size_exp = 5;
    std::size_t desc_buffer_bytes_exp =
        desc_buffer_size_exp + microslice_descriptor_size_exp;

#ifndef NO_DOUBLE_BUFFERING
    _flib_link->init_dma(flib2::create_only, data_buffer_size_exp,
                         desc_buffer_bytes_exp);

    uint8_t* data_buffer =
        reinterpret_cast<uint8_t*>(_flib_link->data_buffer());
    fles::MicrosliceDescriptor* desc_buffer =
        reinterpret_cast<fles::MicrosliceDescriptor*>(
            _flib_link->desc_buffer());

    _data_buffer_view = std::unique_ptr<RingBufferView<volatile uint8_t>>(
        new RingBufferView<volatile uint8_t>(data_buffer,
                                             data_buffer_size_exp));
    _desc_buffer_view =
        std::unique_ptr<RingBufferView<volatile fles::MicrosliceDescriptor>>(
            new RingBufferView<volatile fles::MicrosliceDescriptor>(
                desc_buffer, desc_buffer_size_exp));
#else
    _flib_link->init_dma(
        const_cast<void*>(static_cast<volatile void*>(_data_send_buffer.ptr())),
        data_buffer_size_exp,
        const_cast<void*>(static_cast<volatile void*>(_desc_send_buffer.ptr())),
        desc_buffer_bytes_exp);

    _data_buffer_view = std::unique_ptr<RingBufferView<volatile uint8_t>>(
        new RingBufferView<volatile uint8_t>(_data_send_buffer.ptr(),
                                             data_buffer_size_exp));
    _desc_buffer_view =
        std::unique_ptr<RingBufferView<volatile fles::MicrosliceDescriptor>>(
            new RingBufferView<volatile fles::MicrosliceDescriptor>(
                _desc_send_buffer.ptr(), desc_buffer_size_exp));
#endif

    _flib_link->set_start_idx(0);

    _flib_link->enable_cbmnet_packer(true);

    // assert(_flib_link->mc_index() == 0);
    // assert(_flib_link->pending_mc() == 0);
}

FlibHardwareChannel::~FlibHardwareChannel()
{
    _flib_link->rst_pending_mc();
    _flib_link->deinit_dma();
}

uint64_t FlibHardwareChannel::written_mc() { return _flib_link->mc_index(); }

uint64_t FlibHardwareChannel::written_data() { return _flib_link->channel()->get_data_offset(); }

void FlibHardwareChannel::update_ack_pointers(uint64_t new_acked_data,
                                              uint64_t new_acked_mc)
{
    _flib_link->channel()->set_sw_read_pointers(
        new_acked_data & _data_buffer_view->size_mask(),
        (new_acked_mc & _desc_buffer_view->size_mask()) *
            sizeof(fles::MicrosliceDescriptor));
}

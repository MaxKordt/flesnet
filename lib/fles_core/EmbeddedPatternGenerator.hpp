// Copyright 2012-2014 Jan de Cuveland <cmail@cuveland.de>
#pragma once

#include "RingBufferReadInterface.hpp"
#include "RingBuffer.hpp"
#include "RingBufferView.hpp"
#include "MicrosliceDescriptor.hpp"
#include "log.hpp"
#include <random>
#include <algorithm>

/// Simple embedded software pattern generator.
class EmbeddedPatternGenerator : public InputBufferReadInterface
{
public:
    /// The EmbeddedPatternGenerator constructor.
    EmbeddedPatternGenerator(std::size_t data_buffer_size_exp,
                             std::size_t desc_buffer_size_exp,
                             uint64_t input_index,
                             uint32_t typical_content_size)
        : data_buffer_(data_buffer_size_exp),
          desc_buffer_(desc_buffer_size_exp),
          data_buffer_view_(data_buffer_.ptr(), data_buffer_size_exp),
          desc_buffer_view_(desc_buffer_.ptr(), desc_buffer_size_exp),
          input_index_(input_index), generate_pattern_(false),
          typical_content_size_(typical_content_size), randomize_sizes_(false),
          random_distribution_(typical_content_size)
    {
    }

    EmbeddedPatternGenerator(const EmbeddedPatternGenerator&) = delete;
    void operator=(const EmbeddedPatternGenerator&) = delete;

    virtual RingBufferView<uint8_t>& data_buffer() override
    {
        return data_buffer_view_;
    }

    virtual RingBufferView<fles::MicrosliceDescriptor>& desc_buffer() override
    {
        return desc_buffer_view_;
    }

    void proceed() override;

    virtual DualIndex get_write_index() override { return write_index_; }

    virtual void set_read_index(DualIndex new_read_index) override
    {
        read_index_ = new_read_index;
    }

private:
    /// Input data buffer.
    RingBuffer<uint8_t> data_buffer_;

    /// Input descriptor buffer.
    RingBuffer<fles::MicrosliceDescriptor, true> desc_buffer_;

    RingBufferView<uint8_t> data_buffer_view_;
    RingBufferView<fles::MicrosliceDescriptor> desc_buffer_view_;

    /// This node's index in the list of input nodes
    uint64_t input_index_;

    bool generate_pattern_;
    uint32_t typical_content_size_;
    bool randomize_sizes_;

    /// A pseudo-random number generator.
    std::default_random_engine random_generator_;

    /// Distribution to use in determining data content sizes.
    std::poisson_distribution<unsigned int> random_distribution_;

    /// Number of acknowledged data bytes and microslices. Updated by input
    /// node.
    DualIndex read_index_{0, 0};

    /// FLIB-internal number of written microslices and data bytes.
    DualIndex write_index_{0, 0};
};

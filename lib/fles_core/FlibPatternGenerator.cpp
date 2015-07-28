// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "FlibPatternGenerator.hpp"

/// The thread main function.
void FlibPatternGenerator::produce_data()
{
    try {
        set_cpu(3);

        /// A pseudo-random number generator.
        std::default_random_engine random_generator;

        /// Distribution to use in determining data content sizes.
        std::poisson_distribution<unsigned int> random_distribution(
            _typical_content_size);

        uint64_t written_desc = 0;
        uint64_t written_data = 0;

        uint64_t last_written_desc = 0;
        uint64_t last_written_data = 0;

        uint64_t acked_desc = 0;
        uint64_t acked_data = 0;

        const uint64_t min_avail_desc = _desc_buffer.size() / 4;
        const uint64_t min_avail_data = _data_buffer.bytes() / 4;

        const uint64_t min_written_desc = _desc_buffer.size() / 4;
        const uint64_t min_written_data = _data_buffer.bytes() / 4;

        while (true) {
            // wait until significant space is available
            last_written_desc = written_desc;
            last_written_data = written_data;
            _written_desc = written_desc;
            _written_data = written_data;
            if (_is_stopped)
                return;
            while ((written_data - _acked_data + min_avail_data >
                    _data_buffer.bytes()) ||
                   (written_desc - _acked_desc + min_avail_desc >
                    _desc_buffer.size())) {
                if (_is_stopped)
                    return;
            }
            acked_desc = _acked_desc;
            acked_data = _acked_data;

            while (true) {
                unsigned int content_bytes = _typical_content_size;
                if (_randomize_sizes)
                    content_bytes = random_distribution(random_generator);
                content_bytes &=
                    ~0x7u; // round down to multiple of sizeof(uint64_t)

                // check for space in data and descriptor buffers
                if ((written_data - acked_data + content_bytes >
                     _data_buffer.bytes()) ||
                    (written_desc - acked_desc + 1 > _desc_buffer.size()))
                    break;

                const uint8_t hdr_id = static_cast<uint8_t>(
                    fles::HeaderFormatIdentifier::Standard);
                const uint8_t hdr_ver =
                    static_cast<uint8_t>(fles::HeaderFormatVersion::Standard);
                const uint16_t eq_id = 0xE001;
                const uint16_t flags = 0x0000;
                const uint8_t sys_id =
                    static_cast<uint8_t>(fles::SubsystemIdentifier::FLES);
                const uint8_t sys_ver = static_cast<uint8_t>(
                    _generate_pattern
                        ? fles::SubsystemFormatFLES::BasicRampPattern
                        : fles::SubsystemFormatFLES::Uninitialized);
                uint64_t idx = written_desc;
                uint32_t crc = 0x00000000;
                uint32_t size = content_bytes;
                uint64_t offset = written_data;

                // write to data buffer
                if (_generate_pattern) {
                    for (uint64_t i = 0; i < content_bytes;
                         i += sizeof(uint64_t)) {
                        uint64_t data_word = (_input_index << 48L) | i;
                        reinterpret_cast<volatile uint64_t&>(
                            _data_buffer.at(written_data)) = data_word;
                        written_data += sizeof(uint64_t);
                        crc ^= (data_word & 0xffffffff) ^ (data_word >> 32L);
                    }
                } else {
                    written_data += content_bytes;
                }

                // write to descriptor buffer
                const_cast<fles::MicrosliceDescriptor&>(
                    _desc_buffer.at(written_desc++)) =
                    fles::MicrosliceDescriptor({hdr_id, hdr_ver, eq_id, flags,
                                                sys_id, sys_ver, idx, crc, size,
                                                offset});

                if (written_desc >= last_written_desc + min_written_desc ||
                    written_data >= last_written_data + min_written_data) {
                    last_written_desc = written_desc;
                    last_written_data = written_data;
                    _written_desc = written_desc;
                    _written_data = written_data;
                }
            }
        }
    } catch (std::exception& e) {
        L_(error) << "exception in FlibPatternGenerator: " << e.what();
    }
}

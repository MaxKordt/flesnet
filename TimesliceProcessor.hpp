/**
 * \file TimesliceProcessor.hpp
 *
 * 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef TIMESLICEPROCESSOR_HPP
#define TIMESLICEPROCESSOR_HPP

class TimesliceProcessor : public ThreadContainer
{
public:
    explicit TimesliceProcessor(ComputeBuffer& cb, int index) : _cb(cb), _index(index) {}

    void operator()()
    {
        //set_cpu(2 + _index);

        try {           
            while (true) {
                TimesliceWorkItem wi;
                _cb._work_items.wait_and_pop(wi);
                process(wi);
                TimesliceCompletion c = {wi.ts_pos};
                _cb._completions.push(c);
            }
        }
        catch (concurrent_queue<TimesliceWorkItem>::Stopped) {
            out.trace() << "processor thread " << _index << " done";
        }
    }

    void process(TimesliceWorkItem wi) {
        bool check_pattern = par->check_pattern();

        uint64_t ts_pos = wi.ts_pos;
        uint64_t ts_num = _cb.get_desc(0, ts_pos).ts_num;
        uint64_t ts_size = par->timeslice_size() + par->overlap_size();
        if (out.beDebug()) {
            out.debug() << "processor thread " << _index << " working on timeslice " << ts_num;
        }

        const uint64_t data_size = (1 << par->cn_data_buffer_size_exp());
        const uint64_t data_mask = data_size - 1;

        for (size_t in = 0; in < _cb.size(); in++) {
            assert(_cb.get_desc(in, ts_pos).ts_num == ts_num);

            //uint64_t size = _cb.get_desc(in, ts_pos).size;
            uint64_t offset = _cb.get_desc(in, ts_pos).offset;

            //const RingBuffer<>& data = _cb.data(in);

            uint64_t mc_offset = ((MicrosliceDescriptor&) _cb.get_data(in, offset)).offset;
            for (size_t mc = 0; mc < ts_size; mc++) {
                MicrosliceDescriptor& mc_desc =
                    ((MicrosliceDescriptor&) _cb.get_data
                     (in, offset +  mc * sizeof(MicrosliceDescriptor)));
                uint64_t this_offset = mc_desc.offset - mc_offset + offset
                    + ts_size * sizeof(MicrosliceDescriptor);
                assert(mc_desc.idx == ts_num * par->timeslice_size() + mc);

                if (check_pattern) {
                    uint32_t crc = 0x00000000;
                    if ((this_offset & data_mask) + mc_desc.size > data_size) {
                        out.warn() << "timeslice " << ts_num << " crosses buffer boundary";
                    }
                    for (size_t pos = 0; pos < mc_desc.size; pos += sizeof(uint64_t)) {
                        uint64_t data_word = (uint64_t &) _cb.get_data(in, this_offset + pos);
                        crc ^= (data_word & 0xffffffff) ^ (data_word >> 32);
                        uint64_t expected = ((uint64_t) in << 48) | pos;
                        assert(data_word == expected);
                    }
                    assert(crc == mc_desc.crc);
                }
            }
        }
    }

    ComputeBuffer& _cb;
    int _index;
};


#endif /* TIMESLICEPROCESSOR_HPP */

/**
 * \file ComputeBuffer.hpp
 *
 * 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef COMPUTEBUFFER_HPP
#define COMPUTEBUFFER_HPP

/// Compute buffer and input node connection container class.
/** A ComputeBuffer object represents a timeslice buffer (filled by
    the input nodes) and a group of timeslice building connections to
    input nodes. */

class ComputeBuffer : public IBConnectionGroup<ComputeNodeConnection>
{
    size_t _red_lantern = 0;
    uint64_t _completely_written = 0;
    uint64_t _acked = 0;

    /// Buffer to store acknowledged status of timeslices.
    RingBuffer<uint64_t, true> _ack;

    std::unique_ptr<boost::interprocess::shared_memory_object> _data_shm;
    std::unique_ptr<boost::interprocess::shared_memory_object> _desc_shm;

    std::unique_ptr<boost::interprocess::mapped_region> _data_region;
    std::unique_ptr<boost::interprocess::mapped_region> _desc_region;

public:
    concurrent_queue<TimesliceWorkItem> _work_items;
    concurrent_queue<TimesliceCompletion> _completions;

    /// The ComputeBuffer default constructor.
    ComputeBuffer() :
        _ack(par->cn_desc_buffer_size_exp())
    {
        boost::interprocess::shared_memory_object::remove("flesnet_data");
        boost::interprocess::shared_memory_object::remove("flesnet_desc");

        std::unique_ptr<boost::interprocess::shared_memory_object>
            data_shm(new boost::interprocess::shared_memory_object
                     (boost::interprocess::create_only, "flesnet_data",
                      boost::interprocess::read_write));
        _data_shm = std::move(data_shm);

        std::unique_ptr<boost::interprocess::shared_memory_object>
            desc_shm(new boost::interprocess::shared_memory_object
                     (boost::interprocess::create_only, "flesnet_desc",
                      boost::interprocess::read_write));
        _desc_shm = std::move(desc_shm);

        std::size_t data_size = (1 << par->cn_data_buffer_size_exp()) * par->input_nodes().size();
        _data_shm->truncate(data_size);

        std::size_t desc_size = (1 << par->cn_desc_buffer_size_exp()) * par->input_nodes().size()
            * sizeof(TimesliceComponentDescriptor);
        _desc_shm->truncate(desc_size);

        std::unique_ptr<boost::interprocess::mapped_region>
            data_region(new boost::interprocess::mapped_region
                        (*_data_shm, boost::interprocess::read_write));
        _data_region = std::move(data_region);

        std::unique_ptr<boost::interprocess::mapped_region>
            desc_region(new boost::interprocess::mapped_region
                        (*_desc_shm, boost::interprocess::read_write));
        _desc_region = std::move(desc_region);

        VALGRIND_MAKE_MEM_DEFINED(_data_region->get_address(), _data_region->get_size());
        VALGRIND_MAKE_MEM_DEFINED(_desc_region->get_address(), _desc_region->get_size());
    }

    /// The ComputeBuffer destructor.
    ~ComputeBuffer() {
        boost::interprocess::shared_memory_object::remove("flesnet_data");
        boost::interprocess::shared_memory_object::remove("flesnet_desc");
    }

    ///
    uint8_t* get_data_ptr(uint_fast16_t index) {
        return static_cast<uint8_t*>(_data_region->get_address())
            + index * (1 << par->cn_data_buffer_size_exp());
    }

    ///
    TimesliceComponentDescriptor* get_desc_ptr(uint_fast16_t index) {
        return reinterpret_cast<TimesliceComponentDescriptor*>(_desc_region->get_address())
            + index * (1 << par->cn_desc_buffer_size_exp());
    }

    uint8_t& get_data(uint_fast16_t index, uint64_t offset) {
        offset &= (1 << par->cn_data_buffer_size_exp()) - 1;
        return get_data_ptr(index)[offset];
    }

    TimesliceComponentDescriptor& get_desc(uint_fast16_t index, uint64_t offset) {
        offset &= (1 << par->cn_desc_buffer_size_exp()) - 1;
        return get_desc_ptr(index)[offset];
    }

    /// Handle RDMA_CM_EVENT_CONNECT_REQUEST event.
    virtual void on_connect_request(struct rdma_cm_event* event) {
        if (!_pd)
            init_context(event->id->verbs);

        assert(event->param.conn.private_data_len >= sizeof(InputNodeInfo));
        InputNodeInfo remote_info =
            *reinterpret_cast<const InputNodeInfo*>(event->param.conn.private_data);

        uint_fast16_t index = remote_info.index;
        assert(index < _conn.size() && _conn.at(index) == nullptr);

        uint8_t* data_ptr = get_data_ptr(index);
        std::size_t data_bytes = 1 << par->cn_data_buffer_size_exp();

        TimesliceComponentDescriptor* desc_ptr = get_desc_ptr(index);
        std::size_t desc_bytes = (1 << par->cn_desc_buffer_size_exp())
            * sizeof(TimesliceComponentDescriptor);

        std::unique_ptr<ComputeNodeConnection> conn(new ComputeNodeConnection
                                                    (_ec, index, event->id, remote_info,
                                                     data_ptr, data_bytes, desc_ptr, desc_bytes));
        _conn.at(index) = std::move(conn);

        _conn.at(index)->on_connect_request(event, _pd, _cq);
    }

    /// Completion notification event dispatcher. Called by the event loop.
    virtual void on_completion(const struct ibv_wc& wc) {
        size_t in = wc.wr_id >> 8;
        assert(in < _conn.size());
        switch (wc.wr_id & 0xFF) {

        case ID_SEND_CN_ACK:
            if (out.beDebug()) {
                out.debug() << "[" << in << "] " << "COMPLETE SEND _send_cp_ack";
            }
            _conn[in]->on_complete_send();
            break;

        case ID_SEND_FINALIZE: {
            assert(_work_items.empty());
            assert(_completions.empty());
            _conn[in]->on_complete_send();
            _conn[in]->on_complete_send_finalize();
            _connections_done++;
            _all_done = (_connections_done == _conn.size());
            out.debug() << "SEND FINALIZE complete for id " << in << " all_done=" << _all_done;
            if (_all_done) {
                _work_items.stop();
                _completions.stop();
            }
        }
            break;

        case ID_RECEIVE_CN_WP: {
            _conn[in]->on_complete_recv();
            if (_connected == _conn.size() && in == _red_lantern) {
                auto new_red_lantern =
                    std::min_element(std::begin(_conn), std::end(_conn),
                                     [] (const std::unique_ptr<ComputeNodeConnection>& v1,
                                         const std::unique_ptr<ComputeNodeConnection>& v2)
                                     { return v1->cn_wp().desc < v2->cn_wp().desc; } );

                uint64_t new_completely_written = (*new_red_lantern)->cn_wp().desc;
                _red_lantern = std::distance(std::begin(_conn), new_red_lantern);

                for (uint64_t tpos = _completely_written; tpos < new_completely_written; tpos++) {
                    TimesliceWorkItem wi = {tpos};
                    _work_items.push(wi);
                }

                _completely_written = new_completely_written;
            }
        }
            break;

        default:
            throw InfinibandException("wc for unknown wr_id");
        }
    }

    virtual void handle_ts_completion() {
        //set_cpu(2);

        try {
            while (true) {
                TimesliceCompletion c;
                _completions.wait_and_pop(c);
                if (c.ts_pos == _acked) {
                    do
                        _acked++;
                    while (_ack.at(_acked) > c.ts_pos);
                    for (auto& c : _conn)
                        c->inc_ack_pointers(_acked);
                } else
                    _ack.at(c.ts_pos) = c.ts_pos;
            }
        }
        catch (concurrent_queue<TimesliceCompletion>::Stopped) {
            out.trace() << "handle_ts_completion thread done";
        }
    }

};


#endif /* COMPUTEBUFFER_HPP */

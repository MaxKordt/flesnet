/**
 * \file ComputeBuffer.cpp
 *
 * 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#include "ComputeBuffer.hpp"
#include "ChildProcessManager.hpp"
#include <boost/lexical_cast.hpp>
#include <random>
#include <csignal>

ComputeBuffer::ComputeBuffer(uint64_t compute_index,
                             uint32_t data_buffer_size_exp,
                             uint32_t desc_buffer_size_exp,
                             unsigned short service,
                             uint32_t num_input_nodes,
                             uint32_t timeslice_size,
                             uint32_t overlap_size,
                             uint32_t processor_instances,
                             const std::string processor_executable)
    : _compute_index(compute_index),
      _data_buffer_size_exp(data_buffer_size_exp),
      _desc_buffer_size_exp(desc_buffer_size_exp),
      _service(service),
      _num_input_nodes(num_input_nodes),
      _timeslice_size(timeslice_size),
      _overlap_size(overlap_size),
      _processor_instances(processor_instances),
      _processor_executable(processor_executable),
      _ack(desc_buffer_size_exp)
{
    std::random_device random_device;
    std::uniform_int_distribution<uint64_t> uint_distribution;
    uint64_t random_number = uint_distribution(random_device);
    _shared_memory_identifier = "flesnet_" + boost::lexical_cast
                                <std::string>(random_number);

    boost::interprocess::shared_memory_object::remove(
        (_shared_memory_identifier + "_data").c_str());
    boost::interprocess::shared_memory_object::remove(
        (_shared_memory_identifier + "_desc").c_str());

    std::unique_ptr<boost::interprocess::shared_memory_object> data_shm(
        new boost::interprocess::shared_memory_object(
            boost::interprocess::create_only,
            (_shared_memory_identifier + "_data").c_str(),
            boost::interprocess::read_write));
    _data_shm = std::move(data_shm);

    std::unique_ptr<boost::interprocess::shared_memory_object> desc_shm(
        new boost::interprocess::shared_memory_object(
            boost::interprocess::create_only,
            (_shared_memory_identifier + "_desc").c_str(),
            boost::interprocess::read_write));
    _desc_shm = std::move(desc_shm);

    std::size_t data_size = (1 << _data_buffer_size_exp) * _num_input_nodes;
    _data_shm->truncate(data_size);

    std::size_t desc_size = (1 << _desc_buffer_size_exp) * _num_input_nodes
                            * sizeof(TimesliceComponentDescriptor);
    _desc_shm->truncate(desc_size);

    std::unique_ptr<boost::interprocess::mapped_region> data_region(
        new boost::interprocess::mapped_region(
            *_data_shm, boost::interprocess::read_write));
    _data_region = std::move(data_region);

    std::unique_ptr<boost::interprocess::mapped_region> desc_region(
        new boost::interprocess::mapped_region(
            *_desc_shm, boost::interprocess::read_write));
    _desc_region = std::move(desc_region);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    VALGRIND_MAKE_MEM_DEFINED(_data_region->get_address(),
                              _data_region->get_size());
    VALGRIND_MAKE_MEM_DEFINED(_desc_region->get_address(),
                              _desc_region->get_size());
#pragma GCC diagnostic pop

    boost::interprocess::message_queue::remove(
        (_shared_memory_identifier + "_work_items").c_str());
    boost::interprocess::message_queue::remove(
        (_shared_memory_identifier + "_completions").c_str());

    std::unique_ptr<boost::interprocess::message_queue> work_items_mq(
        new boost::interprocess::message_queue(
            boost::interprocess::create_only,
            (_shared_memory_identifier + "_work_items").c_str(), 1000,
            sizeof(TimesliceWorkItem)));
    _work_items_mq = std::move(work_items_mq);

    std::unique_ptr<boost::interprocess::message_queue> completions_mq(
        new boost::interprocess::message_queue(
            boost::interprocess::create_only,
            (_shared_memory_identifier + "_completions").c_str(), 1000,
            sizeof(TimesliceCompletion)));
    _completions_mq = std::move(completions_mq);
}

ComputeBuffer::~ComputeBuffer()
{
    boost::interprocess::shared_memory_object::remove(
        (_shared_memory_identifier + "_data").c_str());
    boost::interprocess::shared_memory_object::remove(
        (_shared_memory_identifier + "_desc").c_str());
    boost::interprocess::message_queue::remove(
        (_shared_memory_identifier + "_work_items").c_str());
    boost::interprocess::message_queue::remove(
        (_shared_memory_identifier + "_completions").c_str());
}

/// The thread main function.
void ComputeBuffer::operator()()
{
    try
    {
        // set_cpu(0);

        assert(!_processor_executable.empty());
        for (uint_fast32_t i = 0; i < _processor_instances; ++i) {
            std::stringstream index;
            index << i;
            ChildProcess cp{};
            cp.owner = this;
            cp.path = _processor_executable;
            cp.arg = {_processor_executable, _shared_memory_identifier,
                      index.str()};
            ChildProcessManager::get().start_process(cp);
        }

        std::thread ts_compl(&ComputeBuffer::handle_ts_completion, this);

        accept(_service, _num_input_nodes);
        handle_cm_events(_num_input_nodes);
        std::thread t1(&ComputeBuffer::handle_cm_events, this, 0);
        completion_handler();
        ChildProcessManager::get().stop_processes(this);
        ts_compl.join();
        t1.join();

        summary();
    }
    catch (std::exception& e)
    {
        out.error() << "exception in ComputeBuffer: " << e.what();
    }
}

uint8_t* ComputeBuffer::get_data_ptr(uint_fast16_t index)
{
    return static_cast<uint8_t*>(_data_region->get_address())
           + index * (1 << _data_buffer_size_exp);
}

TimesliceComponentDescriptor* ComputeBuffer::get_desc_ptr(uint_fast16_t index)
{
    return reinterpret_cast
           <TimesliceComponentDescriptor*>(_desc_region->get_address())
           + index * (1 << _desc_buffer_size_exp);
}

uint8_t& ComputeBuffer::get_data(uint_fast16_t index, uint64_t offset)
{
    offset &= (1 << _data_buffer_size_exp) - 1;
    return get_data_ptr(index)[offset];
}

TimesliceComponentDescriptor& ComputeBuffer::get_desc(uint_fast16_t index,
                                                      uint64_t offset)
{
    offset &= (1 << _desc_buffer_size_exp) - 1;
    return get_desc_ptr(index)[offset];
}

void ComputeBuffer::on_connect_request(struct rdma_cm_event* event)
{
    if (!_pd)
        init_context(event->id->verbs);

    assert(event->param.conn.private_data_len >= sizeof(InputNodeInfo));
    InputNodeInfo remote_info =
        *reinterpret_cast<const InputNodeInfo*>(event->param.conn.private_data);

    uint_fast16_t index = remote_info.index;
    assert(index < _conn.size() && _conn.at(index) == nullptr);

    uint8_t* data_ptr = get_data_ptr(index);
    TimesliceComponentDescriptor* desc_ptr = get_desc_ptr(index);

    std::unique_ptr<ComputeNodeConnection> conn(new ComputeNodeConnection(
        _ec, index, _compute_index, event->id, remote_info, data_ptr,
        _data_buffer_size_exp, desc_ptr, _desc_buffer_size_exp));
    _conn.at(index) = std::move(conn);

    _conn.at(index)->on_connect_request(event, _pd, _cq);
}

/// Completion notification event dispatcher. Called by the event loop.
void ComputeBuffer::on_completion(const struct ibv_wc& wc)
{
    size_t in = wc.wr_id >> 8;
    assert(in < _conn.size());
    switch (wc.wr_id & 0xFF) {

    case ID_SEND_CN_ACK:
        if (out.beDebug()) {
            out.debug() << "[c" << _compute_index << "] "
                        << "[" << in << "] "
                        << "COMPLETE SEND _send_cp_ack";
        }
        _conn[in]->on_complete_send();
        break;

    case ID_SEND_FINALIZE: {
        if (_processor_executable.empty()) {
            assert(_work_items.empty());
            assert(_completions.empty());
        } else {
            assert(_work_items_mq->get_num_msg() == 0);
            assert(_completions_mq->get_num_msg() == 0);
        }
        _conn[in]->on_complete_send();
        _conn[in]->on_complete_send_finalize();
        ++_connections_done;
        _all_done = (_connections_done == _conn.size());
        out.debug() << "[c" << _compute_index << "] "
                    << "SEND FINALIZE complete for id " << in
                    << " all_done=" << _all_done;
        if (_all_done) {
            if (_processor_executable.empty()) {
                _work_items.stop();
                _completions.stop();
            } else {
                _completions_mq->send(nullptr, 0, 0);
            }
        }
    } break;

    case ID_RECEIVE_CN_WP: {
        _conn[in]->on_complete_recv();
        if (_connected == _conn.size() && in == _red_lantern) {
            auto new_red_lantern = std::min_element(
                std::begin(_conn), std::end(_conn),
                [](const std::unique_ptr<ComputeNodeConnection> & v1,
                   const std::unique_ptr<ComputeNodeConnection> & v2) {
                    return v1->cn_wp().desc < v2->cn_wp().desc;
                });

            uint64_t new_completely_written = (*new_red_lantern)->cn_wp().desc;
            _red_lantern = std::distance(std::begin(_conn), new_red_lantern);

            for (uint64_t tpos = _completely_written;
                 tpos < new_completely_written;
                 ++tpos) {
                TimesliceWorkItem wi = {
                    tpos,                  _timeslice_size,
                    _overlap_size,         static_cast<uint32_t>(_conn.size()),
                    _data_buffer_size_exp, _desc_buffer_size_exp};
                if (_processor_executable.empty()) {
                    _work_items.push(wi);
                } else {
                    _work_items_mq->send(&wi, sizeof(wi), 0);
                }
            }

            _completely_written = new_completely_written;
        }
    } break;

    default:
        throw InfinibandException("wc for unknown wr_id");
    }
}

/// The thread main function.
void ComputeBuffer::handle_ts_completion()
{
    // set_cpu(2);

    try
    {
        while (true) {
            TimesliceCompletion c;
            if (_processor_executable.empty()) {
                _completions.wait_and_pop(c);
            } else {
                std::size_t recvd_size;
                unsigned int priority;
                _completions_mq->receive(&c, sizeof(c), recvd_size, priority);
                if (recvd_size == 0)
                    return;
                assert(recvd_size == sizeof(c));
            }
            if (c.ts_pos == _acked) {
                do
                    ++_acked;
                while (_ack.at(_acked) > c.ts_pos);
                for (auto& connection : _conn)
                    connection->inc_ack_pointers(_acked);
            } else
                _ack.at(c.ts_pos) = c.ts_pos;
        }
    }
    catch (concurrent_queue<TimesliceCompletion>::Stopped)
    {
        out.trace() << "[c" << _compute_index << "] "
                    << "handle_ts_completion thread done";
    }
    catch (std::exception& e)
    {
        out.error() << "exception in ComputeBuffer::handle_ts_completion(): "
                    << e.what();
    }
}

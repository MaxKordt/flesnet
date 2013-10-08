#pragma once
/**
 * \file InputChannelConnection.hpp
 *
 * 2012, 2013, Jan de Cuveland <cmail@cuveland.de>
 */

/// Input node connection class.
/** An InputChannelConnection object represents the endpoint of a single
    timeslice building connection from an input node to a compute
    node. */

class InputChannelConnection : public IBConnection
{
public:
    /// The InputChannelConnection constructor.
    InputChannelConnection(struct rdma_event_channel* ec,
                        uint_fast16_t connection_index,
                        uint_fast16_t remote_connection_index,
                        unsigned int max_send_wr,
                        unsigned int max_pending_write_requests,
                        struct rdma_cm_id* id = nullptr) :
        IBConnection(ec, connection_index, remote_connection_index, id),
        _max_pending_write_requests(max_pending_write_requests)
    {
        assert(_max_pending_write_requests > 0);

        _qp_cap.max_send_wr = max_send_wr; // typical hca maximum: 16k
        _qp_cap.max_send_sge = 4; // max. two chunks each for descriptors and data

        _qp_cap.max_recv_wr = 1; // receive only single ComputeNodeBufferPosition struct
        _qp_cap.max_recv_sge = 1;

        _qp_cap.max_inline_data = sizeof(TimesliceComponentDescriptor);
    }

    InputChannelConnection(const InputChannelConnection&) = delete;
    void operator=(const InputChannelConnection&) = delete;

    /// Wait until enough space is available at target compute node.
    void wait_for_buffer_space(uint64_t data_size, uint64_t desc_size) {
        boost::mutex::scoped_lock lock(_cn_ack_mutex);
        if (out.beTrace()) {
            out.trace() << "[" << _index << "] "
                        << "SENDER data space (bytes) required="
                        << data_size << ", avail="
                        << _cn_ack.data + (1 << _remote_info.data_buffer_size_exp) - _cn_wp.data;
            out.trace() << "[" << _index << "] "
                        << "SENDER desc space (entries) required="
                        << desc_size << ", avail="
                        << _cn_ack.desc + (1 << _remote_info.desc_buffer_size_exp) - _cn_wp.desc;
        }
        while (_cn_ack.data - _cn_wp.data + (1 << _remote_info.data_buffer_size_exp) < data_size
               || _cn_ack.desc - _cn_wp.desc + (1 << _remote_info.data_buffer_size_exp)
               < desc_size) { // TODO: extend condition!
            {
                boost::mutex::scoped_lock lock2(_cn_wp_mutex);
                if (_our_turn) {
                    // send phony update to receive new pointers
                    out.debug() << "[i" << _remote_index << "] " << "[" << _index << "] "
                                << "SENDER send phony update";
                    _our_turn = false;
                    _send_cn_wp = _cn_wp;
                    post_send_cn_wp();
                }
            }
            _cn_ack_cond.wait(lock);
            if (out.beTrace()) {
                out.trace() << "[" << _index << "] "
                            << "SENDER (next try) space avail="
                            << _cn_ack.data - _cn_wp.data
                    + (1 << _remote_info.data_buffer_size_exp)
                            << " desc_avail=" << _cn_ack.desc - _cn_wp.desc
                    + (1 << _remote_info.desc_buffer_size_exp);
            }
        }
    }

    /// Send data and descriptors to compute node.
    void send_data(struct ibv_sge* sge, int num_sge, uint64_t timeslice,
                   uint64_t mc_length, uint64_t data_length, uint64_t skip) {
        int num_sge2 = 0;
        struct ibv_sge sge2[4];

        uint64_t cn_wp_data = _cn_wp.data;
        cn_wp_data += skip;

        uint64_t cn_data_buffer_mask = (1L << _remote_info.data_buffer_size_exp) - 1L;
        uint64_t cn_desc_buffer_mask = (1L << _remote_info.desc_buffer_size_exp) - 1L;
        uint64_t target_bytes_left =
            (1L << _remote_info.data_buffer_size_exp) - (cn_wp_data & cn_data_buffer_mask);

        // split sge list if necessary
        int num_sge_cut = 0;
        if (data_length + mc_length * sizeof(MicrosliceDescriptor) > target_bytes_left) {
            for (int i = 0; i < num_sge; ++i) {
                if (sge[i].length <= target_bytes_left) {
                    target_bytes_left -= sge[i].length;
                } else {
                    if (target_bytes_left) {
                        sge2[num_sge2].addr = sge[i].addr + target_bytes_left;
                        sge2[num_sge2].length = sge[i].length - target_bytes_left;
                        sge2[num_sge2++].lkey = sge[i].lkey;
                        sge[i].length = target_bytes_left;
                        target_bytes_left = 0;
                    } else {
                        sge2[num_sge2++] = sge[i];
                        ++num_sge_cut;
                    }
                }
            }
        }
        num_sge -= num_sge_cut;

        struct ibv_send_wr send_wr_ts, send_wr_tswrap, send_wr_tscdesc;
        memset(&send_wr_ts, 0, sizeof(send_wr_ts));
        send_wr_ts.wr_id = ID_WRITE_DATA;
        send_wr_ts.opcode = IBV_WR_RDMA_WRITE;
        send_wr_ts.sg_list = sge;
        send_wr_ts.num_sge = num_sge;
        send_wr_ts.wr.rdma.rkey = _remote_info.data.rkey;
        send_wr_ts.wr.rdma.remote_addr =
            static_cast<uintptr_t>(_remote_info.data.addr + (cn_wp_data & cn_data_buffer_mask));

        if (num_sge2) {
            memset(&send_wr_tswrap, 0, sizeof(send_wr_ts));
            send_wr_tswrap.wr_id = ID_WRITE_DATA_WRAP;
            send_wr_tswrap.opcode = IBV_WR_RDMA_WRITE;
            send_wr_tswrap.sg_list = sge2;
            send_wr_tswrap.num_sge = num_sge2;
            send_wr_tswrap.wr.rdma.rkey = _remote_info.data.rkey;
            send_wr_tswrap.wr.rdma.remote_addr = static_cast<uintptr_t>(_remote_info.data.addr);
            send_wr_ts.next = &send_wr_tswrap;
            send_wr_tswrap.next = &send_wr_tscdesc;
        } else {
            send_wr_ts.next = &send_wr_tscdesc;
        }

        // timeslice component descriptor
        TimesliceComponentDescriptor tscdesc;
        tscdesc.ts_num = timeslice;
        tscdesc.offset = cn_wp_data;
        tscdesc.size = data_length + mc_length * sizeof(MicrosliceDescriptor);
        struct ibv_sge sge3;
        sge3.addr = reinterpret_cast<uintptr_t>(&tscdesc);
        sge3.length = sizeof(tscdesc);
        sge3.lkey = 0;

        memset(&send_wr_tscdesc, 0, sizeof(send_wr_tscdesc));
        send_wr_tscdesc.wr_id = ID_WRITE_DESC | (timeslice << 24) | (_index << 8);
        send_wr_tscdesc.opcode = IBV_WR_RDMA_WRITE;
        send_wr_tscdesc.send_flags =
            IBV_SEND_INLINE | IBV_SEND_FENCE | IBV_SEND_SIGNALED;
        send_wr_tscdesc.sg_list = &sge3;
        send_wr_tscdesc.num_sge = 1;
        send_wr_tscdesc.wr.rdma.rkey = _remote_info.desc.rkey;
        send_wr_tscdesc.wr.rdma.remote_addr =
            static_cast<uintptr_t>(_remote_info.desc.addr
                                   + (_cn_wp.desc & cn_desc_buffer_mask)
                                   * sizeof(TimesliceComponentDescriptor));

        out.debug() << "[i" << _remote_index << "] " << "[" << _index << "] "
                    << "POST SEND data (timeslice " << timeslice << ")";

        // send everything
        while (_pending_write_requests >= _max_pending_write_requests) {
            //out.fatal() << "MAX REQUESTS! ###";
            boost::this_thread::yield(); // busy wait // TODO
        }
        ++_pending_write_requests;
        post_send(&send_wr_ts);
    }

    /// Increment target write pointers after data has been sent.
    void inc_write_pointers(uint64_t data_size, uint64_t desc_size) {
        boost::mutex::scoped_lock lock(_cn_wp_mutex);
        _cn_wp.data += data_size;
        _cn_wp.desc += desc_size;
        if (_our_turn) {
            _our_turn = false;
            _send_cn_wp = _cn_wp;
            post_send_cn_wp();
        }
    }

    // Get number of bytes to skip in advance (to avoid buffer wrap)
    uint64_t skip_required(uint64_t data_size) {
        uint64_t databuf_size = 1L << _remote_info.data_buffer_size_exp;
        uint64_t databuf_wp = _cn_wp.data & (databuf_size - 1L);
        if (databuf_wp + data_size <= databuf_size)
            return 0;
        else
            return databuf_size - databuf_wp;
    }

    ///
    void finalize() {
        boost::mutex::scoped_lock lock(_cn_wp_mutex);
        _finalize = true;
        if (_our_turn) {
            _our_turn = false;
            if (_cn_wp == _cn_ack)
                _send_cn_wp = CN_WP_FINAL;
            else
                _send_cn_wp = _cn_wp;
            post_send_cn_wp();
        }
    }

    void on_complete_write() {
        _pending_write_requests--;
    }

    /// Handle Infiniband receive completion notification.
    void on_complete_recv() {
        if (_receive_cn_ack == CN_WP_FINAL) {
            _done = true;
            return;
        }
        out.debug() << "[i" << _remote_index << "] " << "[" << _index << "] "
                    << "receive completion, new _cn_ack.data="
                    << _receive_cn_ack.data;
        {
            boost::mutex::scoped_lock lock(_cn_ack_mutex);
            _cn_ack = _receive_cn_ack;
            _cn_ack_cond.notify_one();
        }
        post_recv_cn_ack();
        {
            boost::mutex::scoped_lock lock(_cn_wp_mutex);
            if (_cn_wp != _send_cn_wp) {
                _send_cn_wp = _cn_wp;
                post_send_cn_wp();
            } else if (_finalize) {
                if (_cn_wp == _cn_ack)
                    _send_cn_wp = CN_WP_FINAL;
                post_send_cn_wp();
            } else {
                _our_turn = true;
            }
        }
    }

    virtual void setup(struct ibv_pd* pd) {
        // register memory regions
        _mr_recv = ibv_reg_mr(pd, &_receive_cn_ack,
                              sizeof(ComputeNodeBufferPosition),
                              IBV_ACCESS_LOCAL_WRITE);
        if (!_mr_recv)
            throw InfinibandException("registration of memory region failed");

        _mr_send = ibv_reg_mr(pd, &_send_cn_wp,
                              sizeof(ComputeNodeBufferPosition), 0);
        if (!_mr_send)
            throw InfinibandException("registration of memory region failed");

        // setup send and receive buffers
        recv_sge.addr = reinterpret_cast<uintptr_t>(&_receive_cn_ack);
        recv_sge.length = sizeof(ComputeNodeBufferPosition);
        recv_sge.lkey = _mr_recv->lkey;

        recv_wr.wr_id = ID_RECEIVE_CN_ACK | (_index << 8);
        recv_wr.sg_list = &recv_sge;
        recv_wr.num_sge = 1;

        send_sge.addr = reinterpret_cast<uintptr_t>(&_send_cn_wp);
        send_sge.length = sizeof(ComputeNodeBufferPosition);
        send_sge.lkey = _mr_send->lkey;

        send_wr.wr_id = ID_SEND_CN_WP | (_index << 8);
        send_wr.opcode = IBV_WR_SEND;
        send_wr.send_flags = IBV_SEND_SIGNALED;
        send_wr.sg_list = &send_sge;
        send_wr.num_sge = 1;

        // post initial receive request
        post_recv_cn_ack();
    }

    /// Connection handler function, called on successful connection.
    /**
       \param event RDMA connection manager event structure
    */
    virtual void on_established(struct rdma_cm_event* event) {
        assert(event->param.conn.private_data_len >= sizeof(ComputeNodeInfo));
        memcpy(&_remote_info, event->param.conn.private_data, sizeof(ComputeNodeInfo));

        IBConnection::on_established(event);
    }

    void dereg_mr() {
        if (_mr_recv) {
            ibv_dereg_mr(_mr_recv);
            _mr_recv = nullptr;
        }
            
        if (_mr_send) {
            ibv_dereg_mr(_mr_send);
            _mr_send = nullptr;
        }
    }

    virtual void on_rejected(struct rdma_cm_event* event) {
        dereg_mr();
        IBConnection::on_rejected(event);
    }

    virtual void on_disconnected(struct rdma_cm_event* event) {
        dereg_mr();
        IBConnection::on_disconnected(event);
    }
    
    virtual std::unique_ptr<std::vector<uint8_t> > get_private_data() {
        std::unique_ptr<std::vector<uint8_t> >
            private_data(new std::vector<uint8_t>(sizeof(InputNodeInfo)));

        InputNodeInfo* in_info = reinterpret_cast<InputNodeInfo*>(private_data->data());
        in_info->index = _remote_index;

        return private_data;
    }

private:
    /// Post a receive work request (WR) to the receive queue
    void post_recv_cn_ack() {
        if (out.beDebug()) {
            out.debug() << "[i" << _remote_index << "] " << "[" << _index << "] "
                        << "POST RECEIVE _receive_cn_ack";
        }
        post_recv(&recv_wr);
    }

    /// Post a send work request (WR) to the send queue
    void post_send_cn_wp() {
        if (out.beDebug()) {
            out.debug() << "[i" << _remote_index << "] " << "[" << _index << "] "
                        << "POST SEND _send_cp_wp (data=" << _send_cn_wp.data
                        << " desc=" << _send_cn_wp.desc << ")";
        }
        post_send(&send_wr);
    }

    /// Flag, true if it is the input nodes's turn to send a pointer update.
    bool _our_turn = true;

    bool _finalize = false;

    /// Access information for memory regions on remote end.
    ComputeNodeInfo _remote_info = {};

    /// Local copy of acknowledged-by-CN pointers
    ComputeNodeBufferPosition _cn_ack = {};

    /// Receive buffer for acknowledged-by-CN pointers
    ComputeNodeBufferPosition _receive_cn_ack = {};

    /// Infiniband memory region descriptor for acknowledged-by-CN pointers
    struct ibv_mr* _mr_recv = nullptr;

    /// Mutex protecting access to acknowledged-by-CN pointers
    boost::mutex _cn_ack_mutex;

    /// Condition variable for acknowledged-by-CN pointers    
    boost::condition_variable _cn_ack_cond;

    /// Local version of CN write pointers
    ComputeNodeBufferPosition _cn_wp = {};

    /// Send buffer for CN write pointers
    ComputeNodeBufferPosition _send_cn_wp = {};

    /// Infiniband memory region descriptor for CN write pointers
    struct ibv_mr* _mr_send = nullptr;

    /// Mutex protecting access to CN write pointers
    boost::mutex _cn_wp_mutex;
   
    /// InfiniBand receive work request
    struct ibv_recv_wr recv_wr = {};

    /// Scatter/gather list entry for receive work request
    struct ibv_sge recv_sge;

    /// Infiniband send work request
    struct ibv_send_wr send_wr = {};

    /// Scatter/gather list entry for send work request
    struct ibv_sge send_sge;

    std::atomic_uint _pending_write_requests{0};

    unsigned int _max_pending_write_requests = 0;
};

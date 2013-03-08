/**
 * \file InputNodeConnection.hpp
 *
 * 2012, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef INPUTNODECONNECTION_HPP
#define INPUTNODECONNECTION_HPP

#include <boost/thread.hpp>
#include "Infiniband.hpp"
#include "Parameters.hpp"
#include "Timeslice.hpp"
#include "global.hpp"


/// Input node connection class.
/** An InputNodeConnection object represents the endpoint of a single
    timeslice building connection from an input node to a compute
    node. */

class InputNodeConnection : public IBConnection
{
public:

    /// The InputNodeConnection constructor.
    InputNodeConnection(struct rdma_event_channel* ec, int index, struct rdma_cm_id* id = 0) :
        IBConnection(ec, index, id)
    {
        _qp_cap.max_send_wr = 20;
        _qp_cap.max_send_sge = 8;
        _qp_cap.max_recv_wr = 20;
        _qp_cap.max_recv_sge = 8;
        _qp_cap.max_inline_data = sizeof(TimesliceComponentDescriptor) * 10;

        memset(&_receive_cn_ack, 0, sizeof(ComputeNodeBufferPosition));
        memset(&_cn_ack, 0, sizeof(ComputeNodeBufferPosition));
        memset(&_cn_wp, 0, sizeof(ComputeNodeBufferPosition));
        memset(&_send_cn_wp, 0, sizeof(ComputeNodeBufferPosition));
    }

    /// Wait until enough space is available at target compute node.
    void waitForBufferSpace(uint64_t dataSize, uint64_t descSize) {
        boost::mutex::scoped_lock lock(_cn_ack_mutex);
        if (Log.beTrace()) {
            Log.trace() << "[" << _index << "] "
                        << "SENDER data space (words) required="
                        << dataSize << ", avail="
                        << _cn_ack.data + Par->cnDataBufferSize() - _cn_wp.data;
            Log.trace() << "[" << _index << "] "
                        << "SENDER desc space (words) required="
                        << descSize << ", avail="
                        << _cn_ack.desc + Par->cnDescBufferSize() - _cn_wp.desc;
        }
        while (_cn_ack.data - _cn_wp.data + Par->cnDataBufferSize() < dataSize
                || _cn_ack.desc - _cn_wp.desc + Par->cnDescBufferSize()
                < descSize) {
            {
                boost::mutex::scoped_lock lock2(_cn_wp_mutex);
                if (_ourTurn) {
                    // send phony update to receive new pointers
                    Log.info() << "[" << _index << "] "
                               << "SENDER send phony update";
                    _ourTurn = false;
                    _send_cn_wp = _cn_wp;
                    postSendCnWp();
                }
            }
            _cn_ack_cond.wait(lock);
            if (Log.beTrace()) {
                Log.trace() << "[" << _index << "] "
                            << "SENDER (next try) space avail="
                            << _cn_ack.data - _cn_wp.data
                    + Par->cnDataBufferSize()
                            << " desc_avail=" << _cn_ack.desc - _cn_wp.desc
                    + Par->cnDescBufferSize();
            }
        }
    }

    /// Send data and descriptors to compute node.
    void sendData(struct ibv_sge* sge, int num_sge, uint64_t timeslice,
                  uint64_t mc_length, uint64_t data_length) {
        int num_sge2 = 0;
        struct ibv_sge sge2[4];

        uint64_t target_words_left =
            Par->cnDataBufferSize() - _cn_wp.data % Par->cnDataBufferSize();

        // split sge list if necessary
        int num_sge_cut = 0;
        if (data_length + mc_length > target_words_left) {
            for (int i = 0; i < num_sge; i++) {
                if (sge[i].length <= (target_words_left * sizeof(uint64_t))) {
                    target_words_left -= sge[i].length / sizeof(uint64_t);
                } else {
                    if (target_words_left) {
                        sge2[num_sge2].addr =
                            sge[i].addr
                            + sizeof(uint64_t) * target_words_left;
                        sge2[num_sge2].length =
                            sge[i].length
                            - sizeof(uint64_t) * target_words_left;
                        sge2[num_sge2++].lkey = sge[i].lkey;
                        sge[i].length = sizeof(uint64_t) * target_words_left;
                        target_words_left = 0;
                    } else {
                        sge2[num_sge2++] = sge[i];
                        num_sge_cut++;
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
        send_wr_ts.wr.rdma.rkey = _serverInfo[0].rkey;
        send_wr_ts.wr.rdma.remote_addr =
            (uintptr_t)(_serverInfo[0].addr +
                        (_cn_wp.data % Par->cnDataBufferSize())
                        * sizeof(uint64_t));

        if (num_sge2) {
            memset(&send_wr_tswrap, 0, sizeof(send_wr_ts));
            send_wr_tswrap.wr_id = ID_WRITE_DATA_WRAP;
            send_wr_tswrap.opcode = IBV_WR_RDMA_WRITE;
            send_wr_tswrap.sg_list = sge2;
            send_wr_tswrap.num_sge = num_sge2;
            send_wr_tswrap.wr.rdma.rkey = _serverInfo[0].rkey;
            send_wr_tswrap.wr.rdma.remote_addr =
                (uintptr_t) _serverInfo[0].addr;
            send_wr_ts.next = &send_wr_tswrap;
            send_wr_tswrap.next = &send_wr_tscdesc;
        } else {
            send_wr_ts.next = &send_wr_tscdesc;
        }

        // timeslice component descriptor
        TimesliceComponentDescriptor tscdesc;
        tscdesc.tsNum = timeslice;
        tscdesc.offset = _cn_wp.data;
        tscdesc.size = data_length + mc_length;
        struct ibv_sge sge3;
        sge3.addr = (uintptr_t) &tscdesc;
        sge3.length = sizeof(tscdesc);
        sge3.lkey = 0;

        memset(&send_wr_tscdesc, 0, sizeof(send_wr_tscdesc));
        send_wr_tscdesc.wr_id = ID_WRITE_DESC | (timeslice << 8);
        send_wr_tscdesc.opcode = IBV_WR_RDMA_WRITE;
        send_wr_tscdesc.send_flags =
            IBV_SEND_INLINE | IBV_SEND_FENCE | IBV_SEND_SIGNALED;
        send_wr_tscdesc.sg_list = &sge3;
        send_wr_tscdesc.num_sge = 1;
        send_wr_tscdesc.wr.rdma.rkey = _serverInfo[1].rkey;
        send_wr_tscdesc.wr.rdma.remote_addr =
            (uintptr_t)(_serverInfo[1].addr
                        + (_cn_wp.desc % Par->cnDescBufferSize())
                        * sizeof(TimesliceComponentDescriptor));

        Log.debug() << "[" << _index << "] "
                    << "post_send (timeslice " << timeslice << ")";

        // send everything
        postSend(&send_wr_ts);

        _contentBytesSent += (data_length + mc_length) * sizeof(uint64_t)
            + sizeof(TimesliceComponentDescriptor);                             
    }

    /// Increment target write pointers after data has been sent.
    void incWritePointers(uint64_t dataSize, uint64_t descSize) {
        boost::mutex::scoped_lock lock(_cn_wp_mutex);
        _cn_wp.data += dataSize;
        _cn_wp.desc += descSize;
        if (_ourTurn) {
            _ourTurn = false;
            _send_cn_wp = _cn_wp;
            postSendCnWp();
        }
    }

    ///
    void finalize() {
        boost::mutex::scoped_lock lock(_cn_wp_mutex);
        _finalize = true;
        if (_ourTurn && _cn_wp.desc == _cn_ack.desc) {
            _ourTurn = false;
            _send_cn_wp.data = UINT64_MAX;
            _send_cn_wp.desc = UINT64_MAX;
            postSendCnWp();
        }
    }

    /// Handle Infiniband receive completion notification.
    void onCompleteRecv() {
        if (_receive_cn_ack.data == UINT64_MAX && _receive_cn_ack.desc == UINT64_MAX) {
            _done = true;
            return;
        }
        Log.debug()
                << "[" << _index << "] "
                << "receive completion, new _cn_ack.data="
                << _receive_cn_ack.data;
        {
            boost::mutex::scoped_lock lock(_cn_ack_mutex);
            _cn_ack = _receive_cn_ack;
            _cn_ack_cond.notify_one();
        }
        postRecvCnAck();
        {
            boost::mutex::scoped_lock lock(_cn_wp_mutex);
            if (_cn_wp.data != _send_cn_wp.data
                    || _cn_wp.desc != _send_cn_wp.desc) {
                _send_cn_wp = _cn_wp;
                postSendCnWp();
            } else if (_finalize && _cn_wp.desc == _cn_ack.desc) {
                _send_cn_wp.data = UINT64_MAX;
                _send_cn_wp.desc = UINT64_MAX;
                postSendCnWp();
            } else {
                _ourTurn = true;
            }
        }
    }

    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event for this connection.
    virtual void onAddrResolved(struct ibv_pd* pd, struct ibv_cq* cq) {
        IBConnection::onAddrResolved(pd, cq);
        
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
        recv_sge.addr = (uintptr_t) &_receive_cn_ack;
        recv_sge.length = sizeof(ComputeNodeBufferPosition);
        recv_sge.lkey = _mr_recv->lkey;
        memset(&recv_wr, 0, sizeof recv_wr);
        recv_wr.wr_id = ID_RECEIVE_CN_ACK | (_index << 8);
        recv_wr.sg_list = &recv_sge;
        recv_wr.num_sge = 1;

        send_sge.addr = (uintptr_t) &_send_cn_wp;
        send_sge.length = sizeof(ComputeNodeBufferPosition);
        send_sge.lkey = _mr_send->lkey;
        memset(&send_wr, 0, sizeof send_wr);
        send_wr.wr_id = ID_SEND_CN_WP;
        send_wr.opcode = IBV_WR_SEND;
        send_wr.sg_list = &send_sge;
        send_wr.num_sge = 1;

        // post initial receive request
        postRecvCnAck();
    }

    /// Connection handler function, called on successful connection.
    /**
       \param event RDMA connection manager event structure
       \return      Non-zero if an error occured
    */
    virtual void onConnection(struct rdma_cm_event* event) {
        IBConnection::onConnection(event);

        memcpy(&_serverInfo, event->param.conn.private_data, sizeof _serverInfo);
    }
    
    /// Handle RDMA_CM_EVENT_DISCONNECTED event for this connection.
    virtual void onDisconnect() {
        if (_mr_recv) {
            ibv_dereg_mr(_mr_recv);
            _mr_recv = 0;
        }
            
        if (_mr_send) {
            ibv_dereg_mr(_mr_send);
            _mr_send = 0;
        }
        
        IBConnection::onDisconnect();
    }

    /// Retrieve the number of bytes transmitted (without pointer updates).
    uint64_t contentBytesSent() const {
        return _contentBytesSent;
    }
    
private:

    /// Post a receive work request (WR) to the receive queue
    void postRecvCnAck() {
        if (Log.beDebug()) {
            Log.debug() << "[" << _index << "] "
                        << "POST RECEIVE _receive_cn_ack";
        }
        postRecv(&recv_wr);
    }

    /// Post a send work request (WR) to the send queue
    void postSendCnWp() {
        if (Log.beDebug()) {
            Log.debug() << "[" << _index << "] "
                        << "POST SEND _send_cp_wp (data=" << _send_cn_wp.data
                        << " desc=" << _send_cn_wp.desc << ")";
        }
        postSend(&send_wr);
    }

    /// Flag, true if it is the input nodes's turn to send a pointer update.
    bool _ourTurn = true;

    bool _finalize = false;

    /// Access information for memory regions on remote end.
    ServerInfo _serverInfo[2];    
    
    /// Local copy of acknowledged-by-CN pointers
    ComputeNodeBufferPosition _cn_ack;

    /// Receive buffer for acknowledged-by-CN pointers
    ComputeNodeBufferPosition _receive_cn_ack;

    /// Infiniband memory region descriptor for acknowledged-by-CN pointers
    struct ibv_mr* _mr_recv = nullptr;

    /// Mutex protecting access to acknowledged-by-CN pointers
    boost::mutex _cn_ack_mutex;

    /// Condition variable for acknowledged-by-CN pointers    
    boost::condition_variable_any _cn_ack_cond;

    /// Local version of CN write pointers
    ComputeNodeBufferPosition _cn_wp;

    /// Send buffer for CN write pointers
    ComputeNodeBufferPosition _send_cn_wp;

    /// Infiniband memory region descriptor for CN write pointers
    struct ibv_mr* _mr_send = nullptr;

    /// Mutex protecting access to CN write pointers
    boost::mutex _cn_wp_mutex;
   
    /// InfiniBand receive work request
    struct ibv_recv_wr recv_wr;

    /// Scatter/gather list entry for receive work request
    struct ibv_sge recv_sge;

    /// Infiniband send work request
    struct ibv_sge send_sge;

    /// Scatter/gather list entry for send work request
    struct ibv_send_wr send_wr;

    /// Total number of bytes transmitted (without pointer updates)
    uint64_t _contentBytesSent = 0;
};


#endif /* INPUTNODECONNECTION_HPP */

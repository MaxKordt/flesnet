// Copyright 2012-2014 Jan de Cuveland <cmail@cuveland.de>

#include "InputChannelConnection.hpp"
#include "TimesliceComponentDescriptor.hpp"
#include "MicrosliceDescriptor.hpp"
#include "InputNodeInfo.hpp"
#include "RequestIdentifier.hpp"
#include <log.hpp>
#include <cassert>
#include <cstring>

InputChannelConnection::InputChannelConnection(
    struct rdma_event_channel* ec, uint_fast16_t connection_index,
    uint_fast16_t remote_connection_index, unsigned int max_send_wr,
    unsigned int max_pending_write_requests, struct rdma_cm_id* id)
    : IBConnection(ec, connection_index, remote_connection_index, id),
      _max_pending_write_requests(max_pending_write_requests)
{
    assert(_max_pending_write_requests > 0);

    _qp_cap.max_send_wr = max_send_wr; // typical hca maximum: 16k
    _qp_cap.max_send_sge = 4; // max. two chunks each for descriptors and data

    _qp_cap.max_recv_wr =
        1; // receive only single ComputeNodeStatusMessage struct
    _qp_cap.max_recv_sge = 1;

    _qp_cap.max_inline_data = sizeof(fles::TimesliceComponentDescriptor);
}

bool InputChannelConnection::check_for_buffer_space(uint64_t data_size,
                                                    uint64_t desc_size)
{
    if (false) {
        L_(trace) << "[" << _index << "] "
                  << "SENDER data space (bytes) required=" << data_size
                  << ", avail="
                  << _cn_ack.data +
                         (UINT64_C(1) << _remote_info.data_buffer_size_exp) -
                         _cn_wp.data;
        L_(trace) << "[" << _index << "] "
                  << "SENDER desc space (entries) required=" << desc_size
                  << ", avail="
                  << _cn_ack.desc +
                         (UINT64_C(1) << _remote_info.desc_buffer_size_exp) -
                         _cn_wp.desc;
    }
    if (_cn_ack.data - _cn_wp.data +
                (UINT64_C(1) << _remote_info.data_buffer_size_exp) <
            data_size ||
        _cn_ack.desc - _cn_wp.desc +
                (UINT64_C(1) << _remote_info.desc_buffer_size_exp) <
            desc_size) { // TODO: extend condition!
        return false;
    } else {
        return true;
    }
}

void InputChannelConnection::send_data(struct ibv_sge* sge, int num_sge,
                                       uint64_t timeslice, uint64_t mc_length,
                                       uint64_t data_length, uint64_t skip)
{
    int num_sge2 = 0;
    struct ibv_sge sge2[4];

    uint64_t cn_wp_data = _cn_wp.data;
    cn_wp_data += skip;

    uint64_t cn_data_buffer_mask =
        (UINT64_C(1) << _remote_info.data_buffer_size_exp) - 1;
    uint64_t cn_desc_buffer_mask =
        (UINT64_C(1) << _remote_info.desc_buffer_size_exp) - 1;
    uint64_t target_bytes_left =
        (UINT64_C(1) << _remote_info.data_buffer_size_exp) -
        (cn_wp_data & cn_data_buffer_mask);

    // split sge list if necessary
    int num_sge_cut = 0;
    if (data_length + mc_length * sizeof(fles::MicrosliceDescriptor) >
        target_bytes_left) {
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
    send_wr_ts.wr.rdma.remote_addr = static_cast<uintptr_t>(
        _remote_info.data.addr + (cn_wp_data & cn_data_buffer_mask));

    if (num_sge2) {
        memset(&send_wr_tswrap, 0, sizeof(send_wr_ts));
        send_wr_tswrap.wr_id = ID_WRITE_DATA_WRAP;
        send_wr_tswrap.opcode = IBV_WR_RDMA_WRITE;
        send_wr_tswrap.sg_list = sge2;
        send_wr_tswrap.num_sge = num_sge2;
        send_wr_tswrap.wr.rdma.rkey = _remote_info.data.rkey;
        send_wr_tswrap.wr.rdma.remote_addr =
            static_cast<uintptr_t>(_remote_info.data.addr);
        send_wr_ts.next = &send_wr_tswrap;
        send_wr_tswrap.next = &send_wr_tscdesc;
    } else {
        send_wr_ts.next = &send_wr_tscdesc;
    }

    // timeslice component descriptor
    fles::TimesliceComponentDescriptor tscdesc;
    tscdesc.ts_num = timeslice;
    tscdesc.offset = cn_wp_data;
    tscdesc.size = data_length + mc_length * sizeof(fles::MicrosliceDescriptor);
    tscdesc.num_microslices = mc_length;
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
        static_cast<uintptr_t>(_remote_info.desc.addr +
                               (_cn_wp.desc & cn_desc_buffer_mask) *
                                   sizeof(fles::TimesliceComponentDescriptor));

    if (false) {
        L_(trace) << "[i" << _remote_index << "] "
                  << "[" << _index << "] "
                  << "POST SEND data (timeslice " << timeslice << ")";
    }

    // send everything
    assert(_pending_write_requests < _max_pending_write_requests);
    ++_pending_write_requests;
    post_send(&send_wr_ts);
}

bool InputChannelConnection::write_request_available()
{
    return (_pending_write_requests < _max_pending_write_requests);
}

void InputChannelConnection::inc_write_pointers(uint64_t data_size,
                                                uint64_t desc_size)
{
    _cn_wp.data += data_size;
    _cn_wp.desc += desc_size;
}

bool InputChannelConnection::try_sync_buffer_positions()
{
    if (_our_turn) {
        _our_turn = false;
        _send_status_message.wp = _cn_wp;
        post_send_status_message();
        return true;
    } else {
        return false;
    }
}

uint64_t InputChannelConnection::skip_required(uint64_t data_size)
{
    uint64_t databuf_size = UINT64_C(1) << _remote_info.data_buffer_size_exp;
    uint64_t databuf_wp = _cn_wp.data & (databuf_size - 1);
    if (databuf_wp + data_size <= databuf_size)
        return 0;
    else
        return databuf_size - databuf_wp;
}

void InputChannelConnection::finalize(bool abort)
{
    _finalize = true;
    _abort = abort;
    if (_our_turn) {
        _our_turn = false;
        if (_cn_wp == _cn_ack || _abort) {
            _send_status_message.final = true;
            _send_status_message.abort = _abort;
        } else {
            _send_status_message.wp = _cn_wp;
        }
        post_send_status_message();
    }
}

void InputChannelConnection::on_complete_write() { _pending_write_requests--; }

void InputChannelConnection::on_complete_recv()
{
    if (_recv_status_message.final) {
        _done = true;
        return;
    }
    if (false) {
        L_(trace) << "[i" << _remote_index << "] "
                  << "[" << _index << "] "
                  << "receive completion, new _cn_ack.data="
                  << _recv_status_message.ack.data;
    }
    _cn_ack = _recv_status_message.ack;
    post_recv_status_message();
    {
        if (_cn_wp == _send_status_message.wp && _finalize) {
            if (_cn_wp == _cn_ack || _abort)
                _send_status_message.final = true;
            post_send_status_message();
        } else {
            _our_turn = true;
        }
    }
}

void InputChannelConnection::setup(struct ibv_pd* pd)
{
    // register memory regions
    _mr_recv =
        ibv_reg_mr(pd, &_recv_status_message, sizeof(ComputeNodeStatusMessage),
                   IBV_ACCESS_LOCAL_WRITE);
    if (!_mr_recv)
        throw InfinibandException("registration of memory region failed");

    _mr_send = ibv_reg_mr(pd, &_send_status_message,
                          sizeof(InputChannelStatusMessage), 0);
    if (!_mr_send)
        throw InfinibandException("registration of memory region failed");

    // setup send and receive buffers
    recv_sge.addr = reinterpret_cast<uintptr_t>(&_recv_status_message);
    recv_sge.length = sizeof(ComputeNodeStatusMessage);
    recv_sge.lkey = _mr_recv->lkey;

    recv_wr.wr_id = ID_RECEIVE_STATUS | (_index << 8);
    recv_wr.sg_list = &recv_sge;
    recv_wr.num_sge = 1;

    send_sge.addr = reinterpret_cast<uintptr_t>(&_send_status_message);
    send_sge.length = sizeof(InputChannelStatusMessage);
    send_sge.lkey = _mr_send->lkey;

    send_wr.wr_id = ID_SEND_STATUS | (_index << 8);
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;
    send_wr.sg_list = &send_sge;
    send_wr.num_sge = 1;

    // post initial receive request
    post_recv_status_message();
}

/// Connection handler function, called on successful connection.
/**
   \param event RDMA connection manager event structure
*/
void InputChannelConnection::on_established(struct rdma_cm_event* event)
{
    assert(event->param.conn.private_data_len >= sizeof(ComputeNodeInfo));
    memcpy(&_remote_info, event->param.conn.private_data,
           sizeof(ComputeNodeInfo));

    IBConnection::on_established(event);
}

void InputChannelConnection::dereg_mr()
{
    if (_mr_recv) {
        ibv_dereg_mr(_mr_recv);
        _mr_recv = nullptr;
    }

    if (_mr_send) {
        ibv_dereg_mr(_mr_send);
        _mr_send = nullptr;
    }
}

void InputChannelConnection::on_rejected(struct rdma_cm_event* event)
{
    dereg_mr();
    IBConnection::on_rejected(event);
}

void InputChannelConnection::on_disconnected(struct rdma_cm_event* event)
{
    dereg_mr();
    IBConnection::on_disconnected(event);
}

std::unique_ptr<std::vector<uint8_t>> InputChannelConnection::get_private_data()
{
    std::unique_ptr<std::vector<uint8_t>> private_data(
        new std::vector<uint8_t>(sizeof(InputNodeInfo)));

    InputNodeInfo* in_info =
        reinterpret_cast<InputNodeInfo*>(private_data->data());
    in_info->index = _remote_index;

    return private_data;
}

void InputChannelConnection::post_recv_status_message()
{
    if (false) {
        L_(trace) << "[i" << _remote_index << "] "
                  << "[" << _index << "] "
                  << "POST RECEIVE status message";
    }
    post_recv(&recv_wr);
}

void InputChannelConnection::post_send_status_message()
{
    if (false) {
        L_(trace) << "[i" << _remote_index << "] "
                  << "[" << _index << "] "
                  << "POST SEND status message (wp.data="
                  << _send_status_message.wp.data
                  << " wp.desc=" << _send_status_message.wp.desc << ")";
    }
    post_send(&send_wr);
}

/**
 * \file Infiniband.hpp
 *
 * 2012, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef INFINIBAND_HPP
#define INFINIBAND_HPP

#include <vector>
#include "log.hpp"


/// InfiniBand connection base class.
/** An IBConnection object represents the endpoint of a single
    InfiniBand connection handled by an rdma connection manager. */

class IBConnection
{
public:

    /// The IBConnection constructor. Creates a connection manager ID.
    IBConnection(struct rdma_event_channel* ec, int index) : _index(index) {
        int err = rdma_create_id(ec, &_cmId, this, RDMA_PS_TCP);
        if (err)
            throw ApplicationException("rdma_create_id failed");
    };

    /// Retrieve the InfiniBand queue pair associated with the connection.
    struct ibv_qp* qp() const {
        return _cmId->qp;
    };

    /// Initiate a connection request to target hostname and service.
    /**
       \param hostname The target hostname
       \param service  The target service or port number
    */
    void initiateConnect(const std::string& hostname,
                         const std::string& service) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res;

        int err = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res);
        if (err)
            throw ApplicationException("getaddrinfo failed");

        Log.debug() << "[" << _index << "] "
                    << "resolution of server address and route";

        for (struct addrinfo* t = res; t; t = t->ai_next) {
            err = rdma_resolve_addr(_cmId, NULL, t->ai_addr,
                                    RESOLVE_TIMEOUT_MS);
            if (!err)
                break;
        }
        if (err)
            throw ApplicationException("rdma_resolve_addr failed");

        freeaddrinfo(res);
    }

    /// Connection handler function, called on successful connection.
    /**
       \param event RDMA connection manager event structure
       \return      Non-zero if an error occured
    */
    int onConnection(struct rdma_cm_event* event) {
        memcpy(&_serverInfo, event->param.conn.private_data,
               sizeof _serverInfo);
        
        Log.debug() << "[" << _index << "] "
                    << "connection established";
        
        return 0;
    }
    
    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event for this connection.
    virtual void onAddrResolved(struct ibv_pd* pd) { };

protected:

    /// Access information for a remote memory region.
    typedef struct {
        uint64_t addr; ///< Target memory address
        uint32_t rkey; ///< Target remote access key
    } ServerInfo;

    /// Access information for memory regions on remote end.
    ServerInfo _serverInfo[2];

    /// Index of this connection in a group of connections.
    int _index;

    /// Post InfiniBand SEND work request.
    void postSend(struct ibv_send_wr *wr) {
        struct ibv_send_wr* bad_send_wr;
        
        if (ibv_post_send(qp(), wr, &bad_send_wr))
            throw ApplicationException("ibv_post_send failed");
    }

private:

    /// RDMA connection manager ID.
    struct rdma_cm_id* _cmId;
};


/// InfiniBand connection group base class.
/** An IBConnectionGroup object represents a group of InfiniBand
    connections that use the same completion queue. */

template <typename CONNECTION>
class IBConnectionGroup
{
public:

    /// The IBConnectionGroup default constructor.
    IBConnectionGroup() :
        _pd(0), _connected(0), _ec(0),  _context(0), _compChannel(0), _cq(0) {
        _ec = rdma_create_event_channel();
        if (!_ec)
            throw ApplicationException("rdma_create_event_channel failed");
    };

    /// The IBConnectionGroup default destructor.
    ~IBConnectionGroup() {
        rdma_destroy_event_channel(_ec);
    };
    
    /// Initiate connection requests to list of target hostnames.
    /**
       \param hostnames The list of target hostnames
       \param services  The list of target services or port numbers
    */
    void initiateConnect(const std::vector<std::string>& hostnames,
                         const std::vector<std::string>& services) {
        for (unsigned int i = 0; i < hostnames.size(); i++) {
            CONNECTION* connection = new CONNECTION(_ec, i);
            _conn.push_back(connection);
            connection->initiateConnect(hostnames[i], services[i]);
        }
    };

    /// The connection manager event loop.
    void handleCmEvents() {
        int err;
        struct rdma_cm_event* event;
        
        while ((err = rdma_get_cm_event(_ec, &event)) == 0) {
            int err = onCmEvent(event);
            rdma_ack_cm_event(event);
            if (err || _conn.size() == _connected)
                break;
        };
        if (err)
            throw ApplicationException("rdma_get_cm_event failed");
        
        Log.info() << "number of connections: " << _connected;
    };

    /// The InfiniBand completion notification event loop.
    void completionHandler() {
        const int ne_max = 10;

        struct ibv_cq* ev_cq;
        void* ev_ctx;
        struct ibv_wc wc[ne_max];
        int ne;

        while (true) {
            if (ibv_get_cq_event(_compChannel, &ev_cq, &ev_ctx))
                throw ApplicationException("ibv_get_cq_event failed");

            ibv_ack_cq_events(ev_cq, 1);

            if (ev_cq != _cq)
                throw ApplicationException("CQ event for unknown CQ");

            if (ibv_req_notify_cq(_cq, 0))
                throw ApplicationException("ibv_req_notify_cq failed");

            while ((ne = ibv_poll_cq(_cq, ne_max, wc))) {
                if (ne < 0)
                    throw ApplicationException("ibv_poll_cq failed");

                for (int i = 0; i < ne; i++) {
                    if (wc[i].status != IBV_WC_SUCCESS) {
                        std::ostringstream s;
                        s << ibv_wc_status_str(wc[i].status)
                          << " for wr_id " << (int) wc[i].wr_id;
                        throw ApplicationException(s.str());
                    }

                    onCompletion(wc[i]);
                }
            }
        }
    }

protected:

    /// InfiniBand protection domain
    struct ibv_pd* _pd;

    /// Vector of associated connection objects
    std::vector<CONNECTION*> _conn;

private:

    /// Number of established connections
    unsigned int _connected;

    /// RDMA event channel
    struct rdma_event_channel* _ec;

    /// InfiniBand verbs context
    struct ibv_context* _context;

    /// InfiniBand completion channel
    struct ibv_comp_channel* _compChannel;

    /// InfiniBand completion queue
    struct ibv_cq* _cq;

    /// Connection manager event dispatcher. Called by the CM event loop.
    int onCmEvent(struct rdma_cm_event* event) {
        switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            return onAddrResolved(event->id);
        case RDMA_CM_EVENT_ADDR_ERROR:
            throw ApplicationException("rdma_resolve_addr failed");
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            return onRouteResolved(event->id);
        case RDMA_CM_EVENT_ROUTE_ERROR:
            throw ApplicationException("rdma_resolve_route failed");
        case RDMA_CM_EVENT_CONNECT_ERROR:
            throw ApplicationException("could not establish connection");
        case RDMA_CM_EVENT_UNREACHABLE:
            throw ApplicationException("remote server is not reachable");
        case RDMA_CM_EVENT_REJECTED:
            throw ApplicationException("request rejected by remote endpoint");
        case RDMA_CM_EVENT_ESTABLISHED:
            return onConnection(event);
        case RDMA_CM_EVENT_DISCONNECTED:
            throw ApplicationException("connection has been disconnected");
        case RDMA_CM_EVENT_CONNECT_REQUEST:
        case RDMA_CM_EVENT_CONNECT_RESPONSE:
        case RDMA_CM_EVENT_DEVICE_REMOVAL:
        case RDMA_CM_EVENT_MULTICAST_JOIN:
        case RDMA_CM_EVENT_MULTICAST_ERROR:
        case RDMA_CM_EVENT_ADDR_CHANGE:
        case RDMA_CM_EVENT_TIMEWAIT_EXIT:
        default:
            throw ApplicationException("unknown cm event");
        }
    }

    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event.
    int onAddrResolved(struct rdma_cm_id* id) {
        Log.debug() << "address resolved";

        if (!_pd)
            initContext(id->verbs);

        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof qp_attr);
        qp_attr.cap.max_send_wr = 20;
        qp_attr.cap.max_send_sge = 8;
        qp_attr.cap.max_recv_wr = 20;
        qp_attr.cap.max_recv_sge = 8;
        qp_attr.cap.max_inline_data = sizeof(tscdesc_t) * 10;
        qp_attr.send_cq = _cq;
        qp_attr.recv_cq = _cq;
        qp_attr.qp_type = IBV_QPT_RC;
        int err = rdma_create_qp(id, _pd, &qp_attr);
        if (err)
            throw ApplicationException("creation of QP failed");

        CONNECTION* conn = (CONNECTION*) id->context;
        conn->onAddrResolved(_pd);

        err = rdma_resolve_route(id, RESOLVE_TIMEOUT_MS);
        if (err)
            throw ApplicationException("rdma_resolve_route failed");

        return 0;
    }

    /// Handle RDMA_CM_EVENT_ROUTE_RESOLVED event.
    int onRouteResolved(struct rdma_cm_id* id) {
        Log.debug() << "route resolved";

        struct rdma_conn_param conn_param;
        memset(&conn_param, 0, sizeof conn_param);
        conn_param.initiator_depth = 1;
        conn_param.retry_count = 7;
        int err = rdma_connect(id, &conn_param);
        if (err)
            throw ApplicationException("rdma_connect failed");

        return 0;
    }

    /// Handle RDMA_CM_EVENT_ESTABLISHED event.
    int onConnection(struct rdma_cm_event* event) {
        CONNECTION* conn = (CONNECTION*) event->id->context;

        conn->onConnection(event);
        _connected++;

        return 0;
    }

    /// Initialize the InfiniBand verbs context.
    void initContext(struct ibv_context* context) {
        _context = context;

        Log.debug() << "create verbs objects";

        _pd = ibv_alloc_pd(context);
        if (!_pd)
            throw ApplicationException("ibv_alloc_pd failed");

        _compChannel = ibv_create_comp_channel(context);
        if (!_compChannel)
            throw ApplicationException("ibv_create_comp_channel failed");

        _cq = ibv_create_cq(context, 40, NULL, _compChannel, 0);
        if (!_cq)
            throw ApplicationException("ibv_create_cq failed");

        if (ibv_req_notify_cq(_cq, 0))
            throw ApplicationException("ibv_req_notify_cq failed");
    }

    /// Completion notification event dispatcher. Called by the event loop.
    virtual void onCompletion(const struct ibv_wc& wc) { };
};


#endif /* INFINIBAND_HPP */

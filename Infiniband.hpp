/**
 * \file Infiniband.hpp
 *
 * 2012, Jan de Cuveland <cmail@cuveland.de>
 */

#ifndef INFINIBAND_HPP
#define INFINIBAND_HPP

#include <vector>
#include <cstring> // Required for memset().
#include <netdb.h>
#include <arpa/inet.h>
#include <infiniband/arch.h>
#include <rdma/rdma_cma.h>
#include "global.hpp"


/// InfiniBand exception class.
/** An InfinbandException object signals an error that occured in the
    InfiniBand communication functions. */

class InfinibandException : public std::runtime_error {
public:
    /// The InfinibandException default constructor.
    explicit InfinibandException(const std::string& what_arg = "")
        : std::runtime_error(what_arg) { }
};


/// InfiniBand connection base class.
/** An IBConnection object represents the endpoint of a single
    InfiniBand connection handled by an rdma connection manager. */

class IBConnection
{
public:

    /// The IBConnection constructor. Creates a connection manager ID.
    IBConnection(struct rdma_event_channel* ec, int index) :
        _index(index), _done(false)
    {
        int err = rdma_create_id(ec, &_cmId, this, RDMA_PS_TCP);
        if (err)
            throw InfinibandException("rdma_create_id failed");

        _qp_cap.max_send_wr = 16;
        _qp_cap.max_recv_wr = 16;
        _qp_cap.max_send_sge = 8;
        _qp_cap.max_recv_sge = 8;
        _qp_cap.max_inline_data = 0;
    };

    /// The IBConnection destructor.
    ~IBConnection() {
        int err = rdma_destroy_id(_cmId);
        if (err)
            throw InfinibandException("rdma_destroy_id() failed");
    }
    
    /// Retrieve the InfiniBand queue pair associated with the connection.
    struct ibv_qp* qp() const {
        return _cmId->qp;
    };

    /// Initiate a connection request to target hostname and service.
    /**
       \param hostname The target hostname
       \param service  The target service or port number
    */
    void connect(const std::string& hostname,
                         const std::string& service) {
        struct addrinfo hints;
        memset(&hints, 0, sizeof(struct addrinfo));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res;

        int err = getaddrinfo(hostname.c_str(), service.c_str(), &hints, &res);
        if (err)
            throw InfinibandException("getaddrinfo failed");

        Log.debug() << "[" << _index << "] "
                    << "resolution of server address and route";

        for (struct addrinfo* t = res; t; t = t->ai_next) {
            err = rdma_resolve_addr(_cmId, NULL, t->ai_addr,
                                    RESOLVE_TIMEOUT_MS);
            if (!err)
                break;
        }
        if (err)
            throw InfinibandException("rdma_resolve_addr failed");

        freeaddrinfo(res);
    }

    void disconnect() {
        Log.debug() << "[" << _index << "] "
                    << "disconnect";
        int err = rdma_disconnect(_cmId);
        if (err)
            throw InfinibandException("rdma_disconnect() failed");
    }
    
    /// Connection handler function, called on successful connection.
    /**
       \param event RDMA connection manager event structure
       \return      Non-zero if an error occured
    */
    int onConnection(struct rdma_cm_event* event) {
        memcpy(&_serverInfo, event->param.conn.private_data,
               sizeof _serverInfo);
        
        Log.debug() << "[" << _index << "] " << "connection established";
        
        return 0;
    }
    
    /// Handle RDMA_CM_EVENT_DISCONNECTED event for this connection.
    virtual int onDisconnect() {
        Log.debug() << "[" << _index << "] " << "connection disconnected";

        rdma_destroy_qp(_cmId);
            
        return 0;
    }
    
    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event for this connection.
    virtual void onAddrResolved(struct ibv_pd* pd, struct ibv_cq* cq) {
        Log.debug() << "address resolved";

        struct ibv_qp_init_attr qp_attr;
        memset(&qp_attr, 0, sizeof qp_attr);
        qp_attr.cap = _qp_cap;
        qp_attr.send_cq = cq;
        qp_attr.recv_cq = cq;
        qp_attr.qp_type = IBV_QPT_RC;
        int err = rdma_create_qp(_cmId, pd, &qp_attr);
        if (err)
            throw InfinibandException("creation of QP failed");
        
        err = rdma_resolve_route(_cmId, RESOLVE_TIMEOUT_MS);
        if (err)
            throw InfinibandException("rdma_resolve_route failed");
    };
    
    /// Handle RDMA_CM_EVENT_ROUTE_RESOLVED event for this connection.
    virtual void onRouteResolved() {
        Log.debug() << "route resolved";

        struct rdma_conn_param conn_param;
        memset(&conn_param, 0, sizeof conn_param);
        conn_param.initiator_depth = 1;
        conn_param.retry_count = 7;
        int err = rdma_connect(_cmId, &conn_param);
        if (err)
            throw InfinibandException("rdma_connect failed");
    };

    /// Retrieve index of this connection in the connection group.
    int index() const { return _index; };

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

    /// Flag indicating connection finished state.
    bool _done;

    /// The queue pair capabilities.
    struct ibv_qp_cap _qp_cap;
    
    /// Post an InfiniBand SEND work request (WR) to the send queue
    void postSend(struct ibv_send_wr *wr) {
        struct ibv_send_wr* bad_send_wr;
        
        if (ibv_post_send(qp(), wr, &bad_send_wr))
            throw InfinibandException("ibv_post_send failed");
    }

    /// Post an InfiniBand RECV work request (WR) to the receive queue.
    void postRecv(struct ibv_recv_wr *wr) {
        struct ibv_recv_wr* bad_recv_wr;
        
        if (ibv_post_recv(qp(), wr, &bad_recv_wr))
            throw InfinibandException("ibv_post_recv failed");
    }

private:

    /// RDMA connection manager ID.
    struct rdma_cm_id* _cmId;

    /// Low-level communication parameters
    enum {
        RESOLVE_TIMEOUT_MS = 5000 ///< Resolve timeout in milliseconds.
    };

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
        _pd(0), _allDone(false), _connected(0), _ec(0),  _context(0),
        _compChannel(0), _cq(0) {
        _ec = rdma_create_event_channel();
        if (!_ec)
            throw InfinibandException("rdma_create_event_channel failed");
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
    void connect(const std::vector<std::string>& hostnames,
                         const std::vector<std::string>& services) {
        for (unsigned int i = 0; i < hostnames.size(); i++) {
            CONNECTION* connection = new CONNECTION(_ec, i);
            _conn.push_back(connection);
            connection->connect(hostnames[i], services[i]);
        }
    };

    /// Initiate disconnection.
    void disconnect() {
        for (auto it = _conn.begin(); it != _conn.end(); ++it)
            (*it)->disconnect();
    };

    /// The connection manager event loop.
    void handleCmEvents(bool isConnect = true) {
        int err;
        struct rdma_cm_event* event;
        struct rdma_cm_event event_copy;
        
        while ((err = rdma_get_cm_event(_ec, &event)) == 0) {
            memcpy(&event_copy, event, sizeof(struct rdma_cm_event));
            rdma_ack_cm_event(event);
            int err = onCmEvent(&event_copy);
            if (err)
                break;
            if (_connected == (isConnect ? _conn.size() : 0))
                break;
        };
        if (err)
            throw InfinibandException("rdma_get_cm_event failed");
        
        Log.info() << "number of connections: " << _connected;
    };

    /// The InfiniBand completion notification event loop.
    void completionHandler() {
        const int ne_max = 10;

        struct ibv_cq* ev_cq;
        void* ev_ctx;
        struct ibv_wc wc[ne_max];
        int ne;

        while (!_allDone) {
            if (ibv_get_cq_event(_compChannel, &ev_cq, &ev_ctx))
                throw InfinibandException("ibv_get_cq_event failed");

            ibv_ack_cq_events(ev_cq, 1);

            if (ev_cq != _cq)
                throw InfinibandException("CQ event for unknown CQ");

            if (ibv_req_notify_cq(_cq, 0))
                throw InfinibandException("ibv_req_notify_cq failed");

            while ((ne = ibv_poll_cq(_cq, ne_max, wc))) {
                if (ne < 0)
                    throw InfinibandException("ibv_poll_cq failed");

                for (int i = 0; i < ne; i++) {
                    if (wc[i].status != IBV_WC_SUCCESS) {
                        std::ostringstream s;
                        s << ibv_wc_status_str(wc[i].status)
                          << " for wr_id " << (int) wc[i].wr_id;
                        Log.error() << s.str();
                        continue;
                    }

                    onCompletion(wc[i]);
                }
            }
        }

        Log.info() << "COMPLETION loop done";
    }

    /// Retrieve the InfiniBand protection domain.
    struct ibv_pd* protectionDomain() const {
        return _pd;
    }

    /// Retrieve the InfiniBand completion queue.
    struct ibv_cq* completionQueue() const {
        return _cq;
    }

protected:

    /// InfiniBand protection domain.
    struct ibv_pd* _pd;

    /// Vector of associated connection objects.
    std::vector<CONNECTION*> _conn;

    /// Flag causing termination of completion handler.
    bool _allDone;

    /// Handle RDMA_CM_EVENT_ADDR_RESOLVED event.
    virtual int onAddrResolved(struct rdma_cm_id* id) {
        if (!_pd)
            initContext(id->verbs);

        CONNECTION* conn = (CONNECTION*) id->context;
        
        conn->onAddrResolved(_pd, _cq);

        return 0;
    }

    /// Handle RDMA_CM_EVENT_ROUTE_RESOLVED event.
    virtual int onRouteResolved(struct rdma_cm_id* id) {
        CONNECTION* conn = (CONNECTION*) id->context;

        conn->onRouteResolved();

        return 0;
    }

    /// Handle RDMA_CM_EVENT_ESTABLISHED event.
    virtual int onConnection(struct rdma_cm_event* event) {
        CONNECTION* conn = (CONNECTION*) event->id->context;

        conn->onConnection(event);
        _connected++;

        return 0;
    }

    /// Handle RDMA_CM_EVENT_DISCONNECTED event.
    virtual int onDisconnect(struct rdma_cm_id* id) {
        CONNECTION* conn = (CONNECTION*) id->context;

        conn->onDisconnect();
        _conn[conn->index()] = 0;
        delete conn;
        _connected--;

        return 0;
    }

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
            throw InfinibandException("rdma_resolve_addr failed");
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            return onRouteResolved(event->id);
        case RDMA_CM_EVENT_ROUTE_ERROR:
            throw InfinibandException("rdma_resolve_route failed");
        case RDMA_CM_EVENT_CONNECT_ERROR:
            throw InfinibandException("could not establish connection");
        case RDMA_CM_EVENT_UNREACHABLE:
            throw InfinibandException("remote server is not reachable");
        case RDMA_CM_EVENT_REJECTED:
            throw InfinibandException("request rejected by remote endpoint");
        case RDMA_CM_EVENT_ESTABLISHED:
            return onConnection(event);
        case RDMA_CM_EVENT_DISCONNECTED:
            return onDisconnect(event->id);
        default:
            Log.error() << rdma_event_str(event->event);
            return 0;
        }
    }

    /// Initialize the InfiniBand verbs context.
    void initContext(struct ibv_context* context) {
        _context = context;

        Log.debug() << "create verbs objects";

        _pd = ibv_alloc_pd(context);
        if (!_pd)
            throw InfinibandException("ibv_alloc_pd failed");

        _compChannel = ibv_create_comp_channel(context);
        if (!_compChannel)
            throw InfinibandException("ibv_create_comp_channel failed");

        _cq = ibv_create_cq(context, 40, NULL, _compChannel, 0);
        if (!_cq)
            throw InfinibandException("ibv_create_cq failed");

        if (ibv_req_notify_cq(_cq, 0))
            throw InfinibandException("ibv_req_notify_cq failed");
    }

    /// Completion notification event dispatcher. Called by the event loop.
    virtual void onCompletion(const struct ibv_wc& wc) { };
};


#endif /* INFINIBAND_HPP */

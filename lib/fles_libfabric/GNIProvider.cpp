// Copyright 2016 Thorsten Schuett <schuett@zib.de>

#include "GNIProvider.hpp"

#include <unistd.h>

#include <cassert>
#include <cstring>
#include <memory>
#include <string>
#include <iostream>

#include <rdma/fi_domain.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>

#include "LibfabricException.hpp"

GNIProvider::~GNIProvider()
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
    fi_freeinfo(info_);
    fi_close((fid_t)fabric_);
#pragma GCC diagnostic pop
}

struct fi_info* GNIProvider::exists(std::string local_host_name)
{
    struct fi_info* hints = fi_allocinfo();
    struct fi_info* info = nullptr;

    hints->caps = FI_RMA | FI_MSG | FI_REMOTE_WRITE;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->data_progress = FI_PROGRESS_AUTO;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    hints->domain_attr->mr_mode = FI_MR_BASIC;

    int res = fi_getinfo(FI_VERSION(1, 1), local_host_name.c_str(), nullptr, 0, hints, &info);

    if (!res && (strcmp("gni", info->fabric_attr->prov_name) == 0)) {
        std::cout << info->src_addrlen << std::endl;
        fi_freeinfo(hints);
        return info;
    }

    fi_freeinfo(info);
    fi_freeinfo(hints);

    return nullptr;
}

GNIProvider::GNIProvider(struct fi_info* info) : info_(info)
{
    int res = fi_fabric(info_->fabric_attr, &fabric_, nullptr);
    if (res)
        throw LibfabricException("fi_fabric failed");
}

void GNIProvider::accept(struct fid_pep* /*pep*/,
                         const std::string& /*hostname*/,
                         unsigned short /*port*/, unsigned int /*count*/,
                         fid_eq* /*eq*/)
{
    // there is no accept for GNI
}

void GNIProvider::connect(fid_ep* ep, uint32_t max_send_wr,
                          uint32_t max_send_sge, uint32_t max_recv_wr,
                          uint32_t max_recv_sge, uint32_t max_inline_data,
                          const void* param, size_t param_len, void* /*addr*/)
{
    // @todo send mr message?
}

void GNIProvider::set_hostnames_and_services(
    struct fid_av* av, const std::vector<std::string>& compute_hostnames,
    const std::vector<std::string>& compute_services,
    std::vector<fi_addr_t>& fi_addrs)
{
    struct fi_info* info = fi_allocinfo();
    struct fi_info* hints = fi_allocinfo();

    std::cout << compute_hostnames.size() << std::endl;
    for (size_t i = 0; i < compute_hostnames.size(); i++) {
        fi_addr_t fi_addr;

        hints->caps = FI_RMA | FI_MSG | FI_REMOTE_WRITE;
        hints->ep_attr->type = FI_EP_RDM;
        hints->domain_attr->data_progress = FI_PROGRESS_AUTO;
        hints->domain_attr->threading = FI_THREAD_SAFE;
        hints->domain_attr->mr_mode = FI_MR_BASIC;

        std::cout << compute_hostnames[i].c_str() << " "
                  << compute_services[i].c_str() << std::endl;
        int res = fi_getinfo(FI_VERSION(1, 1), compute_hostnames[i].c_str(),
                             compute_services[i].c_str(), 0, hints, &info);
        assert(res == 0);
        assert(info != NULL);

        std::cout << info->dest_addrlen << std::endl;
        while (info != NULL) {
            std::cout << info->fabric_attr->prov_name << std::endl;
            if (strcmp("gni", info->fabric_attr->prov_name) == 0)
                break;
            info = info->next;
        }
        assert(info != NULL);
        assert(info->dest_addr != NULL);
        res = fi_av_insert(av, info->dest_addr, 1, &fi_addr, 0, NULL);
        assert(res == 1);
        fi_addrs.push_back(fi_addr);
    }
}

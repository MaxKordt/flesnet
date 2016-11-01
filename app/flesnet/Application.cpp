// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "Application.hpp"
#include "EmbeddedPatternGenerator.hpp"
#include "EtcdClient.h"
#include "FlibPatternGenerator.hpp"
#include "shm_channel_client.hpp"
#include <boost/thread/future.hpp>
#include <boost/thread/thread.hpp>
#include <log.hpp>

Application::Application(Parameters const& par,
                         volatile sig_atomic_t* signal_status)
    : par_(par), signal_status_(signal_status)
{
    unsigned input_nodes_size = par.input_nodes().size();
    std::vector<unsigned> input_indexes = par.input_indexes();

    // FIXME: some of this is a terrible mess
    if (!par.input_shm().empty()) {
        try {
            EtcdClient etcd(par.base_url());

            if (par.kv_sync() == true) {
                L_(info) << "Now using key-value store for synchronization at "
                         << par.base_url();
                etcd.check_process(par.input_shm());
            }

            shm_device_ =
                std::make_shared<flib_shm_device_client>(par.input_shm());
            shm_num_channels_ = shm_device_->num_channels();
            L_(info) << "using shared memory";

            // increase number of input nodes to match number of
            // enabled FLIB links if in stand-alone mode
            if (par.standalone() && shm_num_channels_ > 1) {
                input_nodes_size = shm_num_channels_;
                for (unsigned i = 1; i < input_nodes_size; i++) {
                    input_indexes.push_back(i);
                }
            }

        } catch (std::exception const& e) {
            L_(error) << "exception while connecting to shared memory: "
                      << e.what();
        }
    }
    // end FIXME

    if (par.standalone()) {
        L_(info) << "flesnet in stand-alone mode, inputs: " << input_nodes_size;
    }

    // Compute node application

    // set_cpu(1);

    for (unsigned i : par_.compute_indexes()) {
        std::unique_ptr<ComputeBuffer> buffer(new ComputeBuffer(
            i, par_.cn_data_buffer_size_exp(), par_.cn_desc_buffer_size_exp(),
            par_.base_port() + i, input_nodes_size, par_.timeslice_size(),
            par_.processor_instances(), par_.processor_executable(),
            signal_status_));
        buffer->start_processes();
        compute_buffers_.push_back(std::move(buffer));
    }

    set_node();

    // Input node application

    std::vector<std::string> compute_services;
    for (unsigned int i = 0; i < par.compute_nodes().size(); ++i)
        compute_services.push_back(
            boost::lexical_cast<std::string>(par.base_port() + i));

    for (size_t c = 0; c < input_indexes.size(); ++c) {
        unsigned index = input_indexes.at(c);

        if (c < shm_num_channels_) {
            data_sources_.push_back(std::unique_ptr<InputBufferReadInterface>(
                new flib_shm_channel_client(shm_device_, c)));
        } else {
            if (false) {
                data_sources_.push_back(
                    std::unique_ptr<InputBufferReadInterface>(
                        new FlibPatternGenerator(par.in_data_buffer_size_exp(),
                                                 par.in_desc_buffer_size_exp(),
                                                 index,
                                                 par.typical_content_size())));
            } else {
                data_sources_.push_back(
                    std::unique_ptr<InputBufferReadInterface>(
                        new EmbeddedPatternGenerator(
                            par.in_data_buffer_size_exp(),
                            par.in_desc_buffer_size_exp(), index,
                            par.typical_content_size())));
            }
        }

        std::unique_ptr<InputChannelSender> buffer(new InputChannelSender(
            index, *(data_sources_.at(c).get()), par.compute_nodes(),
            compute_services, par.timeslice_size(), par.overlap_size(),
            par.max_timeslice_number()));

        input_channel_senders_.push_back(std::move(buffer));
    }
}

Application::~Application() {}

void Application::run()
{
    // Do not spawn additional thread if only one is needed, simplifies
    // debugging
    if (compute_buffers_.size() == 1 && input_channel_senders_.empty()) {
        L_(debug) << "using existing thread for single compute buffer";
        (*compute_buffers_[0])();
        return;
    };
    if (input_channel_senders_.size() == 1 && compute_buffers_.empty()) {
        L_(debug) << "using existing thread for single input buffer";
        (*input_channel_senders_[0])();
        return;
    };

    // FIXME: temporary code, need to implement interrupt
    boost::thread_group threads;
    std::vector<boost::unique_future<void>> futures;
    bool stop = false;

    for (auto& buffer : compute_buffers_) {
        boost::packaged_task<void> task(std::ref(*buffer));
        futures.push_back(task.get_future());
        threads.add_thread(new boost::thread(std::move(task)));
    }

    for (auto& buffer : input_channel_senders_) {
        boost::packaged_task<void> task(std::ref(*buffer));
        futures.push_back(task.get_future());
        threads.add_thread(new boost::thread(std::move(task)));
    }

    L_(debug) << "threads started: " << threads.size();

    while (!futures.empty()) {
        auto it = boost::wait_for_any(futures.begin(), futures.end());
        try {
            it->get();
        } catch (const std::exception& e) {
            L_(fatal) << "exception from thread: " << e.what();
            stop = true;
        }
        futures.erase(it);
        if (stop)
            threads.interrupt_all();
    }

    threads.join_all();
}

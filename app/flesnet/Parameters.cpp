// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "Parameters.hpp"
#include "GitRevision.hpp"
#include "MicrosliceDescriptor.hpp"
#include "TimesliceComponentDescriptor.hpp"
#include "Utility.hpp"
#include "log.hpp"
#include <algorithm>
#include <boost/algorithm/string/join.hpp>
#include <boost/program_options.hpp>
#include <cpprest/base_uri.h>
#include <fstream>
#include <iterator>

namespace po = boost::program_options;

std::istream& operator>>(std::istream& in, Transport& transport)
{
    std::string token;
    in >> token;
    std::transform(std::begin(token), std::end(token), std::begin(token),
                   [](const unsigned char i) { return tolower(i); });

    if (token == "rdma" || token == "r")
        transport = Transport::RDMA;
    else if (token == "libfabric" || token == "f")
        transport = Transport::LibFabric;
    else if (token == "zeromq" || token == "z")
        transport = Transport::ZeroMQ;
    else
        throw po::invalid_option_value(token);
    return in;
}

std::ostream& operator<<(std::ostream& out, const Transport& transport)
{
    switch (transport) {
    case Transport::RDMA:
        out << "RDMA";
        break;
    case Transport::LibFabric:
        out << "LibFabric";
        break;
    case Transport::ZeroMQ:
        out << "ZeroMQ";
        break;
    }
    return out;
}

std::istream& operator>>(std::istream& in, InterfaceSpecification& ifspec)
{
    in >> ifspec.full_uri;
    try {
        web::uri uri(ifspec.full_uri);
        ifspec.scheme = uri.scheme();
        ifspec.host = uri.host();
        ifspec.path = web::uri::split_path(uri.path());
        ifspec.param = web::uri::split_query(uri.query());
    } catch (const web::uri_exception& e) {
        throw po::invalid_option_value(ifspec.full_uri);
    }
    return in;
}

std::ostream& operator<<(std::ostream& out,
                         const InterfaceSpecification& ifspec)
{
    out << ifspec.full_uri;
    return out;
}

uint32_t Parameters::suggest_in_data_buffer_size_exp()
{
    constexpr float buffer_ram_usage_ratio = 0.05f;

    // ensure value in sensible range
    constexpr uint32_t max_in_data_buffer_size_exp = 30; // 30: 1 GByte
    constexpr uint32_t min_in_data_buffer_size_exp = 20; // 20: 1 MByte

    float total_ram = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    float suggest_in_data_buffer_size =
        buffer_ram_usage_ratio * total_ram / input_indexes_.size();

    uint32_t suggest_in_data_buffer_size_exp =
        static_cast<uint32_t>(ceilf(log2f(suggest_in_data_buffer_size)));

    if (suggest_in_data_buffer_size_exp > max_in_data_buffer_size_exp)
        suggest_in_data_buffer_size_exp = max_in_data_buffer_size_exp;
    if (suggest_in_data_buffer_size_exp < min_in_data_buffer_size_exp)
        suggest_in_data_buffer_size_exp = min_in_data_buffer_size_exp;

    return suggest_in_data_buffer_size_exp;
}

uint32_t Parameters::suggest_in_desc_buffer_size_exp()
{
    // make desc buffer larger by this factor to account for data size
    // fluctuations
    constexpr float in_desc_buffer_oversize_factor = 4.0;

    // ensure value in sensible range
    constexpr float max_desc_data_ratio = 1.0f;
    constexpr float min_desc_data_ratio = 0.1f;

    static_assert(min_desc_data_ratio <= max_desc_data_ratio,
                  "invalid range for desc_data_ratio");

    float in_data_buffer_size = UINT64_C(1) << in_data_buffer_size_exp_;
    float suggest_in_desc_buffer_size = in_data_buffer_size /
                                        typical_content_size_ *
                                        in_desc_buffer_oversize_factor;
    uint32_t suggest_in_desc_buffer_size_exp =
        static_cast<uint32_t>(ceilf(log2f(suggest_in_desc_buffer_size)));

    float relative_size =
        in_data_buffer_size / sizeof(fles::MicrosliceDescriptor);
    uint32_t max_in_desc_buffer_size_exp = static_cast<uint32_t>(
        floorf(log2f(relative_size * max_desc_data_ratio)));
    uint32_t min_in_desc_buffer_size_exp = static_cast<uint32_t>(
        ceilf(log2f(relative_size * min_desc_data_ratio)));

    if (suggest_in_desc_buffer_size_exp > max_in_desc_buffer_size_exp)
        suggest_in_desc_buffer_size_exp = max_in_desc_buffer_size_exp;
    if (suggest_in_desc_buffer_size_exp < min_in_desc_buffer_size_exp)
        suggest_in_desc_buffer_size_exp = min_in_desc_buffer_size_exp;

    return suggest_in_desc_buffer_size_exp;
}

void Parameters::parse_options(int argc, char* argv[])
{
    unsigned log_level = 2;
    std::string log_file;
    std::string config_file;

    po::options_description generic("Generic options");
    auto generic_add = generic.add_options();
    generic_add("config-file,f",
                po::value<std::string>(&config_file)->value_name("<filename>"),
                "read configuration from file");
    generic_add("log-level,l",
                po::value<unsigned>(&log_level)
                    ->default_value(log_level)
                    ->value_name("<n>"),
                "set the global log level (all: 0)");
    generic_add("log-file,L",
                po::value<std::string>(&log_file)->value_name("<filename>"),
                "write log output to file");
    generic_add("help,h", "display this help and exit");
    generic_add("version,V", "output version information and exit");

    po::options_description config("Configuration");
    auto config_add = config.add_options();
    config_add(
        "input-index,i",
        po::value<std::vector<unsigned>>()->multitoken()->value_name("<n> ..."),
        "set this application's index(es) in the list of inputs");
    config_add(
        "output-index,o",
        po::value<std::vector<unsigned>>()->multitoken()->value_name("<n> ..."),
        "set this application's index(es) in the list of outputs");
    config_add("input,I",
               po::value<std::vector<InterfaceSpecification>>()
                   ->multitoken()
                   ->value_name("scheme://host/path?param=value ..."),
               "add an input to the list of participating inputs");
    config_add("output,O",
               po::value<std::vector<InterfaceSpecification>>()
                   ->multitoken()
                   ->value_name("scheme://host/path?param=value ..."),
               "add an output to the list of compute node outputs");
    config_add("timeslice-size",
               po::value<uint32_t>(&timeslice_size_)
                   ->default_value(timeslice_size_)
                   ->value_name("<n>"),
               "set the global timeslice size in number of microslices");
    config_add(
        "overlap-size",
        po::value<uint32_t>(&overlap_size_)
            ->default_value(overlap_size_)
            ->value_name("<n>"),
        "set the global size of the overlap region in number of microslices");
    config_add(
        "in-data-buffer-size-exp",
        po::value<uint32_t>(&in_data_buffer_size_exp_)->value_name("<n>"),
        "data buffer size used per input in the input node (log of number of "
        "bytes)");
    config_add(
        "in-desc-buffer-size-exp",
        po::value<uint32_t>(&in_desc_buffer_size_exp_)->value_name("<n>"),
        "descriptor buffer size used per input in the input node (log of "
        "number of entries)");
    config_add("typical-content-size",
               po::value<uint32_t>(&typical_content_size_)
                   ->default_value(typical_content_size_)
                   ->value_name("<n>"),
               "typical number of content bytes per microslice");
    config_add("input-shm",
               po::value<std::string>(&input_shm_)->value_name("<id>"),
               "name of a shared memory to use as data source");
    config_add("max-timeslice-number,n",
               po::value<uint32_t>(&max_timeslice_number_)->value_name("<n>"),
               "quit after processing given number of timeslices");
    config_add(
        "processor-executable,e",
        po::value<std::string>(&processor_executable_)->value_name("<string>"),
        "name of the executable acting as timeslice processor");
    config_add("processor-instances",
               po::value<uint32_t>(&processor_instances_)
                   ->default_value(processor_instances_)
                   ->value_name("<n>"),
               "number of instances of the timeslice processor executable");
    config_add("base-port",
               po::value<uint32_t>(&base_port_)
                   ->default_value(base_port_)
                   ->value_name("<n>"),
               "base IP port to use for listening");
    config_add(
        "generate-ts-patterns",
        po::value<bool>(&generate_ts_patterns_)
            ->default_value(generate_ts_patterns_)
            ->implicit_value(true)
            ->value_name("<bool>"),
        "generate data pattern when using the microslice pattern generator");
    config_add("random-ts-sizes",
               po::value<bool>(&random_ts_sizes_)
                   ->default_value(random_ts_sizes_)
                   ->implicit_value(true)
                   ->value_name("<bool>"),
               "randomize microslice data sizes when using the microslice "
               "pattern generator");
    config_add("transport,t",
               po::value<Transport>(&transport_)
                   ->default_value(transport_)
                   ->value_name("<id>"),
               "select transport implementation; possible values "
               "(case-insensitive) are: RDMA, LibFabric, ZeroMQ");

    po::options_description cmdline_options("Allowed options");
    cmdline_options.add(generic).add(config);

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, cmdline_options), vm);
    po::notify(vm);

    if (!config_file.empty()) {
        std::ifstream ifs(config_file.c_str());
        if (!ifs) {
            throw ParametersException("cannot open config file: " +
                                      config_file);
        } else {
            po::store(po::parse_config_file(ifs, config), vm);
            notify(vm);
        }
    }

    if (vm.count("help")) {
        std::cout << "flesnet, git revision " << g_GIT_REVISION << std::endl;
        std::cout << cmdline_options << std::endl;
        exit(EXIT_SUCCESS);
    }

    if (vm.count("version")) {
        std::cout << "flesnet, git revision " << g_GIT_REVISION << std::endl;
        exit(EXIT_SUCCESS);
    }

    logging::add_console(static_cast<severity_level>(log_level));
    if (vm.count("log-file")) {
        L_(info) << "logging output to " << log_file;
        if (log_level < 3) {
            log_level = 3;
            L_(info) << "increased file log level to " << log_level;
        }
        logging::add_file(log_file, static_cast<severity_level>(log_level));
    }

    if (timeslice_size_ < 1) {
        throw ParametersException("timeslice size cannot be zero");
    }

#ifndef HAVE_RDMA
    if (transport_ == Transport::RDMA) {
        throw ParametersException("flesnet built without RDMA support");
    }
#endif
#ifndef HAVE_LIBFABRIC
    if (transport_ == Transport::LibFabric) {
        throw ParametersException("flesnet built without LIBFABRIC support");
    }
#endif

    if (!vm.count("input"))
        throw ParametersException("list of inputs is empty");

    if (!vm.count("output"))
        throw ParametersException("list of outputs is empty");

    inputs_ = vm["input"].as<std::vector<InterfaceSpecification>>();
    outputs_ = vm["output"].as<std::vector<InterfaceSpecification>>();

    for (auto& input : inputs_) {
        if (!web::uri::validate(input.full_uri))
            throw ParametersException("invalid input specification: " +
                                      input.full_uri);
    }

    for (auto& output : outputs_) {
        if (!web::uri::validate(output.full_uri))
            throw ParametersException("invalid output specification: " +
                                      output.full_uri);
    }

    if (vm.count("input-index"))
        input_indexes_ = vm["input-index"].as<std::vector<unsigned>>();
    if (vm.count("output-index"))
        output_indexes_ = vm["output-index"].as<std::vector<unsigned>>();

    if (inputs_.empty() && outputs_.empty()) {
        throw ParametersException("no node type specified");
    }

    for (auto input_index : input_indexes_) {
        if (input_index >= inputs_.size()) {
            std::ostringstream oss;
            oss << "input index (" << input_index << ") out of range (0.."
                << inputs_.size() - 1 << ")";
            throw ParametersException(oss.str());
        }
    }

    for (auto output_index : output_indexes_) {
        if (output_index >= outputs_.size()) {
            std::ostringstream oss;
            oss << "output index (" << output_index << ") out of range (0.."
                << outputs_.size() - 1 << ")";
            throw ParametersException(oss.str());
        }
    }

    if (!outputs_.empty() && processor_executable_.empty())
        throw ParametersException("processor executable not specified");

    if (in_data_buffer_size_exp_ == 0 && input_shm().empty()) {
        in_data_buffer_size_exp_ = suggest_in_data_buffer_size_exp();
    }
    if (in_data_buffer_size_exp_ != 0 && !input_shm().empty()) {
        L_(warning) << "using shared memory buffers, in_data_buffer_size_exp "
                       "will be ignored";
        in_data_buffer_size_exp_ = 0;
    }

    if (in_desc_buffer_size_exp_ == 0 && input_shm().empty()) {
        in_desc_buffer_size_exp_ = suggest_in_desc_buffer_size_exp();
    }
    if (in_desc_buffer_size_exp_ != 0 && !input_shm().empty()) {
        L_(warning) << "using shared memory buffers, in_desc_buffer_size_exp "
                       "will be ignored";
        in_desc_buffer_size_exp_ = 0;
    }

    L_(debug) << "inputs (" << inputs_.size()
              << "): " << boost::algorithm::join(input_uris(), " ");
    L_(debug) << "outputs (" << outputs_.size()
              << "): " << boost::algorithm::join(output_uris(), " ");
    for (auto input_index : input_indexes_) {
        L_(info) << "this is input node " << input_index << " (of "
                 << inputs_.size() << ")";
    }
    for (auto output_index : output_indexes_) {
        L_(info) << "this is output " << output_index << " (of "
                 << outputs_.size() << ")";
    }

    for (auto input_index : input_indexes_) {
        if (input_index == 0) {
            L_(info) << "microslice size: "
                     << human_readable_count(typical_content_size_);
            L_(info) << "timeslice size: (" << timeslice_size_ << " + "
                     << overlap_size_ << ") microslices";
            L_(info) << "number of timeslices: " << max_timeslice_number_;
        }
    }
}

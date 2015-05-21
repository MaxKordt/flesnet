// Copyright 2012-2013 Jan de Cuveland <cmail@cuveland.de>

#include "Parameters.hpp"
#include "MicrosliceDescriptor.hpp"
#include "TimesliceComponentDescriptor.hpp"
#include "Utility.hpp"
#include <log.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string/join.hpp>
#include <fstream>

namespace po = boost::program_options;

std::string const Parameters::desc() const
{
    std::stringstream st;

    st << "input nodes (" << _input_nodes.size() << "): " << _input_nodes
       << std::endl;
    st << "compute nodes (" << _compute_nodes.size() << "): " << _compute_nodes
       << std::endl;
    for (auto input_index : _input_indexes) {
        st << "this is input node " << input_index << " (of "
           << _input_nodes.size() << ")" << std::endl;
    }
    for (auto compute_index : _compute_indexes) {
        st << "this is compute node " << compute_index << " (of "
           << _compute_nodes.size() << ")" << std::endl;
    }

    return st.str();
}

uint32_t Parameters::suggest_in_data_buffer_size_exp()
{
    constexpr float buffer_ram_usage_ratio = 0.05;

    // ensure value in sensible range
    constexpr uint32_t max_in_data_buffer_size_exp = 30; // 30: 1 GByte
    constexpr uint32_t min_in_data_buffer_size_exp = 20; // 20: 1 MByte

    float total_ram = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    float suggest_in_data_buffer_size =
        buffer_ram_usage_ratio * total_ram / _input_indexes.size();

    uint32_t suggest_in_data_buffer_size_exp =
        ceilf(log2f(suggest_in_data_buffer_size));

    if (suggest_in_data_buffer_size_exp > max_in_data_buffer_size_exp)
        suggest_in_data_buffer_size_exp = max_in_data_buffer_size_exp;
    if (suggest_in_data_buffer_size_exp < min_in_data_buffer_size_exp)
        suggest_in_data_buffer_size_exp = min_in_data_buffer_size_exp;

    return suggest_in_data_buffer_size_exp;
}

uint32_t Parameters::suggest_cn_data_buffer_size_exp()
{
    constexpr float buffer_ram_usage_ratio = 0.05;

    // ensure value in sensible range
    constexpr uint32_t max_cn_data_buffer_size_exp = 30; // 30: 1 GByte
    constexpr uint32_t min_cn_data_buffer_size_exp = 20; // 20: 1 MByte

    float total_ram = sysconf(_SC_PHYS_PAGES) * sysconf(_SC_PAGE_SIZE);
    float suggest_cn_data_buffer_size =
        buffer_ram_usage_ratio * total_ram /
        (_compute_indexes.size() * _input_nodes.size());

    uint32_t suggest_cn_data_buffer_size_exp =
        ceilf(log2f(suggest_cn_data_buffer_size));

    if (suggest_cn_data_buffer_size_exp > max_cn_data_buffer_size_exp)
        suggest_cn_data_buffer_size_exp = max_cn_data_buffer_size_exp;
    if (suggest_cn_data_buffer_size_exp < min_cn_data_buffer_size_exp)
        suggest_cn_data_buffer_size_exp = min_cn_data_buffer_size_exp;

    return suggest_cn_data_buffer_size_exp;
}

uint32_t Parameters::suggest_in_desc_buffer_size_exp()
{
    // make desc buffer larger by this factor to account for data size
    // fluctuations
    constexpr float in_desc_buffer_oversize_factor = 4.0;

    // ensure value in sensible range
    constexpr float max_desc_data_ratio = 1.0;
    constexpr float min_desc_data_ratio = 0.1;

    static_assert(min_desc_data_ratio <= max_desc_data_ratio,
                  "invalid range for desc_data_ratio");

    float in_data_buffer_size = UINT64_C(1) << _in_data_buffer_size_exp;
    float suggest_in_desc_buffer_size = in_data_buffer_size /
                                        _typical_content_size *
                                        in_desc_buffer_oversize_factor;
    uint32_t suggest_in_desc_buffer_size_exp =
        ceilf(log2f(suggest_in_desc_buffer_size));

    float relative_size =
        in_data_buffer_size / sizeof(fles::MicrosliceDescriptor);
    uint32_t max_in_desc_buffer_size_exp =
        floorf(log2f(relative_size * max_desc_data_ratio));
    uint32_t min_in_desc_buffer_size_exp =
        ceilf(log2f(relative_size * min_desc_data_ratio));

    if (suggest_in_desc_buffer_size_exp > max_in_desc_buffer_size_exp)
        suggest_in_desc_buffer_size_exp = max_in_desc_buffer_size_exp;
    if (suggest_in_desc_buffer_size_exp < min_in_desc_buffer_size_exp)
        suggest_in_desc_buffer_size_exp = min_in_desc_buffer_size_exp;

    return suggest_in_desc_buffer_size_exp;
}

uint32_t Parameters::suggest_cn_desc_buffer_size_exp()
{
    // make desc buffer larger by this factor to account for data size
    // fluctuations
    constexpr float cn_desc_buffer_oversize_factor = 8.0;

    // ensure value in sensible range
    constexpr float min_desc_data_ratio = 0.1;
    constexpr float max_desc_data_ratio = 1.0;

    static_assert(min_desc_data_ratio <= max_desc_data_ratio,
                  "invalid range for desc_data_ratio");

    float cn_data_buffer_size = UINT64_C(1) << _cn_data_buffer_size_exp;
    float suggest_cn_desc_buffer_size =
        cn_data_buffer_size /
        (_typical_content_size * (_timeslice_size + _overlap_size)) *
        cn_desc_buffer_oversize_factor;

    uint32_t suggest_cn_desc_buffer_size_exp =
        ceilf(log2f(suggest_cn_desc_buffer_size));

    float relative_size =
        cn_data_buffer_size / sizeof(fles::TimesliceComponentDescriptor);
    uint32_t min_cn_desc_buffer_size_exp =
        ceilf(log2f(relative_size * min_desc_data_ratio));
    uint32_t max_cn_desc_buffer_size_exp =
        floorf(log2f(relative_size * max_desc_data_ratio));

    if (suggest_cn_desc_buffer_size_exp < min_cn_desc_buffer_size_exp)
        suggest_cn_desc_buffer_size_exp = min_cn_desc_buffer_size_exp;
    if (suggest_cn_desc_buffer_size_exp > max_cn_desc_buffer_size_exp)
        suggest_cn_desc_buffer_size_exp = max_cn_desc_buffer_size_exp;

    return suggest_cn_desc_buffer_size_exp;
}

void Parameters::parse_options(int argc, char* argv[])
{
    unsigned log_level = 2;
    std::string config_file;

    po::options_description generic("Generic options");
    generic.add_options()("version,V", "print version string")(
        "help,h",
        "produce help message")("log-level,l", po::value<unsigned>(&log_level),
                                "set the log level (default:2, all:0)")(
        "config-file,f",
        po::value<std::string>(&config_file)->default_value("flesnet.cfg"),
        "name of a configuration file.");

    po::options_description config("Configuration");
    config.add_options()("input-index,i",
                         po::value<std::vector<unsigned>>()->multitoken(),
                         "this application's index in the list of input nodes")(
        "compute-index,c", po::value<std::vector<unsigned>>()->multitoken(),
        "this application's index in the list of compute nodes")(
        "input-nodes,I", po::value<std::vector<std::string>>()->multitoken(),
        "add host to the list of input nodes")(
        "compute-nodes,C", po::value<std::vector<std::string>>()->multitoken(),
        "add host to the list of compute nodes")(
        "timeslice-size", po::value<uint32_t>(&_timeslice_size),
        "global timeslice size in number of MCs")(
        "overlap-size", po::value<uint32_t>(&_overlap_size),
        "size of the overlap region in number of MCs")(
        "in-data-buffer-size-exp",
        po::value<uint32_t>(&_in_data_buffer_size_exp),
        "exp. size of the input node's data buffer in bytes")(
        "in-desc-buffer-size-exp",
        po::value<uint32_t>(&_in_desc_buffer_size_exp),
        "exp. size of the input node's descriptor buffer"
        " (number of entries)")(
        "cn-data-buffer-size-exp",
        po::value<uint32_t>(&_cn_data_buffer_size_exp),
        "exp. size of the compute node's data buffer in bytes")(
        "cn-desc-buffer-size-exp",
        po::value<uint32_t>(&_cn_desc_buffer_size_exp),
        "exp. size of the compute node's descriptor buffer"
        " (number of entries)")("typical-content-size",
                                po::value<uint32_t>(&_typical_content_size),
                                "typical number of content bytes per MC")(
        "use-flib", po::value<bool>(&_use_flib), "use flib flag")(
        "use-shm", po::value<bool>(&_use_shared_memory), "use shared_meory flag")(
        "standalone", po::value<bool>(&_standalone), "standalone mode flag")(
        "max-timeslice-number,n", po::value<uint32_t>(&_max_timeslice_number),
        "global maximum timeslice number")(
        "processor-executable,e",
        po::value<std::string>(&_processor_executable),
        "name of the executable acting as timeslice processor")(
        "processor-instances", po::value<uint32_t>(&_processor_instances),
        "number of instances of the timeslice processor executable")(
        "base-port", po::value<uint32_t>(&_base_port),
        "base IP port to use for listening");

    po::options_description cmdline_options("Allowed options");
    cmdline_options.add(generic).add(config);

    po::variables_map vm;
    po::store(po::parse_command_line(argc, argv, cmdline_options), vm);
    po::notify(vm);

    std::ifstream ifs(config_file.c_str());
    if (!ifs) {
        std::cout << "can not open config file: " << config_file << "\n";
        exit(EXIT_SUCCESS);
    } else {
        po::store(po::parse_config_file(ifs, config), vm);
        notify(vm);
    }

    if (vm.count("help")) {
        std::cout << cmdline_options << "\n";
        exit(EXIT_SUCCESS);
    }

    if (vm.count("version")) {
        std::cout << "flesnet, version 0.0"
                  << "\n";
        exit(EXIT_SUCCESS);
    }

    logging::add_console(static_cast<severity_level>(log_level));

    if (_timeslice_size < 1) {
        throw ParametersException("timeslice size cannot be zero");
    }

    if (_standalone) {
        _input_nodes = std::vector<std::string>{"127.0.0.1"};
        _input_indexes = std::vector<unsigned>{0};
        _compute_nodes = std::vector<std::string>{"127.0.0.1"};
        _compute_indexes = std::vector<unsigned>{0};
    } else {
        if (!vm.count("input-nodes"))
            throw ParametersException("list of input nodes is empty");

        if (!vm.count("compute-nodes"))
            throw ParametersException("list of compute nodes is empty");

        _input_nodes = vm["input-nodes"].as<std::vector<std::string>>();
        _compute_nodes = vm["compute-nodes"].as<std::vector<std::string>>();

        if (vm.count("input-index"))
            _input_indexes = vm["input-index"].as<std::vector<unsigned>>();
        if (vm.count("compute-index"))
            _compute_indexes = vm["compute-index"].as<std::vector<unsigned>>();

        if (_input_nodes.empty() && _compute_nodes.empty()) {
            throw ParametersException("no node type specified");
        }

        for (auto input_index : _input_indexes) {
            if (input_index >= _input_nodes.size()) {
                std::ostringstream oss;
                oss << "input node index (" << input_index
                    << ") out of range (0.." << _input_nodes.size() - 1 << ")";
                throw ParametersException(oss.str());
            }
        }

        for (auto compute_index : _compute_indexes) {
            if (compute_index >= _compute_nodes.size()) {
                std::ostringstream oss;
                oss << "compute node index (" << compute_index
                    << ") out of range (0.." << _compute_nodes.size() - 1
                    << ")";
                throw ParametersException(oss.str());
            }
        }
    }

    if (!_compute_nodes.empty() && _processor_executable.empty())
        throw ParametersException("processor executable not specified");

    if (_in_data_buffer_size_exp == 0 && !_use_shared_memory) {
        _in_data_buffer_size_exp = suggest_in_data_buffer_size_exp();
    }
    if (_in_data_buffer_size_exp != 0 && _use_shared_memory) {
      L_(warning) << "using shared memory buffers, in_data_buffer_size_exp will be ignored";
      _in_data_buffer_size_exp = 0;
    }

    if (_cn_data_buffer_size_exp == 0)
        _cn_data_buffer_size_exp = suggest_cn_data_buffer_size_exp();

    if (_in_desc_buffer_size_exp == 0 && !_use_shared_memory) {
        _in_desc_buffer_size_exp = suggest_in_desc_buffer_size_exp();
    }
    if (_in_desc_buffer_size_exp != 0 && _use_shared_memory) {
      L_(warning) << "using shared memory buffers, in_desc_buffer_size_exp will be ignored";
      _in_desc_buffer_size_exp = 0;
    }

    if (_cn_desc_buffer_size_exp == 0)
        _cn_desc_buffer_size_exp = suggest_cn_desc_buffer_size_exp();

    if (!_standalone) {
        L_(debug) << "input nodes (" << _input_nodes.size()
                  << "): " << boost::algorithm::join(_input_nodes, " ");
        L_(debug) << "compute nodes (" << _compute_nodes.size()
                  << "): " << boost::algorithm::join(_compute_nodes, " ");
        for (auto input_index : _input_indexes) {
            L_(info) << "this is input node " << input_index << " (of "
                     << _input_nodes.size() << ")";
        }
        for (auto compute_index : _compute_indexes) {
            L_(info) << "this is compute node " << compute_index << " (of "
                     << _compute_nodes.size() << ")";
        }

        for (auto input_index : _input_indexes) {
            if (input_index == 0) {
                print_buffer_info();
            }
        }
    } else {
        print_buffer_info();
    }
}

void Parameters::print_buffer_info()
{
    L_(info) << "microslice size: "
             << human_readable_count(_typical_content_size);
    L_(info) << "timeslice size: (" << _timeslice_size << " + " << _overlap_size
             << ") microslices";
    L_(info) << "number of timeslices: " << _max_timeslice_number;
    if (!_use_shared_memory) {
        L_(info) << "input node buffer size: "
                 << human_readable_count(UINT64_C(1) << _in_data_buffer_size_exp)
                 << " + "
                 << human_readable_count((UINT64_C(1) << _in_desc_buffer_size_exp) *
                                         sizeof(fles::MicrosliceDescriptor));
    }
    L_(info) << "compute node buffer size: "
             << human_readable_count(UINT64_C(1) << _cn_data_buffer_size_exp)
             << " + " << human_readable_count(
                             (UINT64_C(1) << _cn_desc_buffer_size_exp) *
                             sizeof(fles::TimesliceComponentDescriptor));
}

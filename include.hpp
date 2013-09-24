/**
 * \file include.hpp
 *
 * 2013, Jan de Cuveland <cmail@cuveland.de>
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <fstream>
#include <iostream>
#include <queue>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <arpa/inet.h>
#include <infiniband/arch.h>
#include <netdb.h>
#include <rdma/rdma_cma.h>
#include <valgrind/memcheck.h>
#include <sched.h>
#include <numa.h>

#include <boost/program_options.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/random/poisson_distribution.hpp>
#include <boost/random/variate_generator.hpp>
#include <boost/thread.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>

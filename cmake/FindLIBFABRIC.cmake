# Copyright 2016 Thorsten Schuett <schuett@zib.de>, Farouk Salem <salem@zib.de>

find_path(LIBFABRIC_INCLUDE_DIRS rdma/fabric.h)
find_library(LIBFABRIC_LIBRARIES NAMES fabric)

MESSAGE(STATUS "libfabric           = ${LIBFABRIC_LIBRARIES}")
MESSAGE(STATUS "libfabric           = ${LIBFABRIC_INCLUDE_DIRS}")

target_include_directories(${LIBFABRIC_INCLUDE_DIRS})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(LIBFABRIC REQUIRED_VARS LIBFABRIC_LIBRARIES LIBFABRIC_INCLUDE_DIRS)

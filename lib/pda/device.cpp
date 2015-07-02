/**
 * @file
 * @author Dirk Hutter <hutter@compeng.uni-frankfurt.de>
 * @author Dominic Eschweiler<dominic.eschweiler@cern.ch>
 *
 */

// TODO: Why is '0' returned everywere?

#include <iostream>
#include <cstdlib>
#include <cstring>

#include <pda.h>

#include <device.hpp>
#include <data_structures.hpp>

using namespace std;

namespace pda {
device::device(int32_t device_index) {
  const char* pci_ids[] = {
      "10dc beaf", /* CRORC as registered at CERN */
      NULL         /* Delimiter*/
  };

  if ((m_dop = DeviceOperator_new(pci_ids)) == NULL) {
    throw PdaException("Device operator instantiation failed.");
  }

  if (DeviceOperator_getPciDevice(m_dop, &m_device, device_index) !=
      PDA_SUCCESS) {
    throw PdaException("Device object creation failed.");
  }
}

device::~device() {
  if (DeviceOperator_delete(m_dop, PDA_DELETE_PERSISTANT) != PDA_SUCCESS) {
    cout << "Deleting device operator failed!" << endl;
  }
}

uint16_t device::domain() {
  uint16_t domain_id;
  if (PciDevice_getDomainID(m_device, &domain_id) == PDA_SUCCESS) {
    return (domain_id);
  }

  return (0);
}

uint8_t device::bus() {
  uint8_t bus_id;
  if (PciDevice_getBusID(m_device, &bus_id) == PDA_SUCCESS) {
    return (bus_id);
  }

  return (0);
}

uint8_t device::slot() {
  uint8_t device_id;
  if (PciDevice_getDeviceID(m_device, &device_id) == PDA_SUCCESS) {
    return (device_id);
  }

  return (0);
}

uint8_t device::func() {
  uint8_t function_id;
  if (PciDevice_getFunctionID(m_device, &function_id) == PDA_SUCCESS) {
    return (function_id);
  }

  return (0);
}

size_t device::max_payload_size() {
  size_t max_payload_size;
  if (PciDevice_getmaxPayloadSize(m_device, &max_payload_size) == PDA_SUCCESS) {
    return (max_payload_size);
  }
  return (0);
}

} // namespace

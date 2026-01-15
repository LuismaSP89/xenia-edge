/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_NET_UTILS_H_
#define XENIA_KERNEL_XAM_NET_UTILS_H_

#include <cstdint>

namespace xe {
namespace kernel {
namespace xam {

// Helper struct to hold network adapter info
struct AdapterInfo {
  bool found;
  uint32_t ip_addr;    // Network byte order
  uint8_t mac_addr[6];
  uint64_t link_speed;  // bits per second
};

// Query the first active network adapter's info
AdapterInfo QueryActiveAdapter();

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_NET_UTILS_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2023 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_UPNP_H_
#define XENIA_KERNEL_UPNP_H_

#include <map>
#include <shared_mutex>

#include "xenia/base/platform.h"
#include "xenia/base/threading_timer_queue.h"

#if XE_PLATFORM_WIN32
#include <third_party/miniupnp/miniupnpc/include/miniupnpc.h>
#endif

namespace xe {
namespace kernel {

class UPnP {
#if XE_PLATFORM_WIN32
 public:
  UPnP();
  ~UPnP();

  void Initialize();

  void SearchUPnP();

  bool is_active() const { return active_; }

  // internal port is in BE notation.
  uint32_t AddPort(std::string_view addr, uint16_t internal_port,
                   std::string_view protocol);

  // internal port is in BE notation.
  void RemovePort(uint16_t internal_port, std::string_view protocol);

  void RefreshPorts(std::string_view addr);

  void AddMappedConnectPort(uint16_t port, uint16_t mapped_port) {
    mapped_connect_ports_.insert({port, mapped_port});
  }

  void AddMappedBindPort(uint16_t port, uint16_t mapped_port) {
    mapped_bind_ports_.insert({port, mapped_port});
  }

  uint16_t GetMappedConnectPort(uint16_t port);

  uint16_t GetMappedBindPort(uint16_t external_port);

  std::map<std::string, std::map<uint16_t, int32_t>>* port_binding_results() {
    return &port_binding_results_;
  };

  const bool GetRefreshedUnauthorized() const;

  void SetRefreshedUnauthorized(const bool refreshed);

  static const std::string GetLocalIP();

 private:
  // https://openconnectivity.org/developer/specifications/upnp-resources/upnp/internet-gateway-device-igd-v-2-0/
  // http://upnp.org/specs/gw/UPnP-gw-WANIPConnection-v2-Service.pdf
  enum UPnPErrorCodes : int { OnlyPermanentLeasesSupported = 725 };

  typedef std::map<uint16_t, uint16_t> port_binding;

  void RemovePortExternal(uint16_t external_port, std::string_view protocol,
                          bool verbose = true);
  void RefreshPortsTimer();

  bool LoadSavedUPnPDevice();
  const UPNPDev* DiscoverUPnPDevice();
  const UPNPDev* GetDeviceByName(const UPNPDev* device_list,
                                 std::string device_name);
  bool GetAndParseUPnPXmlData(std::string url);

  std::shared_mutex mutex_;
  std::atomic<bool> active_ = false;
  std::atomic<bool> leases_supported_ = true;
  std::atomic<bool> refreshed_unauthorized_ = false;

#if XE_PLATFORM_WIN32
  IGDdatas* igd_data_ = new IGDdatas();
  UPNPUrls* igd_urls_ = new UPNPUrls();
#elif XE_PLATFORM_LINUX
  void* igd_data_ = nullptr;
  void* igd_urls_ = nullptr;
#endif

  std::weak_ptr<xe::threading::TimerQueueWaitItem> wait_item_;

  std::map<std::string, port_binding> port_bindings_;
  std::map<std::string, std::map<uint16_t, int32_t>> port_binding_results_;

  port_binding mapped_connect_ports_;
  port_binding mapped_bind_ports_;

#elif XE_PLATFORM_LINUX
 public:
  UPnP();
  ~UPnP();
  void Initialize() {}
  void SearchUPnP() {}
  bool is_active() const { return false; }
  uint32_t AddPort(std::string_view addr, uint16_t internal_port,
                   std::string_view protocol) {
    return 0;
  }
  void RemovePort(uint16_t internal_port, std::string_view protocol) {}
  void RefreshPorts(std::string_view addr) {}
  void AddMappedConnectPort(uint16_t port, uint16_t mapped_port) {}
  void AddMappedBindPort(uint16_t port, uint16_t mapped_port) {}
  uint16_t GetMappedConnectPort(uint16_t port) { return port; }
  uint16_t GetMappedBindPort(uint16_t external_port) { return external_port; }
  std::map<std::string, std::map<uint16_t, int32_t>>* port_binding_results() {
    static std::map<std::string, std::map<uint16_t, int32_t>> empty;
    return &empty;
  }
  const bool GetRefreshedUnauthorized() const { return false; }
  void SetRefreshedUnauthorized(const bool refreshed) {}
  static const std::string GetLocalIP() { return ""; }
#else
#error "UPnP not implemented for this platform"
#endif  // XE_PLATFORM_WIN32
};
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_UPNP_H_

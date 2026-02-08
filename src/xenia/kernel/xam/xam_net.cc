/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/net_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_error.h"
#include "xenia/kernel/xboxkrnl/xboxkrnl_threading.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xsocket.h"
#include "xenia/kernel/xthread.h"
#include "xenia/xbox.h"

// Platform-specific headers for QueryActiveAdapter()
#ifdef XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#include <iphlpapi.h>  // NOLINT(build/include_order)
#include <winsock2.h>  // NOLINT(build/include_order)
#elif XE_PLATFORM_LINUX
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

DEFINE_string(
    systemlink_interface, "",
    "Name of the network interface to use for System Link (e.g., \"eth0\", "
    "\"Ethernet\", \"Wi-Fi\"). When empty, the first active adapter is used.",
    "Network");

namespace xe {
namespace kernel {
namespace xam {

// Network initialization state tracking
// XNetGetTitleXnAddr returns PENDING until XNetStartup/WSAStartup is called
namespace {
std::mutex g_xnet_init_mutex;
bool g_xnet_initialized = false;
}  // namespace

// Connection state tracking for XNetConnect/XNetGetConnectStatus
// Returns PENDING initially, then CONNECTED after a delay
namespace {
std::mutex g_xnet_connection_mutex;
std::map<uint32_t, std::chrono::steady_clock::time_point> g_xnet_connections;
constexpr auto kXNetConnectDelay = std::chrono::milliseconds(500);
}  // namespace

// System link session tracking for discovery
// When host creates a session, we store full session info here
// https://github.com/G91/TitanOffLine/blob/1e692d9bb9dfac386d08045ccdadf4ae3227bb5e/xkelib/xam/xamNet.h
enum {
  XNCALLER_INVALID = 0x0,
  XNCALLER_TITLE = 0x1,
  XNCALLER_SYSAPP = 0x2,
  XNCALLER_XBDM = 0x3,
  XNCALLER_TEST = 0x4,
  NUM_XNCALLER_TYPES = 0x4,
};

// https://github.com/pmrowla/hl2sdk-csgo/blob/master/common/xbox/xboxstubs.h
typedef struct {
  // FYI: IN_ADDR should be in network-byte order.
  in_addr ina;                   // IP address (zero if not static/DHCP)
  in_addr inaOnline;             // Online IP address (zero if not online)
  xe::be<uint16_t> wPortOnline;  // Online port
  uint8_t abEnet[6];             // Ethernet MAC address
  uint8_t abOnline[20];          // Online identification
} XNADDR;

struct XNDNS {
  xe::be<int32_t> status;
  xe::be<uint32_t> cina;
  in_addr aina[8];
};
static_assert_size(XNDNS, 0x28);

struct XNQOSINFO {
  uint8_t flags;
  uint8_t reserved;
  xe::be<uint16_t> probes_xmit;
  xe::be<uint16_t> probes_recv;
  xe::be<uint16_t> data_len;
  xe::be<uint32_t> data_ptr;
  xe::be<uint16_t> rtt_min_in_msecs;
  xe::be<uint16_t> rtt_med_in_msecs;
  xe::be<uint32_t> up_bits_per_sec;
  xe::be<uint32_t> down_bits_per_sec;
};
static_assert_size(XNQOSINFO, 0x18);

struct XNQOS {
  xe::be<uint32_t> count;
  xe::be<uint32_t> count_pending;
  XNQOSINFO info[1];
};

struct Xsockaddr_t {
  xe::be<uint16_t> sa_family;
  char sa_data[14];
};
static_assert_size(XNQOS, 0x20);

struct X_WSADATA {
  xe::be<uint16_t> version;
  xe::be<uint16_t> version_high;
  char description[256 + 1];
  char system_status[128 + 1];
  xe::be<uint16_t> max_sockets;
  xe::be<uint16_t> max_udpdg;
  xe::be<uint32_t> vendor_info_ptr;
};
static_assert_size(X_WSADATA, 0x190);

struct XWSABUF {
  xe::be<uint32_t> len;
  xe::be<uint32_t> buf_ptr;
};

// XWSAOVERLAPPED is defined in xsocket.h (xe::kernel::XWSAOVERLAPPED)
using xe::kernel::XWSAOVERLAPPED;

void LoadSockaddr(const uint8_t* ptr, sockaddr* out_addr) {
  out_addr->sa_family = xe::load_and_swap<uint16_t>(ptr + 0);
  switch (out_addr->sa_family) {
    case AF_INET: {
      auto in_addr = reinterpret_cast<sockaddr_in*>(out_addr);
      in_addr->sin_port = xe::load_and_swap<uint16_t>(ptr + 2);
      // Maybe? Depends on type.
      in_addr->sin_addr.s_addr = *(uint32_t*)(ptr + 4);
      break;
    }
    default:
      assert_unhandled_case(out_addr->sa_family);
      break;
  }
}

void StoreSockaddr(const sockaddr& addr, uint8_t* ptr) {
  switch (addr.sa_family) {
    case AF_UNSPEC:
      std::memset(ptr, 0, sizeof(addr));
      break;
    case AF_INET: {
      auto& in_addr = reinterpret_cast<const sockaddr_in&>(addr);
      xe::store_and_swap<uint16_t>(ptr + 0, in_addr.sin_family);
      xe::store_and_swap<uint16_t>(ptr + 2, in_addr.sin_port);
      // Maybe? Depends on type.
      xe::store_and_swap<uint32_t>(ptr + 4, in_addr.sin_addr.s_addr);
      break;
    }
    default:
      assert_unhandled_case(addr.sa_family);
      break;
  }
}

// https://github.com/joolswills/mameox/blob/master/MAMEoX/Sources/xbox_Network.cpp#L136
struct XNetStartupParams {
  uint8_t cfgSizeOfStruct;
  uint8_t cfgFlags;
  uint8_t cfgSockMaxDgramSockets;
  uint8_t cfgSockMaxStreamSockets;
  uint8_t cfgSockDefaultRecvBufsizeInK;
  uint8_t cfgSockDefaultSendBufsizeInK;
  uint8_t cfgKeyRegMax;
  uint8_t cfgSecRegMax;
  uint8_t cfgQosDataLimitDiv4;
  uint8_t cfgQosProbeTimeoutInSeconds;
  uint8_t cfgQosProbeRetries;
  uint8_t cfgQosSrvMaxSimultaneousResponses;
  uint8_t cfgQosPairWaitTimeInSeconds;
};
static_assert_size(XNetStartupParams, 0xD);

XNetStartupParams xnet_startup_params = {0};

// Query a network adapter's info. If systemlink_interface cvar is set, only
// that interface is matched. Otherwise, the first active ethernet/wifi adapter.
AdapterInfo QueryActiveAdapter() {
  AdapterInfo info = {};
  const std::string& preferred = cvars::systemlink_interface;

#ifdef XE_PLATFORM_WIN32
  ULONG buffer_size = 0;
  GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                       nullptr, nullptr, &buffer_size);
  if (buffer_size > 0) {
    std::vector<uint8_t> buffer(buffer_size);
    auto adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                             nullptr, adapters, &buffer_size) == NO_ERROR) {
      for (auto adapter = adapters; adapter != nullptr;
           adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp) continue;
        if (adapter->FirstUnicastAddress == nullptr) continue;

        // If user specified an interface, match by FriendlyName
        if (!preferred.empty()) {
          // Compare FriendlyName (wide) with preferred (narrow)
          // Interface names are typically ASCII, so simple comparison works
          const wchar_t* friendly = adapter->FriendlyName;
          bool match = true;
          for (size_t i = 0; i < preferred.size(); ++i) {
            if (friendly[i] == L'\0' ||
                friendly[i] != static_cast<wchar_t>(preferred[i])) {
              match = false;
              break;
            }
          }
          if (!match || friendly[preferred.size()] != L'\0') continue;
        } else {
          // Default: only match ethernet or wifi adapters
          if (adapter->IfType != IF_TYPE_ETHERNET_CSMACD &&
              adapter->IfType != IF_TYPE_IEEE80211) {
            continue;
          }
        }

        auto unicast = adapter->FirstUnicastAddress;
        auto sockaddr =
            reinterpret_cast<sockaddr_in*>(unicast->Address.lpSockaddr);
        if (sockaddr->sin_family == AF_INET) {
          info.found = true;
          info.ip_addr = sockaddr->sin_addr.s_addr;
          info.link_speed = adapter->TransmitLinkSpeed;
          if (adapter->PhysicalAddressLength == 6) {
            std::memcpy(info.mac_addr, adapter->PhysicalAddress, 6);
          }
          XELOGI("QueryActiveAdapter: using interface '{}', ip={:08X}",
                 preferred.empty() ? "(auto)" : preferred, info.ip_addr);
          break;
        }
      }
    }
  }
#elif XE_PLATFORM_LINUX
  struct ifaddrs* ifaddr;
  if (getifaddrs(&ifaddr) == 0) {
    for (auto ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr) continue;
      if (ifa->ifa_addr->sa_family != AF_INET) continue;
      if (ifa->ifa_flags & IFF_LOOPBACK) continue;
      if (!(ifa->ifa_flags & IFF_UP) || !(ifa->ifa_flags & IFF_RUNNING)) {
        continue;
      }

      // If user specified an interface, match by name
      if (!preferred.empty() && preferred != ifa->ifa_name) {
        continue;
      }

      info.found = true;
      info.ip_addr =
          reinterpret_cast<sockaddr_in*>(ifa->ifa_addr)->sin_addr.s_addr;
      info.link_speed = 100000000;  // Assume 100Mbps

      // Get MAC address via ioctl
      int sock = socket(AF_INET, SOCK_DGRAM, 0);
      if (sock >= 0) {
        struct ifreq ifr = {};
        strncpy(ifr.ifr_name, ifa->ifa_name, IFNAMSIZ - 1);
        if (ioctl(sock, SIOCGIFHWADDR, &ifr) == 0) {
          std::memcpy(info.mac_addr, ifr.ifr_hwaddr.sa_data, 6);
        }
        close(sock);
      }
      XELOGI("QueryActiveAdapter: using interface '{}', ip={:08X}",
             ifa->ifa_name, info.ip_addr);
      break;
    }
    freeifaddrs(ifaddr);
  }
#endif

  if (!info.found && !preferred.empty()) {
    XELOGE("QueryActiveAdapter: interface '{}' not found!", preferred);
  }

  return info;
}

dword_result_t NetDll_XNetStartup_entry(dword_t caller,
                                        pointer_t<XNetStartupParams> params) {
  XELOGI("NetDll_XNetStartup: caller={}, params={:08X}", caller.value(),
         params.guest_address());

  if (params) {
    assert_true(params->cfgSizeOfStruct == sizeof(XNetStartupParams));
    std::memcpy(&xnet_startup_params, params, sizeof(XNetStartupParams));
  }

  {
    std::lock_guard<std::mutex> lock(g_xnet_init_mutex);
    g_xnet_initialized = true;
  }

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");

  /*
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartup, kNetworking, kImplemented);

// https://github.com/jogolden/testdev/blob/master/xkelib/syssock.h#L46
dword_result_t NetDll_XNetStartupEx_entry(dword_t caller,
                                          pointer_t<XNetStartupParams> params,
                                          dword_t versionReq) {
  // versionReq
  // MW3, Ghosts: 0x20501400

  return NetDll_XNetStartup_entry(caller, params);
}
DECLARE_XAM_EXPORT1(NetDll_XNetStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_XNetCleanup_entry(dword_t caller, lpvoid_t params) {
  XELOGI("NetDll_XNetCleanup");

  // Reset initialization state
  {
    std::lock_guard<std::mutex> lock(g_xnet_init_mutex);
    g_xnet_initialized = false;
  }

  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  // auto xnet = xam->xnet();
  // xam->set_xnet(nullptr);

  // TODO: Shut down and delete.
  // delete xnet;

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCleanup, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetOpt_entry(dword_t one, dword_t option_id,
                                       lpvoid_t buffer_ptr,
                                       lpdword_t buffer_size) {
  assert_true(one == 1);
  switch (option_id) {
    case 1:
      if (*buffer_size < sizeof(XNetStartupParams)) {
        *buffer_size = sizeof(XNetStartupParams);
        return uint32_t(X_WSA_ERROR::X_WSAEMSGSIZE);
      }
      std::memcpy(buffer_ptr, &xnet_startup_params, sizeof(XNetStartupParams));
      XELOGI("XNetGetOpt: option={}, size={}", option_id.value(),
             static_cast<uint32_t>(*buffer_size));
      return 0;
    default:
      XELOGE("NetDll_XNetGetOpt: option {} unimplemented",
             static_cast<uint32_t>(option_id));
      return uint32_t(X_WSA_ERROR::X_WSAEINVAL);
  }
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetOpt, kNetworking, kSketchy);

dword_result_t NetDll_XNetRandom_entry(dword_t caller, lpvoid_t buffer_ptr,
                                       dword_t length) {
  uint8_t* buffer_data_ptr = buffer_ptr.as<uint8_t*>();

  if (buffer_data_ptr == nullptr || length == 0) {
    return X_ERROR_SUCCESS;
  }

  std::random_device rnd;
  std::mt19937_64 gen(rnd());
  std::uniform_int_distribution<int> dist(0,
                                          std::numeric_limits<uint8_t>::max());

  std::generate(buffer_data_ptr, buffer_data_ptr + length,
                [&]() { return static_cast<uint8_t>(dist(gen)); });

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetRandom, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartup_entry(dword_t caller, word_t version,
                                       pointer_t<X_WSADATA> data_ptr) {
  // TODO(benvanik): abstraction layer needed.
  XELOGI("NetDll_WSAStartup: version={:04X}, data_ptr={:08X}", version.value(),
         data_ptr.guest_address());

  int ret = 0;

#ifdef XE_PLATFORM_WIN32
  WSADATA wsaData = {};

  ret = WSAStartup(version, &wsaData);
#endif

  if (data_ptr) {
    auto data_out = kernel_state()->memory()->TranslateVirtual(data_ptr);

#ifdef XE_PLATFORM_WIN32
    data_ptr->version = wsaData.wVersion;
    data_ptr->version_high = wsaData.wHighVersion;
#else
    data_ptr->version = version.value();
    data_ptr->version_high = 0x0202;
#endif

    // Some games (5841099F) want this value round-tripped - they'll compare if
    // it changes and bugcheck if it does.
    // vendor_info_ptr is at offset 0x18A (after max_udpdg at 0x188)
    uint32_t vendor_ptr = xe::load_and_swap<uint32_t>(data_out + 0x18A);
    xe::store_and_swap<uint32_t>(data_out + 0x18A, vendor_ptr);
  }

  // DEBUG
  /*
  auto xam = kernel_state()->GetKernelModule<XamModule>("xam.xex");
  if (!xam->xnet()) {
    auto xnet = new XNet(kernel_state());
    xnet->Initialize();

    xam->set_xnet(xnet);
  }
  */

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartup, kNetworking, kImplemented);

dword_result_t NetDll_WSAStartupEx_entry(dword_t caller, word_t version,
                                         pointer_t<X_WSADATA> data_ptr,
                                         dword_t versionReq) {
  return NetDll_WSAStartup_entry(caller, version, data_ptr);
}
DECLARE_XAM_EXPORT1(NetDll_WSAStartupEx, kNetworking, kImplemented);

dword_result_t NetDll_WSACleanup_entry(dword_t caller) {
  // This does nothing. Xenia needs WSA running.
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_WSACleanup, kNetworking, kImplemented);

// Instead of using dedicated storage for WSA error like on OS.
// Xbox shares space between normal error codes and WSA errors.
// This under the hood returns directly value received from RtlGetLastError.
dword_result_t NetDll_WSAGetLastError_entry() {
  return XThread::GetLastError();
}
DECLARE_XAM_EXPORT1(NetDll_WSAGetLastError, kNetworking, kImplemented);

dword_result_t NetDll_WSARecvFrom_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers_ptr,
    dword_t buffer_count, lpdword_t num_bytes_recv, lpdword_t flags_ptr,
    pointer_t<XSOCKADDR_IN> from_ptr, lpdword_t fromlen_ptr,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpvoid_t completion_routine_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  // Skip logging repeated polls on already-pending overlapped
  if (!overlapped_ptr || !socket->IsOverlappedPending(overlapped_ptr)) {
    XELOGI("NetDll_WSARecvFrom: socket={:08X}, buffers={}, overlapped={:08X}",
           socket_handle.value(), buffer_count.value(),
           overlapped_ptr.guest_address());
  }

  if (buffer_count == 0 || !buffers_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEINVAL));
    return -1;
  }

  // Compute total receive buffer size across all WSABUF entries.
  // For scatter-gather recv, we receive into a single contiguous buffer
  // (the first one, which must be large enough) and then scatter the data
  // into the individual guest buffers on completion.
  // For VDP sockets the game may pass multiple buffers (header + data).
  uint32_t total_buf_len = 0;
  for (uint32_t i = 0; i < buffer_count; i++) {
    total_buf_len += buffers_ptr[i].len;
  }

  // Use the first buffer for receive — it points to guest memory that will
  // hold the data. For single-buffer case this is the only buffer. For
  // multi-buffer case, the XSocket layer receives into its own temp buffer
  // and scatters on completion.
  uint8_t* buf = kernel_memory()->TranslateVirtual(buffers_ptr->buf_ptr.get());
  uint32_t buf_len = buffers_ptr->len;
  uint32_t flags = flags_ptr ? flags_ptr.value() : 0;

  // Build scatter list for multi-buffer receives
  std::vector<std::pair<uint8_t*, uint32_t>> scatter_buffers;
  if (buffer_count > 1) {
    for (uint32_t i = 0; i < buffer_count; i++) {
      uint8_t* b =
          kernel_memory()->TranslateVirtual(buffers_ptr[i].buf_ptr.get());
      scatter_buffers.push_back({b, buffers_ptr[i].len});
    }
    // Use total length so ASIO receives the full packet
    buf_len = total_buf_len;
  }

  // WSARecvFrom takes XSOCKADDR_IN* (guest big-endian format) directly.
  // For async, the from pointer must point to guest memory that persists
  // until the overlapped completes — from_ptr already does this.
  // For sync, WSARecvFrom handles N_XSOCKADDR_IN conversion internally.
  XSOCKADDR_IN* from_guest_ptr =
      from_ptr ? static_cast<XSOCKADDR_IN*>(from_ptr) : nullptr;

  // Pre-fill fromlen in guest memory for async (avoids stack lifetime issue)
  if (overlapped_ptr && fromlen_ptr && from_ptr) {
    *fromlen_ptr = sizeof(XSOCKADDR_IN);
  }

  int ret = socket->WSARecvFrom(buf, buf_len, flags, from_guest_ptr,
                                overlapped_ptr, std::move(scatter_buffers));

  if (ret >= 0) {
    // Synchronous completion
    if (num_bytes_recv) {
      *num_bytes_recv = ret;
    }
    if (fromlen_ptr) {
      *fromlen_ptr = sizeof(XSOCKADDR_IN);
    }
    return 0;
  } else {
    uint32_t err = socket->XWSAGetLastError();
    XThread::SetLastError(err);
    if (err != uint32_t(X_WSA_ERROR::X_WSA_IO_PENDING)) {
      XELOGI("NetDll_WSARecvFrom: returned -1, error={}", err);
    }
    return -1;
  }
}
DECLARE_XAM_EXPORT2(NetDll_WSARecvFrom, kNetworking, kImplemented,
                    kHighFrequency);

// If the socket is a VDP socket, buffer 0 is the game data length, and buffer 1
// is the unencrypted game data.
dword_result_t NetDll_WSASendTo_entry(
    dword_t caller, dword_t socket_handle, pointer_t<XWSABUF> buffers,
    dword_t num_buffers, lpdword_t num_bytes_sent, dword_t flags,
    pointer_t<XSOCKADDR_IN> to_ptr, dword_t to_len,
    pointer_t<XWSAOVERLAPPED> overlapped, lpvoid_t completion_routine) {
  if (to_ptr) {
    N_XSOCKADDR_IN native_to_log(to_ptr);
    auto dest = asio::ip::address_v4(
        static_cast<uint32_t>(native_to_log.sin_addr));
    uint16_t dest_port = static_cast<uint16_t>(native_to_log.sin_port);
    // Log buffer structure and first bytes of each buffer for VDP debugging
    std::string buf_info;
    for (uint32_t i = 0; i < num_buffers; i++) {
      uint8_t* bdata =
          kernel_memory()->TranslateVirtual(buffers[i].buf_ptr.get());
      uint32_t blen = buffers[i].len;
      buf_info += fmt::format(" buf[{}]={} bytes [", i, blen);
      for (uint32_t j = 0; j < std::min(blen, 16u); j++) {
        buf_info += fmt::format("{:02X}", bdata[j]);
        if (j < std::min(blen, 16u) - 1) buf_info += " ";
      }
      buf_info += "]";
    }
    XELOGI(
        "NetDll_WSASendTo: socket={:08X}, {} bufs to {}:{}, "
        "overlapped={:08X}{}",
        socket_handle.value(), num_buffers.value(), dest.to_string(),
        dest_port, overlapped.guest_address(), buf_info);
  } else {
    XELOGI("NetDll_WSASendTo: socket={:08X}, overlapped={:08X}",
           socket_handle.value(), overlapped.guest_address());
  }

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  // Combine multiple buffers into a single buffer
  std::vector<uint8_t> combined_buffer_mem;
  uint32_t combined_buffer_size = 0;
  uint32_t combined_buffer_offset = 0;
  for (uint32_t i = 0; i < num_buffers; i++) {
    combined_buffer_size += buffers[i].len;
    combined_buffer_mem.resize(combined_buffer_size);

    std::memcpy(combined_buffer_mem.data() + combined_buffer_offset,
                kernel_memory()->TranslateVirtual(buffers[i].buf_ptr),
                buffers[i].len);
    combined_buffer_offset += buffers[i].len;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  int ret = socket->WSASendTo(combined_buffer_mem.data(), combined_buffer_size,
                              flags, &native_to, to_len, overlapped);

  if (ret >= 0) {
    if (num_bytes_sent) {
      *num_bytes_sent = ret;
    }
    XELOGI("NetDll_WSASendTo: sync complete, {} bytes sent", ret);
    return 0;
  } else {
    uint32_t err = socket->XWSAGetLastError();
    XThread::SetLastError(err);
    XELOGI("NetDll_WSASendTo: returned -1, error={}", err);
    return -1;
  }
}
DECLARE_XAM_EXPORT1(NetDll_WSASendTo, kNetworking, kImplemented);

dword_result_t NetDll_WSAWaitForMultipleEvents_entry(dword_t num_events,
                                                     lpdword_t events,
                                                     dword_t wait_all,
                                                     dword_t timeout,
                                                     dword_t alertable) {
  if (num_events > 64) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSA_INVALID_PARAMETER));
    return ~0u;
  }

  uint64_t timeout_wait = (uint64_t)timeout;

  X_STATUS result = 0;
  do {
    result = xboxkrnl::xeNtWaitForMultipleObjectsEx(
        num_events, events, wait_all, 1, alertable,
        timeout != -1 ? &timeout_wait : nullptr);
  } while (result == X_STATUS_ALERTED);

  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return ~0u;
  }
  return 0;
}
DECLARE_XAM_EXPORT2(NetDll_WSAWaitForMultipleEvents, kNetworking, kImplemented,
                    kBlocking);

dword_result_t NetDll_WSACreateEvent_entry() {
  XEvent* ev = new XEvent(kernel_state());
  ev->Initialize(true, false);
  return ev->handle();
}
DECLARE_XAM_EXPORT1(NetDll_WSACreateEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSACloseEvent_entry(dword_t event_handle) {
  X_STATUS result = kernel_state()->object_table()->ReleaseHandle(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSACloseEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSAResetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtClearEvent(event_handle);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSAResetEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSASetEvent_entry(dword_t event_handle) {
  X_STATUS result = xboxkrnl::xeNtSetEvent(event_handle, nullptr);
  if (XFAILED(result)) {
    uint32_t error = xboxkrnl::xeRtlNtStatusToDosError(result);
    XThread::SetLastError(error);
    return 0;
  }
  return 1;
}
DECLARE_XAM_EXPORT1(NetDll_WSASetEvent, kNetworking, kImplemented);

dword_result_t NetDll_WSAEventSelect_entry(dword_t caller,
                                           dword_t socket_handle,
                                           dword_t event_handle,
                                           dword_t network_events) {
  XELOGI("NetDll_WSAEventSelect: socket={:08X}, event={:08X}, events={:08X}",
         socket_handle.value(), event_handle.value(), network_events.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  // For now, just return success - actual event-based IO would need more work
  // The socket is already in non-blocking mode if games call this
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_WSAEventSelect, kNetworking, kImplemented);

dword_result_t NetDll_WSAGetOverlappedResult_entry(
    dword_t caller, dword_t socket_handle,
    pointer_t<XWSAOVERLAPPED> overlapped_ptr, lpdword_t bytes_transferred,
    dword_t wait, lpdword_t flags_ptr) {
  XELOGI("NetDll_WSAGetOverlappedResult: socket={:08X}, overlapped={:08X}",
         socket_handle.value(), overlapped_ptr.guest_address());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return 0;  // FALSE
  }

  if (!overlapped_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEINVAL));
    return 0;
  }

  // Check if the operation is still pending
  bool is_pending = socket->IsOverlappedPending(overlapped_ptr);

  if (is_pending) {
    if (wait && overlapped_ptr->event_handle) {
      // Wait for the operation to complete
      auto ev = kernel_state()->object_table()->LookupObject<XEvent>(
          overlapped_ptr->event_handle);
      if (ev) {
        ev->Wait(0, 0, true, nullptr);
      }
      // After wait, operation should be complete
    } else {
      // Operation still pending and caller doesn't want to wait.
      // Yield to give the ASIO io_context thread time to process incoming
      // packets. Without this, tight game poll loops complete in microseconds
      // while the network round trip takes milliseconds, causing timeouts.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSA_IO_INCOMPLETE));
      return 0;  // FALSE
    }
  }

  // Windows OVERLAPPED convention:
  //   Internal = NTSTATUS (0 = success, nonzero = error)
  //   InternalHigh = bytes transferred
  if (bytes_transferred) {
    *bytes_transferred = overlapped_ptr->internal_high;
  }

  // Check if there was an error (Internal holds the NTSTATUS)
  if (overlapped_ptr->internal != 0) {
    XThread::SetLastError(overlapped_ptr->internal);
    return 0;  // FALSE
  }

  return 1;  // TRUE - success
}
DECLARE_XAM_EXPORT1(NetDll_WSAGetOverlappedResult, kNetworking, kImplemented);

dword_result_t NetDll_WSACancelOverlappedIO_entry(dword_t caller,
                                                   dword_t socket_handle) {
  XELOGI("NetDll_WSACancelOverlappedIO: socket={:08X}", socket_handle.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  socket->WSACancelOverlappedIO();
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_WSACancelOverlappedIO, kNetworking, kImplemented);

struct XnAddrStatus {
  // Address acquisition is not yet complete
  static constexpr uint32_t XNET_GET_XNADDR_PENDING = 0x00000000;
  // XNet is uninitialized or no debugger found
  static constexpr uint32_t XNET_GET_XNADDR_NONE = 0x00000001;
  // Host has ethernet address (no IP address)
  static constexpr uint32_t XNET_GET_XNADDR_ETHERNET = 0x00000002;
  // Host has statically assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_STATIC = 0x00000004;
  // Host has DHCP assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_DHCP = 0x00000008;
  // Host has PPPoE assigned IP address
  static constexpr uint32_t XNET_GET_XNADDR_PPPOE = 0x00000010;
  // Host has one or more gateways configured
  static constexpr uint32_t XNET_GET_XNADDR_GATEWAY = 0x00000020;
  // Host has one or more DNS servers configured
  static constexpr uint32_t XNET_GET_XNADDR_DNS = 0x00000040;
  // Host is currently connected to online service
  static constexpr uint32_t XNET_GET_XNADDR_ONLINE = 0x00000080;
  // Network configuration requires troubleshooting
  static constexpr uint32_t XNET_GET_XNADDR_TROUBLESHOOT = 0x00008000;
};

// Derive a deterministic MAC address from an IP address.
// This ensures both peers compute the same XNADDR for each other.
void DeriveEnetFromIP(uint32_t ip_network_order, uint8_t* enet_out) {
  // Use a recognizable OUI prefix (00:50:XX) followed by IP octets
  // IP is in network byte order (big-endian), so we can extract directly
  uint8_t* ip_bytes = reinterpret_cast<uint8_t*>(&ip_network_order);
  enet_out[0] = 0x00;
  enet_out[1] = 0x50;
  enet_out[2] = ip_bytes[0];
  enet_out[3] = ip_bytes[1];
  enet_out[4] = ip_bytes[2];
  enet_out[5] = ip_bytes[3];
}

dword_result_t NetDll_XNetGetTitleXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> addr_ptr) {
  addr_ptr.Zero();

  // Return PENDING until XNetStartup/WSAStartup has been called
  {
    std::lock_guard<std::mutex> lock(g_xnet_init_mutex);
    if (!g_xnet_initialized) {
      return XnAddrStatus::XNET_GET_XNADDR_PENDING;
    }
  }

  auto adapter = QueryActiveAdapter();

  if (!adapter.found) {
    XELOGE("XNetGetTitleXnAddr: no network adapter found");
    return XnAddrStatus::XNET_GET_XNADDR_NONE;
  }

  addr_ptr->ina.s_addr = adapter.ip_addr;
  std::memcpy(addr_ptr->abEnet, adapter.mac_addr, 6);

  // Match netplay XBOXLIVE mode: game needs ONLINE status to include QoS data
  addr_ptr->inaOnline.s_addr = adapter.ip_addr;
  addr_ptr->wPortOnline = 36000;
  addr_ptr->abOnline[16] = 1;  // platform_type = Xbox360

  uint32_t status = XnAddrStatus::XNET_GET_XNADDR_ETHERNET |
                    XnAddrStatus::XNET_GET_XNADDR_STATIC |
                    XnAddrStatus::XNET_GET_XNADDR_GATEWAY |
                    XnAddrStatus::XNET_GET_XNADDR_DNS |
                    XnAddrStatus::XNET_GET_XNADDR_ONLINE;
  XELOGI("XNetGetTitleXnAddr: ip={:08X}, MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}, status={:08X}",
         addr_ptr->ina.s_addr,
         addr_ptr->abEnet[0], addr_ptr->abEnet[1], addr_ptr->abEnet[2],
         addr_ptr->abEnet[3], addr_ptr->abEnet[4], addr_ptr->abEnet[5],
         status);

  return status;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetTitleXnAddr, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetDebugXnAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> addr_ptr) {
  XELOGI("XNetGetDebugXnAddr: returning NONE");
  addr_ptr.Zero();

  // XNET_GET_XNADDR_NONE causes caller to gracefully return.
  return XnAddrStatus::XNET_GET_XNADDR_NONE;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetDebugXnAddr, kNetworking, kStub);

dword_result_t NetDll_XNetGetXnAddrPlatform_entry(dword_t caller,
                                                  pointer_t<XNADDR> addr_ptr,
                                                  lpdword_t platform_type) {
  *platform_type = 1;
  XELOGI("XNetGetXnAddrPlatform: addr={:08X}, platform=1 (Xbox360)",
         addr_ptr.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetXnAddrPlatform, kNetworking, kStub);

dword_result_t NetDll_XNetXnAddrToMachineId_entry(dword_t caller,
                                                  pointer_t<XNADDR> addr_ptr,
                                                  lpqword_t id_ptr) {
  if (!addr_ptr || !id_ptr) {
    return X_E_INVALIDARG;
  }

  id_ptr.Zero();

  // Convert 6-byte MAC (abEnet) to uint64_t, same as netplay's MacAddress::to_uint64()
  uint64_t mac_uint64 = 0;
  for (int i = 0; i < 6; ++i) {
    mac_uint64 = (mac_uint64 << 8) | addr_ptr->abEnet[i];
  }

  // Apply machine ID mask, consistent with netplay's GetMachineId()
  const uint64_t machine_id_mask = 0xFA00000000000000;
  uint64_t machine_id = machine_id_mask | mac_uint64;

  *id_ptr = machine_id;

  XELOGI(
      "XNetXnAddrToMachineId: MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}, "
      "machine_id={:016X}",
      addr_ptr->abEnet[0], addr_ptr->abEnet[1], addr_ptr->abEnet[2],
      addr_ptr->abEnet[3], addr_ptr->abEnet[4], addr_ptr->abEnet[5],
      machine_id);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToMachineId, kNetworking, kImplemented);

dword_result_t XNetLogonGetTitleID_entry(dword_t caller, lpvoid_t params) {
  return kernel_state()->title_id();
}
DECLARE_XAM_EXPORT1(XNetLogonGetTitleID, kNetworking, kImplemented);

dword_result_t XNetLogonGetMachineID_entry(dword_t caller,
                                           lpqword_t machine_id) {
  XELOGI("XNetLogonGetMachineID: machine_id={:08X}",
         machine_id.guest_address());

  if (machine_id) {
    auto adapter = QueryActiveAdapter();
    if (!adapter.found) {
      XELOGE("XNetLogonGetMachineID: no network adapter found");
      *machine_id = 0;
      return X_STATUS_UNSUCCESSFUL;
    }

    // Use real hardware MAC - consistent with XNetGetTitleXnAddr and XSessionCreate
    uint64_t mac_uint64 = 0;
    for (int i = 0; i < 6; ++i) {
      mac_uint64 = (mac_uint64 << 8) | adapter.mac_addr[i];
    }
    // Apply machine ID mask, consistent with netplay's GetMachineId()
    const uint64_t machine_id_mask = 0xFA00000000000000;
    *machine_id = machine_id_mask | mac_uint64;
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNetLogonGetMachineID, kNetworking, kImplemented);

void NetDll_XNetInAddrToString_entry(dword_t caller, dword_t in_addr,
                                     lpstring_t string_out,
                                     dword_t string_size) {
  if (string_out && string_size > 0) {
    // ASIO expects host byte order, in_addr is in network byte order
    asio::ip::address_v4 addr(ntohl(in_addr));
    std::string result = addr.to_string();
    strncpy(string_out, result.c_str(), string_size - 1);
    string_out[string_size - 1] = '\0';
  }
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToString, kNetworking, kImplemented);

// This converts a XNet address to an IN_ADDR. The IN_ADDR is used for
// subsequent socket calls (like a handle to a XNet address)
dword_result_t NetDll_XNetXnAddrToInAddr_entry(dword_t caller,
                                               pointer_t<XNADDR> xn_addr,
                                               lpvoid_t xid,
                                               pointer_t<in_addr> in_addr_ptr) {
  if (!xn_addr || !in_addr_ptr) {
    return X_E_INVALIDARG;
  }

  // For system link, just use the IP address directly from the XNADDR
  in_addr_ptr->s_addr = xn_addr->ina.s_addr;

  XELOGI(
      "XNetXnAddrToInAddr: xnaddr={:08X}, ina={:08X}, inaOnline={:08X}, "
      "result={:08X}",
      xn_addr.guest_address(), xn_addr->ina.s_addr, xn_addr->inaOnline.s_addr,
      in_addr_ptr->s_addr);

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetXnAddrToInAddr, kNetworking, kImplemented);

// Does the reverse of the above - converts an IN_ADDR to XNADDR
dword_result_t NetDll_XNetInAddrToXnAddr_entry(dword_t caller,
                                               dword_t in_addr_val,
                                               pointer_t<XNADDR> xn_addr,
                                               lpvoid_t xid) {
  if (xn_addr) {
    xn_addr.Zero();
    xn_addr->ina.s_addr = in_addr_val;

    // Derive MAC deterministically from IP so both peers agree on XNADDR
    DeriveEnetFromIP(in_addr_val, xn_addr->abEnet);

    XELOGI("XNetInAddrToXnAddr: ip={:08X} -> MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
           in_addr_val.value(),
           xn_addr->abEnet[0], xn_addr->abEnet[1], xn_addr->abEnet[2],
           xn_addr->abEnet[3], xn_addr->abEnet[4], xn_addr->abEnet[5]);
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetInAddrToXnAddr, kNetworking, kImplemented);

// Converts a server address (DNS name or IP) to IN_ADDR
dword_result_t NetDll_XNetServerToInAddr_entry(dword_t caller,
                                               dword_t in_addr_val,
                                               dword_t service_id,
                                               pointer_t<in_addr> in_addr_ptr) {
  XELOGI("XNetServerToInAddr: addr={:08X}, service={:08X}", in_addr_val.value(),
         service_id.value());

  if (!in_addr_ptr) {
    return X_E_INVALIDARG;
  }

  // For now, just pass through the address
  in_addr_ptr->s_addr = in_addr_val;

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetServerToInAddr, kNetworking, kImplemented);

// System link port - default is 3074
uint16_t g_system_link_port = 3074;

// https://www.google.com/patents/WO2008112448A1?cl=en
// Reserves a port for use by system link
dword_result_t NetDll_XNetSetSystemLinkPort_entry(dword_t caller,
                                                  dword_t port) {
  XELOGI("XNetSetSystemLinkPort: port={}", port.value());
  g_system_link_port = static_cast<uint16_t>(port);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetSetSystemLinkPort, kNetworking, kImplemented);

dword_result_t NetDll_XNetGetSystemLinkPort_entry(dword_t caller,
                                                  lpword_t port_ptr) {
  if (!port_ptr) {
    return uint32_t(X_WSA_ERROR::X_WSAEFAULT);
  }
  *port_ptr = g_system_link_port;
  XELOGI("XNetGetSystemLinkPort: port={}", g_system_link_port);
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetSystemLinkPort, kNetworking, kImplemented);

// https://github.com/ILOVEPIE/Cxbx-Reloaded/blob/master/src/CxbxKrnl/EmuXOnline.h#L39
struct XEthernetStatus {
  static constexpr uint32_t XNET_ETHERNET_LINK_ACTIVE = 0x01;
  static constexpr uint32_t XNET_ETHERNET_LINK_100MBPS = 0x02;
  static constexpr uint32_t XNET_ETHERNET_LINK_10MBPS = 0x04;
  static constexpr uint32_t XNET_ETHERNET_LINK_FULL_DUPLEX = 0x08;
  static constexpr uint32_t XNET_ETHERNET_LINK_HALF_DUPLEX = 0x10;
};

dword_result_t NetDll_XNetGetEthernetLinkStatus_entry(dword_t caller) {
  // Always report link active - consistent with netplay implementation
  // Games check this to determine if networking is available
  uint32_t status = XEthernetStatus::XNET_ETHERNET_LINK_ACTIVE |
                    XEthernetStatus::XNET_ETHERNET_LINK_100MBPS |
                    XEthernetStatus::XNET_ETHERNET_LINK_FULL_DUPLEX;
  XELOGI("XNetGetEthernetLinkStatus: returning {:08X}", status);
  return status;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetEthernetLinkStatus, kNetworking,
                    kImplemented);

dword_result_t NetDll_XNetGetBroadcastVersionStatus_entry(dword_t caller,
                                                          dword_t reset) {
  XELOGI("XNetGetBroadcastVersionStatus: reset={}", reset.value());
  return X_STATUS_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetBroadcastVersionStatus, kNetworking, kStub);

dword_result_t NetDll_XNetDnsLookup_entry(dword_t caller, lpstring_t host,
                                          dword_t event_handle,
                                          lpdword_t pdns) {
  XELOGI("NetDll_XNetDnsLookup: host={}", host ? host.value() : "(null)");

  if (pdns) {
    auto dns_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNDNS));
    auto dns = kernel_memory()->TranslateVirtual<XNDNS*>(dns_guest);
    std::memset(dns, 0, sizeof(XNDNS));

    if (host) {
      asio::error_code ec;
      asio::io_context io;
      asio::ip::tcp::resolver resolver(io);
      auto results = resolver.resolve(host.value(), "", ec);

      if (!ec && !results.empty()) {
        dns->status = 0;  // Success
        dns->cina = 0;
        for (const auto& entry : results) {
          if (dns->cina >= 8) break;
          auto addr = entry.endpoint().address();
          if (addr.is_v4()) {
            dns->aina[dns->cina].s_addr = htonl(addr.to_v4().to_uint());
            dns->cina++;
          }
        }
        XELOGI("NetDll_XNetDnsLookup: resolved {} addresses",
               (uint32_t)dns->cina);
      } else {
        dns->status = 1;  // Error
        XELOGI("NetDll_XNetDnsLookup: failed to resolve host");
      }
    } else {
      dns->status = 1;  // Error - no host provided
    }

    *pdns = dns_guest;
  }

  if (event_handle) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsLookup, kNetworking, kImplemented);

dword_result_t NetDll_XNetDnsRelease_entry(dword_t caller,
                                           pointer_t<XNDNS> dns) {
  XELOGI("XNetDnsRelease: dns={:08X}", dns.guest_address());
  if (!dns) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel_memory()->SystemHeapFree(dns.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetDnsRelease, kNetworking, kStub);

dword_result_t NetDll_XNetQosServiceLookup_entry(dword_t caller, dword_t flags,
                                                 dword_t event_handle,
                                                 lpdword_t pqos) {
  XELOGI("XNetQosServiceLookup: flags={:08X}, event={:08X}", flags.value(),
         event_handle.value());

  if (!pqos) {
    return static_cast<uint32_t>(X_WSA_ERROR::X_WSAEINVAL);
  }

  // Allocate and populate QoS structure with fake "good" results
  auto qos_guest = kernel_memory()->SystemHeapAlloc(sizeof(XNQOS));
  auto qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_guest);

  constexpr uint8_t QOS_COMPLETE = 0x01;
  constexpr uint8_t QOS_TARGET_CONTACTED = 0x02;
  constexpr uint8_t QOS_DATA_RECEIVED = 0x04;

  qos->count = 1;
  qos->count_pending = 0;
  qos->info[0].flags = QOS_COMPLETE | QOS_TARGET_CONTACTED;
  qos->info[0].probes_xmit = 4;
  qos->info[0].probes_recv = 4;
  qos->info[0].data_len = 0;
  qos->info[0].data_ptr = 0;
  qos->info[0].rtt_min_in_msecs = 10;
  qos->info[0].rtt_med_in_msecs = 10;
  qos->info[0].up_bits_per_sec = 1024 * 1024;    // 1 Mbps
  qos->info[0].down_bits_per_sec = 1024 * 1024;  // 1 Mbps

  *pqos = qos_guest;

  if (event_handle) {
    auto ev =
        kernel_state()->object_table()->LookupObject<XEvent>(event_handle);
    if (ev) {
      ev->Set(0, false);
    }
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosServiceLookup, kNetworking, kStub);

dword_result_t NetDll_XNetQosRelease_entry(dword_t caller,
                                           pointer_t<XNQOS> qos) {
  XELOGI("XNetQosRelease: qos={:08X}", qos.guest_address());

  if (!qos) {
    return X_STATUS_INVALID_PARAMETER;
  }
  kernel_memory()->SystemHeapFree(qos.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosRelease, kNetworking, kStub);

// QoS Listen flags
constexpr uint32_t XNET_QOS_LISTEN_ENABLE = 0x00000001;
constexpr uint32_t XNET_QOS_LISTEN_DISABLE = 0x00000002;
constexpr uint32_t XNET_QOS_LISTEN_SET_DATA = 0x00000004;
constexpr uint32_t XNET_QOS_LISTEN_SET_BITSPERSEC = 0x00000008;
constexpr uint32_t XNET_QOS_LISTEN_RELEASE = 0x00000010;

dword_result_t NetDll_XNetQosListen_entry(dword_t caller, lpvoid_t id,
                                          lpvoid_t data, dword_t data_size,
                                          dword_t bits_per_sec, dword_t flags) {
  // Decode flags for readable logging
  std::string flag_str;
  uint32_t f = flags.value();
  if (f & XNET_QOS_LISTEN_ENABLE) flag_str += "ENABLE|";
  if (f & XNET_QOS_LISTEN_DISABLE) flag_str += "DISABLE|";
  if (f & XNET_QOS_LISTEN_SET_DATA) flag_str += "SET_DATA|";
  if (f & XNET_QOS_LISTEN_SET_BITSPERSEC) flag_str += "SET_BITSPERSEC|";
  if (f & XNET_QOS_LISTEN_RELEASE) flag_str += "RELEASE|";
  if (!flag_str.empty()) flag_str.pop_back();  // remove trailing |

  XELOGI(
      "XNetQosListen: flags={:08X} [{}], data_size={}, bps={}, "
      "id={:08X}, data={:08X}",
      f, flag_str, data_size.value(), bits_per_sec.value(),
      id.guest_address(), data.guest_address());

  // Dump the XNKID (session ID) being listened on
  if (id) {
    auto* xnkid = reinterpret_cast<const uint8_t*>(id.host_address());
    XELOGI(
        "XNetQosListen: XNKID={:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        xnkid[0], xnkid[1], xnkid[2], xnkid[3],
        xnkid[4], xnkid[5], xnkid[6], xnkid[7]);
  }

  // Dump QoS data if SET_DATA flag is present
  if ((f & XNET_QOS_LISTEN_SET_DATA) && data && data_size > 0) {
    auto* bytes = reinterpret_cast<const uint8_t*>(data.host_address());
    uint32_t size = data_size.value();
    std::string hex;
    for (uint32_t i = 0; i < size && i < 128; i++) {
      hex += fmt::format("{:02X} ", bytes[i]);
      if ((i + 1) % 16 == 0) hex += "\n  ";
    }
    XELOGI("XNetQosListen: QoS data ({} bytes):\n  {}", size, hex);
  } else if (f & XNET_QOS_LISTEN_ENABLE) {
    XELOGI(
        "XNetQosListen: ENABLE without SET_DATA - game did NOT provide QoS "
        "payload (this is the problem for session discovery)");
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosListen, kNetworking, kImplemented);

struct XNQOSLISTENSTATS {
  xe::be<uint32_t> flags;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_ptr;
  xe::be<uint32_t> probes_received;
  xe::be<uint32_t> probes_responded;
};

dword_result_t NetDll_XNetQosGetListenStats_entry(
    dword_t caller, lpvoid_t id, pointer_t<XNQOSLISTENSTATS> stats_ptr) {
  XELOGI("XNetQosGetListenStats: id={:08X}, stats={:08X}", id.guest_address(),
         stats_ptr.guest_address());

  if (stats_ptr) {
    stats_ptr->flags = 0;
    stats_ptr->data_size = 0;
    stats_ptr->data_ptr = 0;
    stats_ptr->probes_received = 0;
    stats_ptr->probes_responded = 0;
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosGetListenStats, kNetworking, kImplemented);

dword_result_t NetDll_inet_addr_entry(lpstring_t addr_ptr) {
  if (!addr_ptr) {
    return -1;
  }

  // Xbox 360 returns 0 for empty string instead of -1
  // https://docs.microsoft.com/en-us/windows/win32/api/winsock2/nf-winsock2-inet_addr#return-value
  if (addr_ptr.value().empty()) {
    return 0;
  }

  asio::error_code ec;
  auto addr = asio::ip::make_address_v4(addr_ptr.value(), ec);
  if (ec) {
    return -1;  // INADDR_NONE
  }

  // to_uint() returns host byte order which matches byte_swap(inet_addr())
  return addr.to_uint();
}
DECLARE_XAM_EXPORT1(NetDll_inet_addr, kNetworking, kImplemented);

dword_result_t NetDll_socket_entry(dword_t caller, dword_t af, dword_t type,
                                   dword_t protocol) {
  XELOGI("NetDll_socket: af={}, type={}, protocol={}", af.value(), type.value(),
         protocol.value());

  XSocket* socket = new XSocket(kernel_state());
  X_STATUS result = socket->Initialize(XSocket::AddressFamily((uint32_t)af),
                                       XSocket::Type((uint32_t)type),
                                       XSocket::Protocol((uint32_t)protocol));

  if (XFAILED(result)) {
    socket->Release();

    XThread::SetLastError(socket->XWSAGetLastError());
    XELOGE("NetDll_socket: failed with error {:08X}",
           socket->XWSAGetLastError());
    return -1;
  }

  // Clear last error on success
  XThread::SetLastError(0);
  return socket->handle();
}
DECLARE_XAM_EXPORT1(NetDll_socket, kNetworking, kImplemented);

dword_result_t NetDll_closesocket_entry(dword_t caller, dword_t socket_handle) {
  XELOGI("NetDll_closesocket: socket={:08X}", socket_handle.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  // TODO: Absolutely delete this object. It is no longer valid after calling
  // closesocket.
  socket->Close();
  socket->ReleaseHandle();
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_closesocket, kNetworking, kImplemented);

int_result_t NetDll_shutdown_entry(dword_t caller, dword_t socket_handle,
                                   int_t how) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  auto ret = socket->Shutdown(how);
  if (ret == -1) {
    XThread::SetLastError(socket->XWSAGetLastError());
  }
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_shutdown, kNetworking, kImplemented);

dword_result_t NetDll_setsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, dword_t optlen) {
  uint32_t optval = 0;
  if (optval_ptr && optlen >= 4) {
    optval = xe::load_and_swap<uint32_t>(optval_ptr);
  }
  XELOGI(
      "NetDll_setsockopt: socket={:08X}, level={:04X}, optname={:04X}, val={}",
      socket_handle.value(), level.value(), optname.value(), optval);

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->SetOption(level, optname, optval_ptr, optlen);
  int result = XSUCCEEDED(status) ? 0 : -1;
  XELOGI("NetDll_setsockopt: {}, returning {}",
         XSUCCEEDED(status) ? "success" : "failed", result);
  return result;
}
DECLARE_XAM_EXPORT1(NetDll_setsockopt, kNetworking, kImplemented);

dword_result_t NetDll_getsockopt_entry(dword_t caller, dword_t socket_handle,
                                       dword_t level, dword_t optname,
                                       lpvoid_t optval_ptr, lpdword_t optlen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  uint32_t native_len = *optlen;
  X_STATUS status = socket->GetOption(level, optname, optval_ptr, &native_len);
  return XSUCCEEDED(status) ? 0 : -1;
}
DECLARE_XAM_EXPORT1(NetDll_getsockopt, kNetworking, kImplemented);

dword_result_t NetDll_ioctlsocket_entry(dword_t caller, dword_t socket_handle,
                                        dword_t cmd, lpvoid_t arg_ptr) {
  uint32_t arg_val = 0;
  if (arg_ptr) {
    arg_val = xe::load_and_swap<uint32_t>(arg_ptr);
  }
  XELOGI("NetDll_ioctlsocket: socket={:08X}, cmd={:08X}, arg={}",
         socket_handle.value(), cmd.value(), arg_val);

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  X_STATUS status = socket->IOControl(cmd, arg_ptr);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->XWSAGetLastError());
    XELOGE("NetDll_ioctlsocket: failed with error {:08X}",
           socket->XWSAGetLastError());
    return -1;
  }

  // Clear last error on success - clear both guest and host errors
  XThread::SetLastError(0);
#ifdef XE_PLATFORM_WIN32
  WSASetLastError(0);
#endif
  XELOGI("NetDll_ioctlsocket: success");
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_ioctlsocket, kNetworking, kImplemented);

dword_result_t NetDll_bind_entry(dword_t caller, dword_t socket_handle,
                                 pointer_t<XSOCKADDR_IN> name,
                                 dword_t namelen) {
  XELOGI("NetDll_bind: socket={:08X}, namelen={}", socket_handle.value(),
         namelen.value());

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR_IN native_name(name);

  uint16_t port = static_cast<uint16_t>(native_name.sin_port);
  auto addr = asio::ip::address_v4(static_cast<uint32_t>(native_name.sin_addr));
  XELOGI("NetDll_bind: binding to {}:{}", addr.to_string(), port);
  X_STATUS status = socket->Bind(&native_name, namelen);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->XWSAGetLastError());
    XELOGE("NetDll_bind: failed with error {:08X}", socket->XWSAGetLastError());
    return -1;
  }

  // Clear last error on success
  XThread::SetLastError(0);
  XELOGI("NetDll_bind: success, port={}", socket->bound_port());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_bind, kNetworking, kImplemented);

dword_result_t NetDll_connect_entry(dword_t caller, dword_t socket_handle,
                                    pointer_t<XSOCKADDR> name,
                                    dword_t namelen) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  // Log the address we're connecting to
  auto* addr_in = reinterpret_cast<const XSOCKADDR_IN*>(name.host_address());
  XELOGI("NetDll_connect: socket={:08X}, addr={}.{}.{}.{}:{}",
         socket_handle.value(),
         (uint8_t)(addr_in->sin_addr >> 24),
         (uint8_t)(addr_in->sin_addr >> 16),
         (uint8_t)(addr_in->sin_addr >> 8),
         (uint8_t)(addr_in->sin_addr),
         (uint16_t)addr_in->sin_port);

  N_XSOCKADDR native_name(name);
  X_STATUS status = socket->Connect(&native_name, namelen);
  if (XFAILED(status)) {
    XELOGI("NetDll_connect: failed, error={:08X}", socket->XWSAGetLastError());
    XThread::SetLastError(socket->XWSAGetLastError());
    return -1;
  }

  XELOGI("NetDll_connect: success");
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_connect, kNetworking, kImplemented);

dword_result_t NetDll_listen_entry(dword_t caller, dword_t socket_handle,
                                   int_t backlog) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  XELOGI("NetDll_listen: socket={:08X}, port={}, backlog={}",
         socket_handle.value(), socket->bound_port(), backlog.value());

  X_STATUS status = socket->Listen(backlog);
  if (XFAILED(status)) {
    XELOGI("NetDll_listen: failed, error={:08X}", socket->XWSAGetLastError());
    XThread::SetLastError(socket->XWSAGetLastError());
    return -1;
  }

  XELOGI("NetDll_listen: success");
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_listen, kNetworking, kImplemented);

dword_result_t NetDll_accept_entry(dword_t caller, dword_t socket_handle,
                                   pointer_t<XSOCKADDR> addr_ptr,
                                   lpdword_t addrlen_ptr) {
  if (!addr_ptr) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEFAULT));
    return -1;
  }

  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  XELOGI("NetDll_accept: socket={:08X}, port={}", socket_handle.value(),
         socket->bound_port());

  N_XSOCKADDR native_addr(addr_ptr);
  int native_len = *addrlen_ptr;
  auto new_socket = socket->Accept(&native_addr, &native_len);
  if (new_socket) {
    addr_ptr->address_family = native_addr.address_family;
    std::memcpy(addr_ptr->sa_data, native_addr.sa_data, *addrlen_ptr - 2);
    *addrlen_ptr = native_len;

    XELOGI("NetDll_accept: accepted connection, new_socket={:08X}",
           new_socket->handle());
    return new_socket->handle();
  } else {
    XELOGI("NetDll_accept: no connection available");
    return -1;
  }
}
DECLARE_XAM_EXPORT1(NetDll_accept, kNetworking, kImplemented);

struct x_fd_set {
  xe::be<uint32_t> fd_count;
  xe::be<uint32_t> fd_array[64];
};

struct host_set {
  uint32_t count;
  object_ref<XSocket> sockets[64];

  void Load(const x_fd_set* guest_set) {
    assert_true(guest_set->fd_count < 64);
    this->count = guest_set->fd_count;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket_handle = static_cast<X_HANDLE>(guest_set->fd_array[i]);
      if (socket_handle == -1) {
        this->count = i;
        break;
      }
      auto socket =
          kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
      assert_not_null(socket);
      this->sockets[i] = socket;
    }
  }

  void Store(x_fd_set* guest_set) {
    guest_set->fd_count = 0;
    for (uint32_t i = 0; i < this->count; ++i) {
      auto socket = this->sockets[i];
      guest_set->fd_array[guest_set->fd_count++] = socket->handle();
    }
  }
};

int_result_t NetDll_select_entry(dword_t caller, dword_t nfds,
                                 pointer_t<x_fd_set> readfds,
                                 pointer_t<x_fd_set> writefds,
                                 pointer_t<x_fd_set> exceptfds,
                                 lpvoid_t timeout_ptr) {
  XELOGI(
      "NetDll_select: nfds={}, readfds={}, writefds={}, exceptfds={}, "
      "timeout={:08X}",
      nfds.value(), readfds ? readfds->fd_count.get() : 0,
      writefds ? writefds->fd_count.get() : 0,
      exceptfds ? exceptfds->fd_count.get() : 0, timeout_ptr.guest_address());

  // Load socket sets from guest
  host_set host_readfds = {0};
  if (readfds) {
    host_readfds.Load(readfds);
    for (uint32_t i = 0; i < host_readfds.count; ++i) {
      XELOGI("NetDll_select: readfds[{}] = handle {:08X} port {}",
             i, host_readfds.sockets[i]->handle(),
             host_readfds.sockets[i]->bound_port());
    }
  }
  host_set host_writefds = {0};
  if (writefds) {
    host_writefds.Load(writefds);
  }
  host_set host_exceptfds = {0};
  if (exceptfds) {
    host_exceptfds.Load(exceptfds);
  }

  // Parse timeout
  int64_t timeout_us = -1;  // -1 = infinite (block forever)
  if (timeout_ptr) {
    int32_t tv_sec = timeout_ptr.as_array<int32_t>()[0];
    int32_t tv_usec = timeout_ptr.as_array<int32_t>()[1];
    Clock::ScaleGuestDurationTimeval(&tv_sec, &tv_usec);
    timeout_us = static_cast<int64_t>(tv_sec) * 1000000 + tv_usec;
  }

  // If no sockets in any set, just sleep for the timeout (common: game uses
  // select as a timer).
  bool has_sockets = (host_readfds.count + host_writefds.count +
                      host_exceptfds.count) > 0;
  if (!has_sockets) {
    if (timeout_us > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(timeout_us));
    }
    return 0;
  }

  // Poll loop: check socket readiness using ASIO queries.
  // Poll at 1ms intervals until timeout expires or sockets become ready.
  auto start = std::chrono::steady_clock::now();
  int total_ready = 0;

  do {
    // Build result sets — only include sockets that are ready
    host_set ready_read = {0};
    host_set ready_write = {0};
    // exceptfds: we don't have a good ASIO equivalent, leave empty

    for (uint32_t i = 0; i < host_readfds.count; ++i) {
      if (host_readfds.sockets[i]->GetBytesAvailable() > 0) {
        ready_read.sockets[ready_read.count++] = host_readfds.sockets[i];
      }
    }

    for (uint32_t i = 0; i < host_writefds.count; ++i) {
      if (host_writefds.sockets[i]->IsWritable()) {
        ready_write.sockets[ready_write.count++] = host_writefds.sockets[i];
      }
    }

    total_ready = ready_read.count + ready_write.count;

    if (total_ready > 0) {
      // Store results back to guest
      if (readfds) ready_read.Store(readfds);
      if (writefds) ready_write.Store(writefds);
      if (exceptfds) {
        host_set empty = {0};
        empty.Store(exceptfds);
      }
      return total_ready;
    }

    // Not ready yet — check timeout
    if (timeout_us == 0) {
      break;  // Non-blocking poll
    }

    if (timeout_us > 0) {
      auto elapsed = std::chrono::steady_clock::now() - start;
      if (std::chrono::duration_cast<std::chrono::microseconds>(elapsed)
              .count() >= timeout_us) {
        break;  // Timeout expired
      }
    }

    // Sleep 1ms before next poll
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (true);

  // Timeout: return 0 with empty sets
  if (readfds) {
    host_set empty = {0};
    empty.Store(readfds);
  }
  if (writefds) {
    host_set empty = {0};
    empty.Store(writefds);
  }
  if (exceptfds) {
    host_set empty = {0};
    empty.Store(exceptfds);
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_select, kNetworking, kImplemented);

dword_result_t NetDll_recv_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  return socket->Recv(buf_ptr, buf_len, flags);
}
DECLARE_XAM_EXPORT1(NetDll_recv, kNetworking, kImplemented);

dword_result_t NetDll_recvfrom_entry(dword_t caller, dword_t socket_handle,
                                     lpvoid_t buf_ptr, dword_t buf_len,
                                     dword_t flags,
                                     pointer_t<XSOCKADDR_IN> from_ptr,
                                     lpdword_t fromlen_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR_IN native_from;
  if (from_ptr) {
    native_from = *from_ptr;
  }
  uint32_t native_fromlen = fromlen_ptr ? fromlen_ptr.value() : 0;
  int ret = socket->RecvFrom(buf_ptr, buf_len, flags, &native_from,
                             fromlen_ptr ? &native_fromlen : 0);

  if (from_ptr) {
    from_ptr->sin_family = native_from.sin_family;
    from_ptr->sin_port = native_from.sin_port;
    from_ptr->sin_addr = native_from.sin_addr;
    std::memset(from_ptr->x_sin_zero, 0, sizeof(from_ptr->x_sin_zero));
  }
  if (fromlen_ptr) {
    *fromlen_ptr = native_fromlen;
  }

  if (ret == -1) {
    XThread::SetLastError(socket->XWSAGetLastError());
  } else if (ret > 0) {
    uint32_t from_ip = static_cast<uint32_t>(native_from.sin_addr);
    uint16_t from_port = static_cast<uint16_t>(native_from.sin_port);

    // Discard packets from ourselves (self-broadcast echo)
    auto adapter = QueryActiveAdapter();
    XELOGI(
        "NetDll_recvfrom: from_ip={:08X}, adapter_ip={:08X}, "
        "ntohl(adapter_ip)={:08X}, match={}",
        from_ip, adapter.ip_addr, adapter.found ? ntohl(adapter.ip_addr) : 0u,
        adapter.found && from_ip == ntohl(adapter.ip_addr));
    if (adapter.found && from_ip == ntohl(adapter.ip_addr)) {
      XELOGI("NetDll_recvfrom: discarding self-broadcast ({} bytes)", ret);
      XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAEWOULDBLOCK));
      return -1;
    }

    auto from_addr = asio::ip::address_v4(from_ip);
    // Log full packet for protocol debugging
    std::string hex_dump;
    uint8_t* data = reinterpret_cast<uint8_t*>(buf_ptr.host_address());
    for (int i = 0; i < ret; i++) {
      hex_dump += fmt::format("{:02X} ", data[i]);
      if ((i + 1) % 16 == 0) hex_dump += "\n";
    }
    XELOGI("NetDll_recvfrom: received {} bytes from {}:{}:\n{}", ret,
           from_addr.to_string(), from_port, hex_dump);
  }

  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_recvfrom, kNetworking, kImplemented);

dword_result_t NetDll_send_entry(dword_t caller, dword_t socket_handle,
                                 lpvoid_t buf_ptr, dword_t buf_len,
                                 dword_t flags) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  return socket->Send(buf_ptr, buf_len, flags);
}
DECLARE_XAM_EXPORT1(NetDll_send, kNetworking, kImplemented);

dword_result_t NetDll_sendto_entry(dword_t caller, dword_t socket_handle,
                                   lpvoid_t buf_ptr, dword_t buf_len,
                                   dword_t flags,
                                   pointer_t<XSOCKADDR_IN> to_ptr,
                                   dword_t to_len) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  N_XSOCKADDR_IN native_to(to_ptr);
  uint32_t dest_addr = static_cast<uint32_t>(native_to.sin_addr);
  uint16_t dest_port = static_cast<uint16_t>(native_to.sin_port);

  // Check if this is a broadcast and we have a configured interface
  bool is_broadcast = (dest_addr == 0xFFFFFFFF);
  if (is_broadcast && !cvars::systemlink_interface.empty()) {
    auto adapter = QueryActiveAdapter();
    if (adapter.found) {
      // Send broadcast via a temporary socket bound to the specific interface
      asio::error_code ec;
      asio::io_context io;
      asio::ip::udp::socket temp_sock(io);
      temp_sock.open(asio::ip::udp::v4(), ec);
      if (!ec) {
        // Enable broadcast on temp socket
        temp_sock.set_option(asio::socket_base::broadcast(true), ec);
        // Bind to the specific interface
        auto local_addr = asio::ip::address_v4(ntohl(adapter.ip_addr));
        temp_sock.bind(asio::ip::udp::endpoint(local_addr, 0), ec);
        if (!ec) {
          auto dest = asio::ip::udp::endpoint(
              asio::ip::address_v4::broadcast(), dest_port);
          size_t sent = temp_sock.send_to(
              asio::buffer(reinterpret_cast<const void*>(buf_ptr.host_address()), buf_len), dest, 0, ec);
          if (!ec) {
            // Log first 32 bytes of broadcast data
            std::string hex_dump;
            uint8_t* data = reinterpret_cast<uint8_t*>(buf_ptr.host_address());
            for (uint32_t i = 0; i < std::min(buf_len.value(), 32u); i++) {
              hex_dump += fmt::format("{:02X} ", data[i]);
            }
            XELOGI("NetDll_sendto: broadcast {} bytes to port {} via {} [{}]",
                   sent, dest_port, local_addr.to_string(), hex_dump);
            return static_cast<int>(sent);
          }
        }
      }
      XELOGE("NetDll_sendto: broadcast via interface failed: {}", ec.message());
      // Fall through to normal send
    }
  }

  if (to_ptr) {
    auto dest = asio::ip::address_v4(dest_addr);
    // Log first 32 bytes of sent data for protocol debugging
    std::string hex_dump;
    uint8_t* data = reinterpret_cast<uint8_t*>(buf_ptr.host_address());
    for (uint32_t i = 0; i < std::min(buf_len.value(), 32u); i++) {
      hex_dump += fmt::format("{:02X} ", data[i]);
    }
    XELOGI("NetDll_sendto: {} bytes from {} to {}:{} [{}]", buf_len.value(),
           socket->local_endpoint_string(), dest.to_string(), dest_port,
           hex_dump);
  }

  int ret = socket->SendTo(buf_ptr, buf_len, flags, &native_to, to_len);
  return ret;
}
DECLARE_XAM_EXPORT1(NetDll_sendto, kNetworking, kImplemented);

dword_result_t NetDll___WSAFDIsSet_entry(dword_t socket_handle,
                                         pointer_t<x_fd_set> fd_set) {
  const uint8_t max_fd_count =
      std::min((uint32_t)fd_set->fd_count, uint32_t(64));
  for (uint8_t i = 0; i < max_fd_count; i++) {
    if (fd_set->fd_array[i] == socket_handle) {
      return 1;
    }
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll___WSAFDIsSet, kNetworking, kImplemented);

void NetDll_WSASetLastError_entry(dword_t error_code) {
  XThread::SetLastError(error_code);
}
DECLARE_XAM_EXPORT1(NetDll_WSASetLastError, kNetworking, kImplemented);

dword_result_t NetDll_getsockname_entry(dword_t caller, dword_t socket_handle,
                                        lpvoid_t buf_ptr, lpdword_t len_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  int buffer_len = *len_ptr;

  X_STATUS status = socket->GetSockName(buf_ptr, &buffer_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->XWSAGetLastError());
    return -1;
  }

  // Log the result
  if (buffer_len >= sizeof(XSOCKADDR_IN)) {
    auto addr = reinterpret_cast<XSOCKADDR_IN*>(buf_ptr.host_address());
    XELOGI("NetDll_getsockname: socket={:08X}, family={}, port={}, addr={:08X}",
           socket_handle.value(), uint16_t(addr->sin_family),
           uint16_t(addr->sin_port), uint32_t(addr->sin_addr));
  }

  *len_ptr = buffer_len;
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getsockname, kNetworking, kImplemented);

dword_result_t NetDll_getpeername_entry(dword_t caller, dword_t socket_handle,
                                        lpvoid_t buf_ptr, lpdword_t len_ptr) {
  auto socket =
      kernel_state()->object_table()->LookupObject<XSocket>(socket_handle);
  if (!socket) {
    XThread::SetLastError(uint32_t(X_WSA_ERROR::X_WSAENOTSOCK));
    return -1;
  }

  int buffer_len = *len_ptr;

  X_STATUS status = socket->GetPeerName(buf_ptr, &buffer_len);
  if (XFAILED(status)) {
    XThread::SetLastError(socket->XWSAGetLastError());
    return -1;
  }

  *len_ptr = buffer_len;
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_getpeername, kNetworking, kImplemented);

dword_result_t NetDll_XNetCreateKey_entry(dword_t caller, lpdword_t key_id,
                                          lpdword_t exchange_key) {
  XELOGI("XNetCreateKey: key_id={:08X}, exchange_key={:08X}",
         key_id.guest_address(), exchange_key.guest_address());

  // Generate system link session key (matching netplay)
  if (key_id) {
    std::random_device rnd;
    std::mt19937_64 gen(rnd());
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
    uint8_t* key = reinterpret_cast<uint8_t*>(key_id.host_address());
    // Top byte = 0x00 (XNKID_SYSTEM_LINK), rest random
    key[0] = 0x00;
    for (int i = 1; i < 8; ++i) {
      key[i] = static_cast<uint8_t>(dist(gen) & 0xFF);
    }
  }
  if (exchange_key) {
    // Identity exchange key (each byte = its index)
    // Matches netplay's GenerateIdentityExchangeKey
    uint8_t* key = reinterpret_cast<uint8_t*>(exchange_key.host_address());
    for (int i = 0; i < 16; ++i) {
      key[i] = static_cast<uint8_t>(i);
    }
  }
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetCreateKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetRegisterKey_entry(dword_t caller, lpdword_t key_id,
                                            lpdword_t exchange_key) {
  XELOGI("XNetRegisterKey: key_id={:08X}, exchange_key={:08X}",
         key_id.guest_address(), exchange_key.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetRegisterKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetUnregisterKey_entry(dword_t caller,
                                              lpdword_t key_id) {
  XELOGI("XNetUnregisterKey: key_id={:08X}", key_id.guest_address());
  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterKey, kNetworking, kImplemented);

dword_result_t NetDll_XNetConnect_entry(dword_t caller, dword_t addr) {
  XELOGI("XNetConnect({:08X})", addr.value());

  // Track connection start time for this address
  {
    std::lock_guard<std::mutex> lock(g_xnet_connection_mutex);
    g_xnet_connections[addr.value()] = std::chrono::steady_clock::now();
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetConnect, kNetworking, kImplemented);

dword_result_t NetDll_XNetUnregisterInAddr_entry(dword_t caller, dword_t addr) {
  XELOGI("XNetUnregisterInAddr({:08X})", addr.value());

  // Remove connection tracking
  {
    std::lock_guard<std::mutex> lock(g_xnet_connection_mutex);
    g_xnet_connections.erase(addr.value());
  }

  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(NetDll_XNetUnregisterInAddr, kNetworking, kStub);

dword_result_t NetDll_XNetGetConnectStatus_entry(dword_t caller,
                                                 dword_t addr) {
  // Check if connection was initiated and how long ago
  {
    std::lock_guard<std::mutex> lock(g_xnet_connection_mutex);
    auto it = g_xnet_connections.find(addr.value());
    if (it != g_xnet_connections.end()) {
      auto elapsed = std::chrono::steady_clock::now() - it->second;
      if (elapsed < kXNetConnectDelay) {
        XELOGI("XNetGetConnectStatus({:08X}) -> PENDING", addr.value());
        return 0x01;  // XNET_CONNECT_STATUS_PENDING
      }
    }
  }

  XELOGI("XNetGetConnectStatus({:08X}) -> CONNECTED", addr.value());
  return 0x02;  // XNET_CONNECT_STATUS_CONNECTED
}
DECLARE_XAM_EXPORT1(NetDll_XNetGetConnectStatus, kNetworking, kImplemented);

dword_result_t NetDll_XNetQosLookup_entry(
    dword_t caller, dword_t num_xnaddrs, lpvoid_t xnaddrs_ptr,
    lpvoid_t xnkids_ptr, lpvoid_t xnkeys_ptr, dword_t num_gateways,
    lpvoid_t gateways_ptr, lpvoid_t service_ids_ptr, dword_t num_probes,
    dword_t bits_per_sec, dword_t flags, dword_t event_handle,
    lpdword_t pqos) {
  XELOGI(
      "XNetQosLookup: num_xnaddrs={}, xnaddrs={:08X}, xnkids={:08X}, "
      "xnkeys={:08X}, num_gateways={}, gateways={:08X}, "
      "service_ids={:08X}, probes={}, bps={}, flags={:08X}, event={:08X}, "
      "pqos={:08X}",
      num_xnaddrs.value(), xnaddrs_ptr.guest_address(),
      xnkids_ptr.guest_address(), xnkeys_ptr.guest_address(),
      num_gateways.value(), gateways_ptr.guest_address(),
      service_ids_ptr.guest_address(), num_probes.value(),
      bits_per_sec.value(), flags.value(), event_handle.value(),
      pqos.guest_address());

  // Dump XNADDR and XNKID arrays for each target
  for (uint32_t i = 0; i < num_xnaddrs; i++) {
    if (xnaddrs_ptr) {
      auto* addrs = reinterpret_cast<const XNADDR*>(xnaddrs_ptr.host_address());
      uint32_t ina_val, ina_online_val;
      std::memcpy(&ina_val, &addrs[i].ina, sizeof(uint32_t));
      std::memcpy(&ina_online_val, &addrs[i].inaOnline, sizeof(uint32_t));
      XELOGI(
          "XNetQosLookup: target[{}] XNADDR: ina={:08X}, inaOnline={:08X}, "
          "wPortOnline={}, MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
          i, ina_val, ina_online_val,
          (uint16_t)addrs[i].wPortOnline,
          addrs[i].abEnet[0], addrs[i].abEnet[1], addrs[i].abEnet[2],
          addrs[i].abEnet[3], addrs[i].abEnet[4], addrs[i].abEnet[5]);
    }
    if (xnkids_ptr) {
      auto* kids = reinterpret_cast<const uint8_t*>(xnkids_ptr.host_address());
      XELOGI(
          "XNetQosLookup: target[{}] XNKID="
          "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
          i, kids[i * 8], kids[i * 8 + 1], kids[i * 8 + 2], kids[i * 8 + 3],
          kids[i * 8 + 4], kids[i * 8 + 5], kids[i * 8 + 6], kids[i * 8 + 7]);
    }
    if (xnkeys_ptr) {
      auto* keys = reinterpret_cast<const uint8_t*>(xnkeys_ptr.host_address());
      XELOGI(
          "XNetQosLookup: target[{}] XNKEY="
          "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
          "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
          i,
          keys[i * 16], keys[i * 16 + 1], keys[i * 16 + 2], keys[i * 16 + 3],
          keys[i * 16 + 4], keys[i * 16 + 5], keys[i * 16 + 6], keys[i * 16 + 7],
          keys[i * 16 + 8], keys[i * 16 + 9], keys[i * 16 + 10], keys[i * 16 + 11],
          keys[i * 16 + 12], keys[i * 16 + 13], keys[i * 16 + 14], keys[i * 16 + 15]);
    }
  }

  if (!pqos) {
    return 0;
  }

  uint32_t count = num_xnaddrs + num_gateways;
  uint32_t size = sizeof(XNQOS) + (sizeof(XNQOSINFO) * count);
  uint32_t qos_guest = kernel_memory()->SystemHeapAlloc(size);
  XNQOS* qos = kernel_memory()->TranslateVirtual<XNQOS*>(qos_guest);
  std::memset(qos, 0, size);

  *pqos = qos_guest;

  constexpr uint8_t QOS_COMPLETE = 0x01;
  constexpr uint8_t QOS_TARGET_CONTACTED = 0x02;
  constexpr uint8_t QOS_DATA_RECEIVED = 0x04;

  auto fill_qos_entries = [&]() {
    for (uint32_t i = 0; i < count; i++) {
      qos->info[i].flags = QOS_COMPLETE | QOS_TARGET_CONTACTED;
      qos->info[i].probes_xmit = num_probes ? num_probes.value() : 4;
      qos->info[i].probes_recv = num_probes ? num_probes.value() : 4;
      qos->info[i].data_len = 0;
      qos->info[i].data_ptr = 0;
      qos->info[i].rtt_min_in_msecs = 1;
      qos->info[i].rtt_med_in_msecs = 5;
      qos->info[i].up_bits_per_sec = 100000000;    // 100 Mbps
      qos->info[i].down_bits_per_sec = 100000000;  // 100 Mbps
    }
  };

  if (event_handle) {
    // Async path: game will poll count_pending until 0, then check count.
    // Must return with count_pending > 0 to prevent immediate retry spam.
    qos->count_pending = count;
    qos->count = 0;

    uint32_t evt_handle = event_handle.value();
    uint32_t probes_val = num_probes.value();

    std::thread([qos, count, probes_val, evt_handle]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      for (uint32_t i = 0; i < count; i++) {
        qos->info[i].flags = QOS_COMPLETE | QOS_TARGET_CONTACTED;
        qos->info[i].probes_xmit = probes_val ? probes_val : 4;
        qos->info[i].probes_recv = probes_val ? probes_val : 4;
        qos->info[i].data_len = 0;
        qos->info[i].data_ptr = 0;
        qos->info[i].rtt_min_in_msecs = 1;
        qos->info[i].rtt_med_in_msecs = 5;
        qos->info[i].up_bits_per_sec = 100000000;
        qos->info[i].down_bits_per_sec = 100000000;

        qos->count_pending =
            std::max(static_cast<int32_t>(qos->count_pending - 1), 0);
        qos->count++;
      }

      xboxkrnl::xeNtSetEvent(evt_handle, nullptr);
    }).detach();
  } else {
    // Synchronous path: no event to signal, fill results immediately.
    qos->count = count;
    qos->count_pending = 0;
    fill_qos_entries();
    XELOGI("XNetQosLookup: sync complete, {} entries, flags=0x{:02X}", count,
           QOS_COMPLETE | QOS_TARGET_CONTACTED);
  }

  return 0;
}
DECLARE_XAM_EXPORT1(NetDll_XNetQosLookup, kNetworking, kImplemented);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

DECLARE_XAM_EMPTY_REGISTER_EXPORTS(Net);

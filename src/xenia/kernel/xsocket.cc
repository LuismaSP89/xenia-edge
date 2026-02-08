/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsocket.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/net_utils.h"
#include "xenia/kernel/xam/xam_module.h"
#include "xenia/kernel/xevent.h"

namespace xe {
namespace kernel {

// Tracks ports that were remapped by Bind() (original_port + 10000).
// Key = remapped port (host-side).
// Value = {original port (game-side), reference count}.
// Reference-counted because multiple sockets can bind to the same remapped port
// (with SO_REUSEADDR). Entries are removed when the last socket closes.
struct PortRemap {
  uint16_t original_port;
  uint32_t refcount;
};
static std::mutex remapped_ports_mutex;
static std::unordered_map<uint16_t, PortRemap> remapped_ports;

// Reverse a port remap if one exists. Returns the original game-side port,
// or the input port unchanged if no mapping exists.
// Also handles remote hosts' remapped ports: if the port is in the remapped
// range (10001-11023, corresponding to privileged ports 1-1023 + 10000),
// un-remap it even if not in our local table. This allows cross-machine
// communication to work correctly when the sender's port was remapped.
static uint16_t UnremapPort(uint16_t port) {
  // First check local remapped_ports table
  {
    std::lock_guard<std::mutex> lock(remapped_ports_mutex);
    auto it = remapped_ports.find(port);
    if (it != remapped_ports.end()) {
      return it->second.original_port;
    }
  }

  // Also un-remap ports in the privileged+10000 range, to handle remote hosts
  // whose privileged ports were remapped. Ports 1-1023 become 10001-11023.
  if (port >= 10001 && port < 11024) {
    return port - 10000;
  }

  return port;
}

static void AddPortRemap(uint16_t remapped_port, uint16_t original_port) {
  std::lock_guard<std::mutex> lock(remapped_ports_mutex);
  auto it = remapped_ports.find(remapped_port);
  if (it != remapped_ports.end()) {
    it->second.refcount++;
  } else {
    remapped_ports[remapped_port] = {original_port, 1};
  }
}

static void RemovePortRemap(uint16_t remapped_port) {
  std::lock_guard<std::mutex> lock(remapped_ports_mutex);
  auto it = remapped_ports.find(remapped_port);
  if (it != remapped_ports.end()) {
    if (--it->second.refcount == 0) {
      remapped_ports.erase(it);
    }
  }
}

// Shared io_context for all sockets
static asio::io_context& GetIoContext() {
  static asio::io_context io_context;
  return io_context;
}

// io_context background thread for async operations.
// Protected by io_thread_mutex for start/stop thread safety and
// restartability (e.g., game relaunch without emulator restart).
static std::mutex io_thread_mutex;
static bool io_thread_running = false;
static std::unique_ptr<
    asio::executor_work_guard<asio::io_context::executor_type>>
    io_work_guard;
static std::thread io_thread;
static std::atomic<uint32_t> active_socket_count{0};

// Force-stop the io thread during process exit. Without this,
// std::exit() triggers static destructors, and ~std::thread calls
// std::terminate() if the thread is still joinable.
static void ForceStopIoThreadAtExit() {
  std::lock_guard<std::mutex> lock(io_thread_mutex);
  if (!io_thread_running) return;
  io_work_guard.reset();
  GetIoContext().stop();
  if (io_thread.joinable()) {
    io_thread.detach();
  }
  io_thread_running = false;
}

static std::once_flag atexit_once;
static void RegisterAtExitOnce() {
  std::call_once(atexit_once, []() {
    std::atexit(ForceStopIoThreadAtExit);
    std::at_quick_exit(ForceStopIoThreadAtExit);
  });
}

static void EnsureIoThreadRunning() {
  RegisterAtExitOnce();
  std::lock_guard<std::mutex> lock(io_thread_mutex);
  if (io_thread_running) {
    return;
  }

  auto& io = GetIoContext();
  io.restart();
  io_work_guard = std::make_unique<
      asio::executor_work_guard<asio::io_context::executor_type>>(
      asio::make_work_guard(io));
  io_thread = std::thread([&io]() {
    XELOGI("XSocket: io_context thread started");
    io.run();
    XELOGI("XSocket: io_context thread exiting");
  });
  io_thread_running = true;
}

void XSocket::ShutdownIOThread() {
  XELOGI("XSocket::ShutdownIOThread: releasing work guard");

  {
    std::lock_guard<std::mutex> lock(io_thread_mutex);
    if (!io_thread_running) {
      XELOGI("XSocket::ShutdownIOThread: io thread not running");
      return;
    }
    // Release the work guard. All sockets should already be closed by the
    // caller, so the only remaining work is the queued operation_aborted
    // handlers. run() will process them and then return naturally.
    // Do NOT call stop() here — it would prevent those handlers from running,
    // leaking the object_ref<XSocket> captures they hold.
    io_work_guard.reset();
  }

  // Join the io thread. run() will return after processing all remaining
  // handlers (operation_aborted from closed sockets). This releases the
  // object_ref captures, allowing XSocket destructors to fire.
  if (io_thread.joinable()) {
    io_thread.join();
  }

  {
    std::lock_guard<std::mutex> lock(io_thread_mutex);
    io_thread_running = false;
  }
  XELOGI("XSocket::ShutdownIOThread: io thread stopped");

  // Reset the port remap table.
  {
    std::lock_guard<std::mutex> lock(remapped_ports_mutex);
    remapped_ports.clear();
  }
}

uint32_t AsioErrorToWSAError(const asio::error_code& ec) {
  if (!ec) return 0;
  if (ec == asio::error::operation_aborted)
    return uint32_t(X_WSA_ERROR::X_WSA_OPERATION_ABORTED);
  if (ec == asio::error::would_block)
    return uint32_t(X_WSA_ERROR::X_WSAEWOULDBLOCK);
  if (ec == asio::error::connection_refused)
    return uint32_t(X_WSA_ERROR::X_WSAECONNREFUSED);
  if (ec == asio::error::connection_reset)
    return uint32_t(X_WSA_ERROR::X_WSAECONNRESET);
  if (ec == asio::error::timed_out)
    return uint32_t(X_WSA_ERROR::X_WSAETIMEDOUT);
  if (ec == asio::error::host_unreachable)
    return uint32_t(X_WSA_ERROR::X_WSAEHOSTUNREACH);
  if (ec == asio::error::message_size)
    return uint32_t(X_WSA_ERROR::X_WSAEMSGSIZE);
  if (ec == asio::error::no_buffer_space)
    return uint32_t(X_WSA_ERROR::X_WSAENOBUFS);
  if (ec == asio::error::not_connected)
    return uint32_t(X_WSA_ERROR::X_WSAENOTCONN);
  if (ec == asio::error::already_connected)
    return uint32_t(X_WSA_ERROR::X_WSAEISCONN);
  if (ec == asio::error::shut_down)
    return uint32_t(X_WSA_ERROR::X_WSAESHUTDOWN);
  if (ec == asio::error::access_denied)
    return uint32_t(X_WSA_ERROR::X_WSAEACCES);
  if (ec == asio::error::address_in_use)
    return uint32_t(X_WSA_ERROR::X_WSAEADDRINUSE);
  // Fallback
  return static_cast<uint32_t>(ec.value());
}

XSocket::XSocket(KernelState* kernel_state)
    : XObject(kernel_state, kObjectType) {
  active_socket_count.fetch_add(1, std::memory_order_relaxed);
}

XSocket::XSocket(KernelState* kernel_state, asio::ip::tcp::socket socket)
    : XObject(kernel_state, kObjectType), tcp_socket_(std::move(socket)) {
  af_ = AddressFamily::X_AF_INET;
  type_ = Type::X_SOCK_STREAM;
  proto_ = Protocol::X_IPPROTO_TCP;
  active_socket_count.fetch_add(1, std::memory_order_relaxed);
}

XSocket::~XSocket() {
  XELOGI("XSocket::~XSocket: destroying socket (remaining={})",
         active_socket_count.load() - 1);
  Close();
  if (active_socket_count.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    XELOGI("XSocket::~XSocket: last socket, stopping io thread");
    // Last socket destroyed — stop the io_context thread if still running.
    // Normally ShutdownIOThread() handles this during kernel teardown, but
    // this covers the case where sockets are destroyed outside that path.
    // Use stop() as a last resort since we can't do the graceful pattern
    // (close-all-sockets-first) from a single socket's destructor.
    std::lock_guard<std::mutex> lock(io_thread_mutex);
    if (io_thread_running) {
      XELOGI("XSocket::~XSocket: io thread running, stopping");
      io_work_guard.reset();
      GetIoContext().stop();
      if (io_thread.get_id() == std::this_thread::get_id()) {
        XELOGI("XSocket::~XSocket: on io thread, detaching");
        io_thread.detach();
      } else if (io_thread.joinable()) {
        XELOGI("XSocket::~XSocket: joining io thread");
        io_thread.join();
        XELOGI("XSocket::~XSocket: io thread joined");
      }
      io_thread_running = false;
    }
  }
}

uint64_t XSocket::native_handle() {
  if (tcp_socket_ && tcp_socket_->is_open()) {
    return static_cast<uint64_t>(tcp_socket_->native_handle());
  }
  if (udp_socket_ && udp_socket_->is_open()) {
    return static_cast<uint64_t>(udp_socket_->native_handle());
  }
  if (acceptor_ && acceptor_->is_open()) {
    return static_cast<uint64_t>(acceptor_->native_handle());
  }
  return static_cast<uint64_t>(-1);
}

std::string XSocket::local_endpoint_string() const {
  asio::error_code ec;
  if (tcp_socket_ && tcp_socket_->is_open()) {
    auto ep = tcp_socket_->local_endpoint(ec);
    if (!ec) {
      return ep.address().to_string() + ":" + std::to_string(ep.port());
    }
  }
  if (udp_socket_ && udp_socket_->is_open()) {
    auto ep = udp_socket_->local_endpoint(ec);
    if (!ec) {
      return ep.address().to_string() + ":" + std::to_string(ep.port());
    }
  }
  return "unknown";
}

X_STATUS XSocket::Initialize(AddressFamily af, Type type, Protocol proto) {
  af_ = af;
  type_ = type;
  proto_ = proto;

  if (proto == Protocol::X_IPPROTO_VDP) {
    // VDP is a layer on top of UDP.
    proto = Protocol::X_IPPROTO_UDP;
  }

  asio::error_code ec;

  if (type == Type::X_SOCK_STREAM) {
    tcp_socket_.emplace(GetIoContext());
    tcp_socket_->open(asio::ip::tcp::v4(), ec);
  } else if (type == Type::X_SOCK_DGRAM) {
    udp_socket_.emplace(GetIoContext());
    udp_socket_->open(asio::ip::udp::v4(), ec);
  } else {
    return X_STATUS_INVALID_PARAMETER;
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  // Allow port reuse so that in-process relaunches can rebind ports
  // immediately without waiting for TIME_WAIT to expire.
  asio::error_code reuse_ec;
  if (tcp_socket_)
    tcp_socket_->set_option(asio::socket_base::reuse_address(true), reuse_ec);
  else if (udp_socket_)
    udp_socket_->set_option(asio::socket_base::reuse_address(true), reuse_ec);

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Close() {
  // Signal async handlers to skip guest memory writes during teardown.
  closing_.store(true, std::memory_order_release);

  // Clean up port remap entry if this socket had one.
  if (remapped_port_ != 0) {
    RemovePortRemap(remapped_port_);
    remapped_port_ = 0;
  }

  // Close the sockets. ASIO guarantees that closing a socket cancels all
  // pending async operations and their handlers will be invoked with
  // operation_aborted on the io_context thread.
  asio::error_code ec;

  if (tcp_socket_) {
    if (tcp_socket_->is_open()) {
      tcp_socket_->shutdown(asio::socket_base::shutdown_both, ec);
      tcp_socket_->close(ec);
    }
  }

  if (udp_socket_) {
    if (udp_socket_->is_open()) {
      udp_socket_->close(ec);
    }
  }

  if (acceptor_) {
    if (acceptor_->is_open()) {
      acceptor_->close(ec);
    }
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t* optlen) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // Helper to get a typed ASIO option from whichever socket is active
  auto get_opt = [&](auto& option) {
    if (tcp_socket_)
      tcp_socket_->get_option(option, ec);
    else if (udp_socket_)
      udp_socket_->get_option(option, ec);
  };

  if (level == 0xFFFF) {
    // SOL_SOCKET level
    switch (optname) {
      case 0x0004: {  // SO_REUSEADDR
        asio::socket_base::reuse_address opt;
        get_opt(opt);
        if (!ec) {
          xe::store_and_swap<int32_t>(optval_ptr, opt.value() ? 1 : 0);
          *optlen = 4;
        }
        break;
      }
      case 0x0020: {  // SO_BROADCAST
        asio::socket_base::broadcast opt;
        get_opt(opt);
        if (!ec) {
          xe::store_and_swap<int32_t>(optval_ptr, opt.value() ? 1 : 0);
          *optlen = 4;
        }
        break;
      }
      case 0x0080: {  // SO_LINGER
        asio::socket_base::linger opt;
        get_opt(opt);
        if (!ec) {
          // Guest expects {be<uint16_t> onoff, be<uint16_t> secs}
          xe::store_and_swap<uint16_t>(optval_ptr,
                                       opt.enabled() ? 1 : 0);
          xe::store_and_swap<uint16_t>(
              static_cast<uint8_t*>(optval_ptr) + 2,
              static_cast<uint16_t>(opt.timeout()));
          *optlen = 4;
        }
        break;
      }
      case 0x1001: {  // SO_SNDBUF
        asio::socket_base::send_buffer_size opt;
        get_opt(opt);
        if (!ec) {
          xe::store_and_swap<int32_t>(optval_ptr, opt.value());
          *optlen = 4;
        }
        break;
      }
      case 0x1002: {  // SO_RCVBUF
        asio::socket_base::receive_buffer_size opt;
        get_opt(opt);
        if (!ec) {
          xe::store_and_swap<int32_t>(optval_ptr, opt.value());
          *optlen = 4;
        }
        break;
      }
      case 0x1005:    // SO_SNDTIMEO
      case 0x1006: {  // SO_RCVTIMEO
        // No ASIO typed option — use native handle with byte-swap on output
        int native_handle_val = static_cast<int>(native_handle());
        if (native_handle_val == -1) {
          return X_STATUS_INVALID_HANDLE;
        }
        int native_optname =
            (optname == 0x1005) ? SO_SNDTIMEO : SO_RCVTIMEO;
        struct timeval tv;
        socklen_t tv_len = sizeof(tv);
        int ret = getsockopt(native_handle_val, SOL_SOCKET, native_optname,
                             reinterpret_cast<char*>(&tv), &tv_len);
        if (ret < 0) {
          last_error_ = AsioErrorToWSAError(
              asio::error_code(errno, asio::error::get_system_category()));
          return X_STATUS_UNSUCCESSFUL;
        }
        int32_t ms =
            static_cast<int32_t>(tv.tv_sec * 1000 + tv.tv_usec / 1000);
        xe::store_and_swap<int32_t>(optval_ptr, ms);
        *optlen = 4;
        return X_STATUS_SUCCESS;
      }
      default:
        if (optname == static_cast<uint32_t>(~0x0080u)) {
          // SO_DONTLINGER — return inverse of linger enabled
          asio::socket_base::linger opt;
          get_opt(opt);
          if (!ec) {
            xe::store_and_swap<int32_t>(optval_ptr, opt.enabled() ? 0 : 1);
            *optlen = 4;
          }
        } else if (optname == static_cast<uint32_t>(~0x0004u)) {
          // SO_EXCLUSIVEADDRUSE — return inverse of reuse_address
          asio::socket_base::reuse_address opt;
          get_opt(opt);
          if (!ec) {
            xe::store_and_swap<int32_t>(optval_ptr, opt.value() ? 0 : 1);
            *optlen = 4;
          }
        } else {
          XELOGW("XSocket::GetOption: unknown SOL_SOCKET option {:04X}",
                 optname);
          return X_STATUS_SUCCESS;
        }
        break;
    }
  } else if (level == 0x6) {
    // IPPROTO_TCP level
    switch (optname) {
      case 0x0001: {  // TCP_NODELAY
        asio::ip::tcp::no_delay opt;
        get_opt(opt);
        if (!ec) {
          xe::store_and_swap<int32_t>(optval_ptr, opt.value() ? 1 : 0);
          *optlen = 4;
        }
        break;
      }
      default:
        XELOGW("XSocket::GetOption: unknown TCP option {:04X}", optname);
        return X_STATUS_SUCCESS;
    }
  } else {
    XELOGW("XSocket::GetOption: unknown level {:04X} option {:04X}", level,
           optname);
    return X_STATUS_SUCCESS;
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                            uint32_t optlen) {
  if (level == 0xFFFF && (optname == 0x5801 || optname == 0x5802)) {
    // Disable socket encryption
    secure_ = false;
    return X_STATUS_SUCCESS;
  }

  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // Helper to set a typed ASIO option on whichever socket is active
  auto set_opt = [&](auto option) {
    if (tcp_socket_)
      tcp_socket_->set_option(option, ec);
    else if (udp_socket_)
      udp_socket_->set_option(option, ec);
  };

  // Read the guest big-endian option value (most options are a single int32)
  int32_t guest_value = 0;
  if (optlen >= 4) {
    guest_value = xe::load_and_swap<int32_t>(optval_ptr);
  } else if (optlen >= 2) {
    guest_value = xe::load_and_swap<int16_t>(optval_ptr);
  } else if (optlen >= 1) {
    guest_value = *static_cast<uint8_t*>(optval_ptr);
  }

  if (level == 0xFFFF) {
    // SOL_SOCKET level
    switch (optname) {
      case 0x0004:  // SO_REUSEADDR
        set_opt(asio::socket_base::reuse_address(guest_value != 0));
        break;
      case 0x0020:  // SO_BROADCAST
        broadcast_socket_ = (guest_value != 0);
        set_opt(asio::socket_base::broadcast(guest_value != 0));
        // Bind the socket to the systemlink interface so broadcasts go out
        // the correct NIC. If the socket is bound to 0.0.0.0 (any), the OS
        // picks an interface which may be wrong.
        if (guest_value != 0 && udp_socket_) {
          asio::error_code ep_ec;
          auto cur_ep = udp_socket_->local_endpoint(ep_ec);
          bool bound_to_any =
              !ep_ec && cur_ep.address() == asio::ip::address_v4::any();
          if (bound_to_any) {
            auto adapter = xam::QueryActiveAdapter();
            if (adapter.found) {
              auto local_addr =
                  asio::ip::address_v4(ntohl(adapter.ip_addr));
              uint16_t cur_port = cur_ep.port();
              // Close and reopen to rebind to the specific interface
              auto native_nb = udp_socket_->non_blocking();
              udp_socket_->close(ep_ec);
              udp_socket_->open(asio::ip::udp::v4(), ep_ec);
              if (!ep_ec) {
                udp_socket_->set_option(
                    asio::socket_base::reuse_address(true), ep_ec);
                udp_socket_->set_option(
                    asio::socket_base::broadcast(true), ep_ec);
                udp_socket_->non_blocking(native_nb, ep_ec);
                asio::error_code bind_ec;
                udp_socket_->bind(
                    asio::ip::udp::endpoint(local_addr, cur_port), bind_ec);
                if (!bind_ec) {
                  bound_port_ =
                      udp_socket_->local_endpoint(bind_ec).port();
                  XELOGI(
                      "XSocket::SetOption: SO_BROADCAST rebound to {}:{}",
                      local_addr.to_string(), bound_port_);
                } else {
                  XELOGE(
                      "XSocket::SetOption: SO_BROADCAST rebind failed: {}",
                      bind_ec.message());
                }
              }
            }
          }
        }
        break;
      case 0x0080: {  // SO_LINGER
        // Guest sends {be<uint16_t> onoff, be<uint16_t> secs}
        uint16_t onoff = xe::load_and_swap<uint16_t>(optval_ptr);
        uint16_t secs = xe::load_and_swap<uint16_t>(
            static_cast<uint8_t*>(optval_ptr) + 2);
        set_opt(asio::socket_base::linger(onoff != 0, secs));
        break;
      }
      case 0x1001:  // SO_SNDBUF
        set_opt(asio::socket_base::send_buffer_size(guest_value));
        break;
      case 0x1002:  // SO_RCVBUF
        set_opt(asio::socket_base::receive_buffer_size(guest_value));
        break;
      case 0x1005:    // SO_SNDTIMEO
      case 0x1006: {  // SO_RCVTIMEO
        // No ASIO typed option — use native handle with byte-swapped value.
        // Guest sends timeout as milliseconds in big-endian int32.
        int native_handle_val = static_cast<int>(native_handle());
        if (native_handle_val == -1) {
          return X_STATUS_INVALID_HANDLE;
        }
        int native_optname =
            (optname == 0x1005) ? SO_SNDTIMEO : SO_RCVTIMEO;
        struct timeval tv;
        tv.tv_sec = guest_value / 1000;
        tv.tv_usec = (guest_value % 1000) * 1000;
        int ret = setsockopt(native_handle_val, SOL_SOCKET, native_optname,
                             reinterpret_cast<const char*>(&tv), sizeof(tv));
        if (ret < 0) {
          last_error_ = AsioErrorToWSAError(
              asio::error_code(errno, asio::error::get_system_category()));
          return X_STATUS_UNSUCCESSFUL;
        }
        return X_STATUS_SUCCESS;
      }
      default:
        if (optname == static_cast<uint32_t>(~0x0080u)) {
          // SO_DONTLINGER — disable linger
          set_opt(asio::socket_base::linger(false, 0));
        } else if (optname == static_cast<uint32_t>(~0x0004u)) {
          // SO_EXCLUSIVEADDRUSE — disable reuse
          set_opt(asio::socket_base::reuse_address(false));
        } else {
          XELOGW("XSocket::SetOption: unknown SOL_SOCKET option {:04X}",
                 optname);
          return X_STATUS_SUCCESS;
        }
        break;
    }
  } else if (level == 0x6) {
    // IPPROTO_TCP level
    switch (optname) {
      case 0x0001:  // TCP_NODELAY
        set_opt(asio::ip::tcp::no_delay(guest_value != 0));
        break;
      default:
        XELOGW("XSocket::SetOption: unknown TCP option {:04X}", optname);
        return X_STATUS_SUCCESS;
    }
  } else {
    XELOGW("XSocket::SetOption: unknown level {:04X} option {:04X}", level,
           optname);
    return X_STATUS_SUCCESS;
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    XELOGE("XSocket::SetOption: failed level={:04X} opt={:04X}: {}", level,
           optname, ec.message());
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::IOControl(uint32_t cmd, uint8_t* arg_ptr) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // FIONBIO - set non-blocking mode
  if (cmd == 0x8004667E) {
    uint32_t value = *reinterpret_cast<uint32_t*>(arg_ptr);
    bool non_blocking = (value != 0);

    if (tcp_socket_) {
      tcp_socket_->non_blocking(non_blocking, ec);
    } else if (udp_socket_) {
      udp_socket_->non_blocking(non_blocking, ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }
    return X_STATUS_SUCCESS;
  }

  // FIONREAD - get bytes available
  if (cmd == 0x4004667F) {
    size_t available = 0;
    if (tcp_socket_) {
      available = tcp_socket_->available(ec);
    } else if (udp_socket_) {
      available = udp_socket_->available(ec);
    }

    if (ec) {
      last_error_ = AsioErrorToWSAError(ec);
      return X_STATUS_UNSUCCESSFUL;
    }

    *reinterpret_cast<uint32_t*>(arg_ptr) = static_cast<uint32_t>(available);
    return X_STATUS_SUCCESS;
  }

  XELOGE("XSocket::IOControl: unsupported command {:08X}", cmd);
  return X_STATUS_INVALID_PARAMETER;
}

X_STATUS XSocket::Connect(N_XSOCKADDR* name, int name_len) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  auto* addr_in = reinterpret_cast<N_XSOCKADDR_IN*>(name);
  asio::ip::address_v4 addr(addr_in->sin_addr);
  uint16_t port = addr_in->sin_port;

  XELOGI("XSocket::Connect: connecting to {}:{}", addr.to_string(), port);

  asio::error_code ec;

  if (tcp_socket_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    XELOGI("XSocket::Connect: TCP connecting to {}:{}, socket is_open={}",
           addr.to_string(), port, tcp_socket_->is_open());
    tcp_socket_->connect(endpoint, ec);
    if (!ec) {
      asio::error_code verify_ec;
      auto local_ep = tcp_socket_->local_endpoint(verify_ec);
      auto remote_ep = tcp_socket_->remote_endpoint(verify_ec);
      XELOGI("XSocket::Connect: TCP success, local={}:{}, remote={}:{}, is_open={}",
             local_ep.address().to_string(), local_ep.port(),
             remote_ep.address().to_string(), remote_ep.port(),
             tcp_socket_->is_open());
    } else {
      XELOGE("XSocket::Connect: TCP failed, error={} ({})", ec.value(), ec.message());
    }
  } else if (udp_socket_) {
    // UDP "connect" just sets the default destination - doesn't actually connect
    XELOGI("XSocket::Connect: UDP connect (sets default destination)");
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->connect(endpoint, ec);
  }

  if (ec) {
    XELOGE("XSocket::Connect: error={} ({})", ec.value(), ec.message());
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Bind(N_XSOCKADDR_IN* name, int name_len) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  // On Linux, ports < 1024 require root privileges.
  // Remap to port + 10000 to avoid privilege issues.
  // On Windows, low ports work without admin, so skip remapping.
  // Note: N_XSOCKADDR_IN uses xe::be<> which auto-converts to/from host endian.
  const uint16_t original_port = static_cast<uint16_t>(name->sin_port);
#if XE_PLATFORM_LINUX
  if (original_port < 1024 && original_port != 0) {
    uint16_t new_port = original_port + 10000;
    name->sin_port = new_port;
    AddPortRemap(new_port, original_port);
    remapped_port_ = new_port;
    XELOGW("XSocket::Bind: port {} requires privileges, remapping to port {}",
           original_port, new_port);
  }
#endif

  asio::ip::address_v4 addr(name->sin_addr);
  uint16_t port = name->sin_port;

  // If SO_BROADCAST was already set and we're binding to 0.0.0.0, bind to the
  // systemlink interface instead so broadcasts go out the correct NIC.
  if (broadcast_socket_ && udp_socket_ &&
      addr == asio::ip::address_v4::any()) {
    auto adapter = xam::QueryActiveAdapter();
    if (adapter.found) {
      addr = asio::ip::address_v4(ntohl(adapter.ip_addr));
      XELOGI(
          "XSocket::Bind: broadcast socket binding to systemlink interface "
          "{}:{} instead of 0.0.0.0",
          addr.to_string(), port);
    }
  }

  asio::error_code ec;

  if (tcp_socket_) {
    asio::ip::tcp::endpoint endpoint(addr, port);
    tcp_socket_->bind(endpoint, ec);
  } else if (udp_socket_) {
    asio::ip::udp::endpoint endpoint(addr, port);
    udp_socket_->bind(endpoint, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  bound_ = true;

  // Get the actual bound port (important when binding to port 0)
  if (tcp_socket_) {
    bound_port_ = tcp_socket_->local_endpoint(ec).port();
  } else if (udp_socket_) {
    bound_port_ = udp_socket_->local_endpoint(ec).port();
  }

  if (ec) {
    bound_port_ = port;
  }

  // Log the final bind state
  if (remapped_port_ != 0) {
    XELOGI("XSocket::Bind: bound to {}:{} (game_port={}, remapped)",
           addr.to_string(), bound_port_, original_port);
  } else {
    XELOGI("XSocket::Bind: bound to {}:{}", addr.to_string(), bound_port_);
  }

  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::Listen(int backlog) {
  if (!tcp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  asio::error_code ec;

  // Get the local endpoint the socket was bound to
  auto local_ep = tcp_socket_->local_endpoint(ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  // Close the tcp_socket - we'll create an acceptor on the same endpoint
  tcp_socket_->close(ec);
  tcp_socket_.reset();

  // Create an acceptor and bind it to the same endpoint
  acceptor_.emplace(GetIoContext());
  acceptor_->open(asio::ip::tcp::v4(), ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    acceptor_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }

  // Set reuse_address before binding
  acceptor_->set_option(asio::socket_base::reuse_address(true), ec);
  if (ec) {
    XELOGE("XSocket::Listen: reuse_address failed: {}", ec.message());
  }

  XELOGI("XSocket::Listen: binding acceptor to {}:{}",
         local_ep.address().to_string(), local_ep.port());
  acceptor_->bind(local_ep, ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    acceptor_.reset();
    return X_STATUS_UNSUCCESSFUL;
  }

  // Start listening
  acceptor_->listen(backlog, ec);
  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  // Set non-blocking so accept() returns EWOULDBLOCK instead of blocking
  acceptor_->non_blocking(true, ec);
  if (ec) {
    XELOGE("XSocket::Listen: non_blocking failed: {}", ec.message());
  }

  // Log the actual local endpoint
  asio::error_code ep_ec;
  auto actual_ep = acceptor_->local_endpoint(ep_ec);
  if (ep_ec) {
    XELOGE("XSocket::Listen: local_endpoint failed: {}", ep_ec.message());
  } else {
    XELOGI("XSocket::Listen: listening on {}:{} (bound_port_={}), backlog={}",
           actual_ep.address().to_string(), actual_ep.port(), bound_port_,
           backlog);
  }
  return X_STATUS_SUCCESS;
}

object_ref<XSocket> XSocket::Accept(N_XSOCKADDR* name, int* name_len) {
  if (!acceptor_) {
    XELOGE("XSocket::Accept: no acceptor");
    return nullptr;
  }

  asio::error_code ec;

  // Log acceptor state
  XELOGI("XSocket::Accept: acceptor is_open={}", acceptor_->is_open());

  // Accept a new connection
  asio::ip::tcp::socket new_socket(GetIoContext());
  asio::ip::tcp::endpoint peer_endpoint;
  acceptor_->accept(new_socket, peer_endpoint, ec);

  if (ec) {
    // Only log non-WOULDBLOCK errors or occasionally log WOULDBLOCK to reduce spam
    if (ec.value() != 10035) {
      XELOGI("XSocket::Accept: error={} ({})", ec.value(), ec.message());
    }
    last_error_ = AsioErrorToWSAError(ec);
    if (name) {
      std::memset(name, 0, *name_len);
    }
    *name_len = 0;
    return nullptr;
  }

  // SUCCESS - we accepted a connection!
  XELOGI("XSocket::Accept: SUCCESS from {}:{}",
         peer_endpoint.address().to_string(), peer_endpoint.port());

  // Fill in the client address
  if (name && *name_len >= static_cast<int>(sizeof(sockaddr_in))) {
    sockaddr_in* addr = reinterpret_cast<sockaddr_in*>(name);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(peer_endpoint.port());
    addr->sin_addr.s_addr = htonl(peer_endpoint.address().to_v4().to_uint());
    std::memset(reinterpret_cast<char*>(addr) + 8, 0, 8);  // Zero sin_zero
  }
  *name_len = sizeof(sockaddr_in);

  // Create a kernel object to represent the new socket
  auto socket =
      object_ref<XSocket>(new XSocket(kernel_state_, std::move(new_socket)));
  socket->bound_ = true;

  return socket;
}

int XSocket::Shutdown(int how) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  asio::socket_base::shutdown_type shutdown_type;

  switch (how) {
    case 0:
      shutdown_type = asio::socket_base::shutdown_receive;
      break;
    case 1:
      shutdown_type = asio::socket_base::shutdown_send;
      break;
    case 2:
    default:
      shutdown_type = asio::socket_base::shutdown_both;
      break;
  }

  if (tcp_socket_) {
    tcp_socket_->shutdown(shutdown_type, ec);
  }
  // UDP sockets don't support shutdown (connectionless protocol)

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return 0;
}

int XSocket::Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    bytes_received =
        udp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  } else if (tcp_socket_) {
    bytes_received =
        tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                      N_XSOCKADDR_IN* from, uint32_t* from_len) {
  if (!udp_socket_ && !tcp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_received = 0;

  if (udp_socket_) {
    asio::ip::udp::endpoint sender_endpoint;
    bytes_received = udp_socket_->receive_from(asio::buffer(buf, buf_len),
                                               sender_endpoint, 0, ec);

    if (!ec && from) {
      uint16_t os_port = sender_endpoint.port();
      uint16_t game_port = UnremapPort(os_port);
      from->sin_family = AF_INET;
      from->sin_addr = sender_endpoint.address().to_v4().to_uint();
      from->sin_port = game_port;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
      if (os_port != game_port) {
        XELOGI("XSocket::RecvFrom: {} bytes from {}:{} (os_port={}, unmapped)",
               bytes_received, sender_endpoint.address().to_string(), game_port,
               os_port);
      } else {
        XELOGI("XSocket::RecvFrom: {} bytes from {}:{}", bytes_received,
               sender_endpoint.address().to_string(), game_port);
      }
    }
    if (from_len) {
      *from_len = sizeof(N_XSOCKADDR_IN);
    }
  } else if (tcp_socket_) {
    bytes_received =
        tcp_socket_->receive(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_received);
}

int XSocket::Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags) {
  if (!tcp_socket_ && !udp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  } else if (tcp_socket_) {
    bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

int XSocket::SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags,
                    N_XSOCKADDR_IN* to, uint32_t to_len) {
  if (!udp_socket_ && !tcp_socket_) {
    return -1;
  }

  asio::error_code ec;
  size_t bytes_sent = 0;

  if (udp_socket_) {
    if (to) {
      asio::ip::address_v4 addr(to->sin_addr);
      uint16_t game_port = to->sin_port;
      uint16_t os_port = game_port;

#if XE_PLATFORM_LINUX
      // Remap destination port to match Bind() remapping so that broadcasts
      // reach sockets that had their bind port shifted by +10000.
      if (game_port != 0 && game_port < 1024) {
        os_port = game_port + 10000;
      }
#endif

      asio::ip::udp::endpoint endpoint(addr, os_port);

      if (os_port != game_port) {
        XELOGI("XSocket::SendTo: {} bytes to {}:{} (os_port={}, mapped)",
               buf_len, addr.to_string(), game_port, os_port);
      } else {
        XELOGI("XSocket::SendTo: {} bytes to {}:{}", buf_len, addr.to_string(),
               game_port);
      }

      bytes_sent =
          udp_socket_->send_to(asio::buffer(buf, buf_len), endpoint, flags, ec);
    } else {
      // Send to connected endpoint
      bytes_sent = udp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
    }
  } else if (tcp_socket_) {
    bytes_sent = tcp_socket_->send(asio::buffer(buf, buf_len), flags, ec);
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return -1;
  }

  return static_cast<int>(bytes_sent);
}

bool XSocket::QueuePacket(uint32_t src_ip, uint16_t src_port,
                          const uint8_t* buf, size_t len) {
  packet* pkt = reinterpret_cast<packet*>(new uint8_t[sizeof(packet) + len]);
  pkt->src_ip = src_ip;
  pkt->src_port = src_port;

  pkt->data_len = static_cast<uint16_t>(len);
  std::memcpy(pkt->data, buf, len);

  std::lock_guard<std::mutex> lock(incoming_packet_mutex_);
  incoming_packets_.push(reinterpret_cast<uint8_t*>(pkt));

  // TODO: Limit on number of incoming packets?
  return true;
}

X_STATUS XSocket::GetSockName(uint8_t* buf, int* buf_len) {
  if (!tcp_socket_ && !udp_socket_ && !acceptor_) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (*buf_len < static_cast<int>(sizeof(XSOCKADDR_IN))) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  asio::error_code ec;
  auto* addr = reinterpret_cast<XSOCKADDR_IN*>(buf);
  std::memset(addr, 0, sizeof(XSOCKADDR_IN));

  if (tcp_socket_) {
    auto ep = tcp_socket_->local_endpoint(ec);
    if (!ec) {
      addr->sin_family = AF_INET;
      addr->sin_port = UnremapPort(ep.port());
      addr->sin_addr = ep.address().to_v4().to_uint();
    }
  } else if (udp_socket_) {
    auto ep = udp_socket_->local_endpoint(ec);
    if (!ec) {
      addr->sin_family = AF_INET;
      addr->sin_port = UnremapPort(ep.port());
      addr->sin_addr = ep.address().to_v4().to_uint();
    }
  } else if (acceptor_) {
    auto ep = acceptor_->local_endpoint(ec);
    if (!ec) {
      addr->sin_family = AF_INET;
      addr->sin_port = UnremapPort(ep.port());
      addr->sin_addr = ep.address().to_v4().to_uint();
    }
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  *buf_len = sizeof(XSOCKADDR_IN);
  return X_STATUS_SUCCESS;
}

X_STATUS XSocket::GetPeerName(uint8_t* buf, int* buf_len) {
  if (!tcp_socket_ && !udp_socket_) {
    return X_STATUS_INVALID_HANDLE;
  }

  if (*buf_len < static_cast<int>(sizeof(XSOCKADDR_IN))) {
    return X_STATUS_BUFFER_TOO_SMALL;
  }

  asio::error_code ec;
  auto* addr = reinterpret_cast<XSOCKADDR_IN*>(buf);
  std::memset(addr, 0, sizeof(XSOCKADDR_IN));

  if (tcp_socket_) {
    auto ep = tcp_socket_->remote_endpoint(ec);
    if (!ec) {
      addr->sin_family = AF_INET;
      addr->sin_port = ep.port();
      addr->sin_addr = ep.address().to_v4().to_uint();
    }
  } else if (udp_socket_) {
    auto ep = udp_socket_->remote_endpoint(ec);
    if (!ec) {
      addr->sin_family = AF_INET;
      addr->sin_port = ep.port();
      addr->sin_addr = ep.address().to_v4().to_uint();
    }
  }

  if (ec) {
    last_error_ = AsioErrorToWSAError(ec);
    return X_STATUS_UNSUCCESSFUL;
  }

  *buf_len = sizeof(XSOCKADDR_IN);
  return X_STATUS_SUCCESS;
}

size_t XSocket::GetBytesAvailable() const {
  asio::error_code ec;
  size_t avail = 0;
  if (tcp_socket_ && tcp_socket_->is_open()) {
    avail = tcp_socket_->available(ec);
  } else if (udp_socket_ && udp_socket_->is_open()) {
    avail = udp_socket_->available(ec);
  } else if (acceptor_ && acceptor_->is_open()) {
    // For listening sockets (acceptors), always report ready.
    // The game will call accept() which returns EWOULDBLOCK if no connection.
    // This matches BSD socket behavior where select() can return ready
    // but accept() can still fail with EWOULDBLOCK.
    avail = 1;
  }
  if (ec) {
    XELOGE("XSocket::GetBytesAvailable: port={} error: {}", bound_port_,
           ec.message());
  }
  if (avail > 0) {
    XELOGI("XSocket::GetBytesAvailable: port={} avail={}", bound_port_, avail);
  }
  return avail;
}

bool XSocket::IsWritable() const {
  // UDP is always writable (connectionless, datagram)
  if (udp_socket_ && udp_socket_->is_open()) {
    return true;
  }
  if (tcp_socket_ && tcp_socket_->is_open()) {
    asio::error_code ec;
    // Use wait() with zero timeout to check writability
    const_cast<asio::ip::tcp::socket*>(&*tcp_socket_)
        ->wait(asio::socket_base::wait_write, ec);
    return !ec;
  }
  return false;
}

uint32_t XSocket::XWSAGetLastError() const { return last_error_; }

void XSocket::CompleteOverlapped(XWSAOVERLAPPED* overlapped,
                                 uint32_t error_status,
                                 uint32_t bytes_transferred) {
  // During teardown, skip guest memory writes and event signaling — just
  // remove the op from the pending set so Close() can finish draining.
  if (closing_.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(pending_ops_mutex_);
    pending_ops_.erase(overlapped);
    return;
  }

  // Windows OVERLAPPED convention:
  //   Internal = NTSTATUS (0 = success, or error code)
  //   InternalHigh = bytes transferred
  overlapped->internal = error_status;
  overlapped->internal_high = bytes_transferred;

  // Signal Xbox event if provided
  if (overlapped->event_handle) {
    auto evt = kernel_state()->object_table()->LookupObject<XEvent>(
        overlapped->event_handle);
    if (evt) {
      evt->Set(0, false);
    }
  }

  // Remove from pending set
  std::lock_guard<std::mutex> lock(pending_ops_mutex_);
  pending_ops_.erase(overlapped);
}

int XSocket::WSARecvFrom(
    uint8_t* buf, uint32_t buf_len, uint32_t flags, XSOCKADDR_IN* from,
    XWSAOVERLAPPED* overlapped,
    std::vector<std::pair<uint8_t*, uint32_t>> scatter_buffers) {
  // No overlapped: do a normal synchronous receive
  if (!overlapped) {
    N_XSOCKADDR_IN native_from;
    uint32_t native_from_len = sizeof(native_from);

    // For scatter-gather, receive into a temp buffer to avoid overflowing
    // the first guest buffer (buf_len is the total across all buffers, but
    // buf only points to the first one).
    std::vector<uint8_t> temp_buf;
    uint8_t* recv_dst = buf;
    if (scatter_buffers.size() > 1) {
      temp_buf.resize(buf_len);
      recv_dst = temp_buf.data();
    }

    int ret = RecvFrom(recv_dst, buf_len, flags, from ? &native_from : nullptr,
                       from ? &native_from_len : nullptr);
    if (ret >= 0 && from) {
      // Convert N_XSOCKADDR_IN (native sin_family) to XSOCKADDR_IN (be<>)
      from->sin_family = native_from.sin_family;
      from->sin_port = UnremapPort(native_from.sin_port);
      from->sin_addr = native_from.sin_addr;
      std::memset(from->x_sin_zero, 0, sizeof(from->x_sin_zero));
    }
    // Scatter data into multiple guest buffers if needed
    if (ret > 0 && scatter_buffers.size() > 1) {
      uint32_t src_offset = 0;
      uint32_t copy_len = static_cast<uint32_t>(ret);
      for (auto& [dst, dst_len] : scatter_buffers) {
        if (src_offset >= copy_len) break;
        uint32_t chunk = std::min(dst_len, copy_len - src_offset);
        std::memcpy(dst, temp_buf.data() + src_offset, chunk);
        src_offset += chunk;
      }
    }
    return ret;
  }

  // Overlapped provided: go straight to async.
  // Don't try sync first — the socket may be blocking, which would hang.
  if (!udp_socket_ || !udp_socket_->is_open()) {
    // Only UDP async recv is supported, and socket must be open
    last_error_ = uint32_t(X_WSA_ERROR::X_WSAEINVAL);
    return -1;
  }

  // Check if this overlapped is already pending — don't issue duplicate
  // async_receive_from calls on the same socket (undefined in ASIO).
  {
    std::lock_guard<std::mutex> lock(pending_ops_mutex_);
    if (pending_ops_.count(overlapped)) {
      // Already pending, just return IO_PENDING again
      last_error_ = uint32_t(X_WSA_ERROR::X_WSA_IO_PENDING);
      return -1;
    }
  }

  EnsureIoThreadRunning();

  // Track pending operation
  {
    std::lock_guard<std::mutex> lock(pending_ops_mutex_);
    pending_ops_.insert(overlapped);
  }

  // Initialize overlapped as pending (STATUS_PENDING = 0x103)
  overlapped->internal = 0x103;
  overlapped->internal_high = 0;

  // Allocate intermediate buffer and endpoint for async lifetime
  auto recv_buffer = std::make_shared<std::vector<uint8_t>>(buf_len);
  auto sender_endpoint = std::make_shared<asio::ip::udp::endpoint>();

  // Capture object_ref to keep socket alive during async op
  auto self = retain_object(this);

  // Capture guest pointers for completion — these point to guest memory
  // which persists until the overlapped operation completes.
  uint8_t* guest_buf = buf;
  uint32_t guest_buf_len = buf_len;
  XSOCKADDR_IN* guest_from = from;
  XWSAOVERLAPPED* guest_overlapped = overlapped;

  // Move scatter_buffers into shared_ptr for async lambda capture
  auto scatter =
      std::make_shared<std::vector<std::pair<uint8_t*, uint32_t>>>(
          std::move(scatter_buffers));

  udp_socket_->async_receive_from(
      asio::buffer(*recv_buffer), *sender_endpoint,
      [self, recv_buffer, sender_endpoint, guest_buf, guest_buf_len,
       guest_from, guest_overlapped,
       scatter](const asio::error_code& ec, size_t bytes_received) {
        // During teardown, skip all guest memory access.
        if (self->closing_.load(std::memory_order_acquire)) {
          self->CompleteOverlapped(guest_overlapped, 0, 0);
          return;
        }
        if (ec) {
          uint32_t wsa_err = AsioErrorToWSAError(ec);
          XELOGI("XSocket::WSARecvFrom async complete: error {}", ec.message());
          self->CompleteOverlapped(guest_overlapped, wsa_err, 0);
          return;
        }

        uint32_t copy_len = static_cast<uint32_t>(
            std::min(bytes_received, static_cast<size_t>(guest_buf_len)));

        if (scatter->size() > 1) {
          // Scatter received data across multiple guest buffers
          uint32_t src_offset = 0;
          for (auto& [dst, dst_len] : *scatter) {
            if (src_offset >= copy_len) break;
            uint32_t chunk = std::min(dst_len, copy_len - src_offset);
            std::memcpy(dst, recv_buffer->data() + src_offset, chunk);
            src_offset += chunk;
          }
        } else {
          // Single buffer — copy directly
          std::memcpy(guest_buf, recv_buffer->data(), copy_len);
        }

        // Fill from address if requested.
        // guest_from points to guest XSOCKADDR_IN memory with be<> fields,
        // so assignments go through be<> operator= which handles byte swap.
        uint16_t os_port = sender_endpoint->port();
        uint16_t game_port = UnremapPort(os_port);
        if (guest_from) {
          guest_from->sin_family = AF_INET;
          guest_from->sin_addr =
              sender_endpoint->address().to_v4().to_uint();
          guest_from->sin_port = game_port;
          std::memset(guest_from->x_sin_zero, 0,
                      sizeof(guest_from->x_sin_zero));
        }

        // Log first 16 bytes for protocol debugging
        std::string hex;
        for (uint32_t i = 0; i < std::min(copy_len, 16u); i++) {
          hex += fmt::format("{:02X} ", recv_buffer->data()[i]);
        }
        if (os_port != game_port) {
          XELOGI(
              "XSocket::WSARecvFrom async complete: {} bytes from {}:{} "
              "(os_port={}, unmapped) [{}]",
              copy_len, sender_endpoint->address().to_string(), game_port,
              os_port, hex);
        } else {
          XELOGI(
              "XSocket::WSARecvFrom async complete: {} bytes from {}:{} [{}]",
              copy_len, sender_endpoint->address().to_string(), game_port, hex);
        }
        self->CompleteOverlapped(guest_overlapped, 0, copy_len);
      });

  last_error_ = uint32_t(X_WSA_ERROR::X_WSA_IO_PENDING);
  return -1;
}

int XSocket::WSASendTo(const uint8_t* buf, uint32_t buf_len, uint32_t flags,
                        N_XSOCKADDR_IN* to, uint32_t to_len,
                        XWSAOVERLAPPED* overlapped) {
  if (!udp_socket_ && !tcp_socket_) {
    last_error_ = uint32_t(X_WSA_ERROR::X_WSAENOTSOCK);
    return -1;
  }

  // No overlapped: do a normal synchronous send
  if (!overlapped) {
    return SendTo(const_cast<uint8_t*>(buf), buf_len, flags, to, to_len);
  }

  // Overlapped provided: go straight to async
  if (!udp_socket_ || !udp_socket_->is_open()) {
    last_error_ = uint32_t(X_WSA_ERROR::X_WSAEINVAL);
    return -1;
  }

  // Copy guest data into an intermediate buffer for async lifetime
  auto send_buffer = std::make_shared<std::vector<uint8_t>>(buf, buf + buf_len);

  // Build endpoint from destination address
  asio::ip::udp::endpoint endpoint;
  uint16_t game_port = 0;
  uint16_t os_port = 0;
  if (to) {
    asio::ip::address_v4 addr(to->sin_addr);
    game_port = to->sin_port;
    os_port = game_port;

#if XE_PLATFORM_LINUX
    // Remap destination port to match Bind() remapping so that broadcasts
    // reach sockets that had their bind port shifted by +10000.
    if (game_port != 0 && game_port < 1024) {
      os_port = game_port + 10000;
    }
#endif

    endpoint = asio::ip::udp::endpoint(addr, os_port);

    if (os_port != game_port) {
      XELOGI("XSocket::WSASendTo: {} bytes to {}:{} (os_port={}, mapped)",
             buf_len, addr.to_string(), game_port, os_port);
    } else {
      XELOGI("XSocket::WSASendTo: {} bytes to {}:{}", buf_len, addr.to_string(),
             game_port);
    }
  }

  EnsureIoThreadRunning();

  // Track pending operation
  {
    std::lock_guard<std::mutex> lock(pending_ops_mutex_);
    pending_ops_.insert(overlapped);
  }

  // Initialize overlapped as pending (STATUS_PENDING = 0x103)
  overlapped->internal = 0x103;
  overlapped->internal_high = 0;

  auto self = retain_object(this);
  XWSAOVERLAPPED* guest_overlapped = overlapped;

  if (to) {
    udp_socket_->async_send_to(
        asio::buffer(*send_buffer), endpoint,
        [self, send_buffer,
         guest_overlapped](const asio::error_code& ec, size_t bytes_sent) {
          if (ec) {
            uint32_t wsa_err = AsioErrorToWSAError(ec);
            XELOGI("XSocket::WSASendTo async complete: error {}", ec.message());
            self->CompleteOverlapped(guest_overlapped, wsa_err, 0);
            return;
          }

          XELOGI("XSocket::WSASendTo async complete: {} bytes", bytes_sent);
          self->CompleteOverlapped(guest_overlapped, 0,
                                   static_cast<uint32_t>(bytes_sent));
        });
  } else {
    udp_socket_->async_send(
        asio::buffer(*send_buffer),
        [self, send_buffer,
         guest_overlapped](const asio::error_code& ec, size_t bytes_sent) {
          if (ec) {
            uint32_t wsa_err = AsioErrorToWSAError(ec);
            self->CompleteOverlapped(guest_overlapped, wsa_err, 0);
            return;
          }
          self->CompleteOverlapped(guest_overlapped, 0,
                                   static_cast<uint32_t>(bytes_sent));
        });
  }

  last_error_ = uint32_t(X_WSA_ERROR::X_WSA_IO_PENDING);
  return -1;
}

void XSocket::WSACancelOverlappedIO() {
  asio::error_code ec;
  if (udp_socket_ && udp_socket_->is_open()) {
    udp_socket_->cancel(ec);
  }
  if (tcp_socket_ && tcp_socket_->is_open()) {
    tcp_socket_->cancel(ec);
  }
  // ASIO will fire pending handlers with operation_aborted,
  // which CompleteOverlapped handles via the error status.
}

bool XSocket::IsOverlappedPending(XWSAOVERLAPPED* overlapped) const {
  std::lock_guard<std::mutex> lock(pending_ops_mutex_);
  return pending_ops_.count(overlapped) > 0;
}

}  // namespace kernel
}  // namespace xe

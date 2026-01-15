/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xsession.h"

#include <cstring>

#include "xenia/base/logging.h"
#include "xenia/kernel/xam/net_utils.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
/*
 * Most of the structs below were found in the Source SDK, provided as stubs.
 * Specifically, they can be found in the Source 2007 SDK and the Alien Swarm
 * Source SDK. Both are available on Steam for free. A GitHub mirror of the
 * Alien Swarm SDK can be found here:
 * https://github.com/NicolasDe/AlienSwarm/blob/master/src/common/xbox/xboxstubs.h
 */

struct XGI_XUSER_ACHIEVEMENT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> achievement_id;
};
static_assert_size(XGI_XUSER_ACHIEVEMENT, 0x8);

struct XGI_XUSER_GET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;  // If xuid is 0 then user_index is used.
  xe::be<uint32_t>
      property_size_ptr;  // Normally filled with sizeof(XUSER_PROPERTY), with
                          // exception of binary and wstring type.
  xe::be<uint32_t> context_address;
  xe::be<uint32_t> property_address;
};
static_assert_size(XGI_XUSER_GET_PROPERTY, 0x20);

struct XGI_XUSER_SET_CONTEXT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert_size(XGI_XUSER_SET_CONTEXT, 0x18);

struct XGI_XUSER_SET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> property_id;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_address;
};
static_assert_size(XGI_XUSER_SET_PROPERTY, 0x20);

struct XUSER_STATS_VIEW {
  xe::be<uint32_t> ViewId;
  xe::be<uint32_t> TotalViewRows;
  xe::be<uint32_t> NumRows;
  xe::be<uint32_t> pRows;
};

struct XUSER_STATS_COLUMN {
  xe::be<uint16_t> ColumnId;
  X_USER_DATA Value;
};

struct XUSER_STATS_RESET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> view_id;
};

struct XUSER_ANID {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> cchAnIdBuffer;
  xe::be<uint32_t> pszAnIdBuffer;
  xe::be<uint32_t> value_const;  // 1
};

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_CONTEXT));
      const XGI_XUSER_SET_CONTEXT* xgi_context =
          reinterpret_cast<const XGI_XUSER_SET_CONTEXT*>(buffer);

      XELOGD("XGIUserSetContext({:08X}, ID: {:08X}, Value: {:08X})",
             xgi_context->user_index.get(),
             xgi_context->context.context_id.get(),
             xgi_context->context.value.get());

      UserProfile* user = nullptr;
      if (xgi_context->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_context->xuid);
      } else {
        user =
            kernel_state_->xam_state()->GetUserProfile(xgi_context->user_index);
      }

      if (user) {
        kernel_state_->xam_state()->user_tracker()->UpdateContext(
            user->xuid(), xgi_context->context.context_id,
            xgi_context->context.value);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_PROPERTY));
      const XGI_XUSER_SET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_SET_PROPERTY*>(buffer);

      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})",
             xgi_property->user_index.get(), xgi_property->property_id.get(),
             xgi_property->data_size.get(), xgi_property->data_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (user) {
        Property property(
            xgi_property->property_id,
            Property::get_valid_data_size(xgi_property->property_id,
                                          xgi_property->data_size),
            memory_->TranslateVirtual<uint8_t*>(xgi_property->data_address));

        kernel_state_->xam_state()->user_tracker()->AddProperty(user->xuid(),
                                                                &property);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_ACHIEVEMENT));
      uint32_t achievement_count = xe::load_and_swap<uint32_t>(buffer + 0);
      uint32_t achievements_ptr = xe::load_and_swap<uint32_t>(buffer + 4);
      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})", achievement_count,
             achievements_ptr);

      auto* achievement =
          memory_->TranslateVirtual<XGI_XUSER_ACHIEVEMENT*>(achievements_ptr);
      for (uint32_t i = 0; i < achievement_count; i++, achievement++) {
        kernel_state_->achievement_manager()->EarnAchievement(
            achievement->user_index, kernel_state_->title_id(),
            achievement->achievement_id);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_xuid = xe::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);

      XELOGI(
          "XSessionCreate: session={:08X}, flags={:08X}, public_slots={}, "
          "private_slots={}, xuid={:08X}, info={:08X}, nonce={:08X}",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid,
          session_info_ptr, nonce_ptr);

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Allow offline and system link session creation, but do not allow
      // Xbox Live featured session creation.
      constexpr uint32_t HOST = 0x01;
      constexpr uint32_t PEER_NETWORK = 0x20;  // System Link
      constexpr uint32_t SYSTEMLINK_ALLOWED = HOST | PEER_NETWORK;
      if (flags & ~SYSTEMLINK_ALLOWED) {
        XELOGI("XSessionCreate: rejected, flags {:08X} require Xbox Live",
               flags);
        return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
      }

      // Fill in session info with our local address for system link
      if (session_info_ptr) {
        // XSESSION_INFO structure:
        // 0x00: XNKID sessionID (8 bytes)
        // 0x08: XNKEY keyExchangeKey (16 bytes)
        // 0x18: XNADDR hostAddress (36 bytes)
        //   XNADDR layout:
        //     0x00: in_addr ina (4 bytes) - IP address
        //     0x04: in_addr inaOnline (4 bytes) - Online IP
        //     0x08: uint16_t wPortOnline (2 bytes) - Online port
        //     0x0A: uint8_t abEnet[6] - MAC address
        //     0x10: uint8_t abOnline[20] - Online ID
        auto session_info = memory_->TranslateVirtual(session_info_ptr);

        // Generate a random session ID
        for (int i = 0; i < 8; i++) {
          session_info[i] = static_cast<uint8_t>(rand() & 0xFF);
        }

        // Generate a random key exchange key
        for (int i = 0; i < 16; i++) {
          session_info[8 + i] = static_cast<uint8_t>(rand() & 0xFF);
        }

        // Fill in host address with local network info
        auto host_addr = session_info + 24;  // Offset to XNADDR

        // Query the local adapter for IP and MAC
        auto adapter = QueryActiveAdapter();
        if (adapter.found) {
          // ina at offset 0 (4 bytes, network byte order)
          std::memcpy(host_addr, &adapter.ip_addr, 4);
          // abEnet at offset 10 (0x0A)
          std::memcpy(host_addr + 10, adapter.mac_addr, 6);
        }

        XELOGI("XSessionCreate: initialized session info at {:08X}",
               session_info_ptr);
      }

      // Fill in nonce if provided
      if (nonce_ptr) {
        auto nonce = memory_->TranslateVirtual(nonce_ptr);
        for (int i = 0; i < 8; i++) {
          nonce[i] = static_cast<uint8_t>(rand() & 0xFF);
        }
        XELOGI("XSessionCreate: initialized nonce at {:08X}", nonce_ptr);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      XELOGI("XSessionDelete: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t array_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array_ptr = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t indices_array_ptr = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      XELOGI(
          "XSessionJoin: session={:08X}, count={}, xuids={:08X}, "
          "indices={:08X}, private_slots={:08X}",
          session_ptr, array_count, xuid_array_ptr, indices_array_ptr,
          private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0013: {
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t array_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t xuid_array_ptr = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t indices_array_ptr = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      XELOGI(
          "XSessionLeave: session={:08X}, count={}, xuids={:08X}, "
          "indices={:08X}, private_slots={:08X}, buffer_len={:08X}",
          session_ptr, array_count, xuid_array_ptr, indices_array_ptr,
          private_slots_array, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      XELOGI("XSessionStart: buffer={:08X}", buffer_ptr);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      XELOGI("XSessionEnd: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0016: {
      XELOGI("XSessionSearch: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0017: {
      XELOGI("XSessionModify: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      XELOGI("XSessionMigrateHost: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      XELOGI("XSessionLeaveLocal: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B001A: {
      XELOGI("XSessionLeaveRemote: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B001B: {
      XELOGI("XSessionArbitrationRegister: buffer={:08X}, len={:08X}",
             buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      XELOGI("XSessionGetDetails: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B001D: {
      XELOGI("XSessionFlushStats: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0021: {
      XELOGD("XUserReadStats");

      struct XUserReadStats {
        xe::be<uint32_t> titleId;
        xe::be<uint32_t> xuids_count;
        xe::be<uint32_t> xuids_guest_address;
        xe::be<uint32_t> specs_count;
        xe::be<uint32_t> specs_guest_address;
        xe::be<uint32_t> results_size;
        xe::be<uint32_t> results_guest_address;
      }* data = reinterpret_cast<XUserReadStats*>(buffer);

      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      XELOGD("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      // Used in 5451082A, 5553081E
      // XUserGetCachedANID
      XELOGI("XUserGetANID({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x000B0041: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_GET_PROPERTY));
      const XGI_XUSER_GET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_GET_PROPERTY*>(buffer);

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (!user) {
        XELOGD(
            "XGIUserGetProperty - Invalid user provided: Index: {:08X} XUID: "
            "{:16X}",
            xgi_property->user_index.get(), xgi_property->xuid.get());
        return X_E_NOTFOUND;
      }

      // Process context
      if (xgi_property->context_address) {
        XUSER_CONTEXT* context = memory_->TranslateVirtual<XUSER_CONTEXT*>(
            xgi_property->context_address);

        XELOGD("XGIUserGetProperty - Context requested: {:08X} XUID: {:16X}",
               context->context_id.get(), user->xuid());

        auto context_value =
            kernel_state_->xam_state()->user_tracker()->GetUserContext(
                user->xuid(), context->context_id);

        if (!context_value) {
          return X_E_INVALIDARG;
        }

        context->value = context_value.value();
        return X_E_SUCCESS;
      }

      if (!xgi_property->property_size_ptr || !xgi_property->property_address) {
        return X_E_INVALIDARG;
      }

      // Process property
      XUSER_PROPERTY* property = memory_->TranslateVirtual<XUSER_PROPERTY*>(
          xgi_property->property_address);

      XELOGD("XGIUserGetProperty - Property requested: {:08X} XUID: {:16X}",
             property->property_id.get(), user->xuid());

      return kernel_state_->xam_state()->user_tracker()->GetProperty(
          user->xuid(),
          memory_->TranslateVirtual<uint32_t*>(xgi_property->property_size_ptr),
          property);
    }
    case 0x000B0071: {
      XELOGD("ContentEnumerate::ResetEnumerator({:08X}, {:08X}), unimplemented",
             buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

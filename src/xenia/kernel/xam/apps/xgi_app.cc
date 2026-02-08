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
  XELOGI("XGI::DispatchMessageSync({:08X}, {:08X}, {:08X})", message,
         buffer_ptr, buffer_length);
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
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_CREATE));
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      XGI_SESSION_CREATE* data =
          reinterpret_cast<XGI_SESSION_CREATE*>(buffer);

      XELOGI(
          "XSessionCreate: session={:08X}, flags={:08X}, public_slots={}, "
          "private_slots={}, user_index={}, info={:08X}, nonce={:08X}",
          (uint32_t)data->obj_ptr, (uint32_t)data->flags,
          (uint32_t)data->num_slots_public, (uint32_t)data->num_slots_private,
          (uint32_t)data->user_index, (uint32_t)data->session_info_ptr,
          (uint32_t)data->nonce_ptr);

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Allow offline and system link session creation, but do not allow
      // Xbox Live featured session creation.
      uint32_t flags = data->flags;
      constexpr uint32_t HOST = 0x01;
      constexpr uint32_t PEER_NETWORK = 0x20;  // System Link
      constexpr uint32_t SYSTEMLINK_ALLOWED = HOST | PEER_NETWORK;
      if (flags & ~SYSTEMLINK_ALLOWED) {
        XELOGI("XSessionCreate: rejected, flags {:08X} require Xbox Live",
               flags);
        return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
      }

      // Get XSession object from guest object pointer
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        XELOGE("XSessionCreate: invalid session object");
        return X_STATUS_INVALID_HANDLE;
      }

      // Log session info contents for debugging
      if (data->session_info_ptr) {
        auto session_info =
            memory_->TranslateVirtual<XSESSION_INFO*>(data->session_info_ptr);
        XELOGI(
            "XSessionCreate: session_info hostAddress.ina={:08X}, "
            "MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}",
            (uint32_t)session_info->hostAddress.ina,
            session_info->hostAddress.abEnet[0],
            session_info->hostAddress.abEnet[1],
            session_info->hostAddress.abEnet[2],
            session_info->hostAddress.abEnet[3],
            session_info->hostAddress.abEnet[4],
            session_info->hostAddress.abEnet[5]);
      }

      auto result =
          session->CreateSession(data->user_index, data->num_slots_public,
                                 data->num_slots_private, data->flags,
                                 data->session_info_ptr, data->nonce_ptr);

      return result;
    }
    case 0x000B0011: {
      // XSessionDelete
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_STATE));
      XELOGI("XSessionDelete: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }

      return session->DeleteSession();
    }
    case 0x000B0012: {
      // XSessionJoinLocal
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MANAGE));
      XGI_SESSION_MANAGE* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);

      XELOGI(
          "XSessionJoinLocal: session={:08X}, count={}, xuids={:08X}, "
          "indices={:08X}, private_slots={:08X}",
          (uint32_t)data->obj_ptr, (uint32_t)data->array_count,
          (uint32_t)data->xuid_array_ptr, (uint32_t)data->indices_array_ptr,
          (uint32_t)data->private_slots_array_ptr);

      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->JoinSession(data);
    }
    case 0x000B0013: {
      // XSessionLeaveLocal
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MANAGE));
      XGI_SESSION_MANAGE* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);

      XELOGI(
          "XSessionLeaveLocal: session={:08X}, count={}, xuids={:08X}, "
          "indices={:08X}, private_slots={:08X}",
          (uint32_t)data->obj_ptr, (uint32_t)data->array_count,
          (uint32_t)data->xuid_array_ptr, (uint32_t)data->indices_array_ptr,
          (uint32_t)data->private_slots_array_ptr);

      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->LeaveSession(data);
    }
    case 0x000B0014: {
      // XSessionStart
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_STATE));
      XELOGI("XSessionStart: buffer={:08X}", buffer_ptr);

      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->StartSession();
    }
    case 0x000B0015: {
      // XSessionEnd
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_STATE));
      XELOGI("XSessionEnd: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      XGI_SESSION_STATE* data = reinterpret_cast<XGI_SESSION_STATE*>(buffer);
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->EndSession();
    }
    case 0x000B0016: {
      // XSessionSearch
      XELOGW("XSessionSearch: not implemented");
      return X_E_SUCCESS;
    }
    case 0x000B0017: {
      // XSessionModify
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MODIFY));
      XELOGI("XSessionModify: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      XGI_SESSION_MODIFY* data = reinterpret_cast<XGI_SESSION_MODIFY*>(buffer);
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->ModifySession(data);
    }
    case 0x000B0018: {
      XELOGI("XSessionMigrateHost: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B0019: {
      // XSessionLeaveLocal (alternative entry point, same as 0x000B0013)
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MANAGE));
      XGI_SESSION_MANAGE* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);

      XELOGI("XSessionLeaveLocal: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->LeaveSession(data);
    }
    case 0x000B001A: {
      // XSessionLeaveRemote
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_MANAGE));
      XGI_SESSION_MANAGE* data = reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);

      XELOGI("XSessionLeaveRemote: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        return X_STATUS_INVALID_HANDLE;
      }
      return session->LeaveSession(data);
    }
    case 0x000B001B: {
      XELOGI("XSessionArbitrationRegister: buffer={:08X}, len={:08X}",
             buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      // XSessionGetDetails
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_SESSION_DETAILS));
      XELOGI("XSessionGetDetails: buffer={:08X}, len={:08X}", buffer_ptr,
             buffer_length);

      XGI_SESSION_DETAILS* data =
          reinterpret_cast<XGI_SESSION_DETAILS*>(buffer);
      uint8_t* obj_ptr = memory_->TranslateVirtual<uint8_t*>(data->obj_ptr);
      auto session =
          XObject::GetNativeObject<XSession>(kernel_state_, obj_ptr);
      if (!session) {
        XELOGE("XSessionGetDetails: invalid session object");
        return X_STATUS_INVALID_HANDLE;
      }
      return session->GetSessionDetails(data);
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

      uint32_t results_addr =
          static_cast<uint32_t>(data->results_guest_address);
      if (!results_addr) {
        return X_E_INVALIDARG;
      }

      // X_USER_STATS_READ_RESULTS: {num_views: u32, views_ptr: u32}
      struct StatsReadResults {
        xe::be<uint32_t> num_views;
        xe::be<uint32_t> views_ptr;
      };

      // X_USER_STATS_VIEW: {view_id: u32, total_view_rows: u32, num_rows: u32,
      // rows_ptr: u32}
      struct StatsView {
        xe::be<uint32_t> view_id;
        xe::be<uint32_t> total_view_rows;
        xe::be<uint32_t> num_rows;
        xe::be<uint32_t> rows_ptr;
      };

      auto* results =
          kernel_state_->memory()->TranslateVirtual<StatsReadResults*>(
              results_addr);

      uint32_t specs_count = static_cast<uint32_t>(data->specs_count);
      uint32_t specs_addr = static_cast<uint32_t>(data->specs_guest_address);

      if (specs_count == 0 || !specs_addr) {
        results->num_views = 0;
        results->views_ptr = 0;
        return X_E_SUCCESS;
      }

      // Allocate guest memory for the views array
      uint32_t views_size = specs_count * static_cast<uint32_t>(sizeof(StatsView));
      uint32_t views_guest =
          kernel_state_->memory()->SystemHeapAlloc(views_size);
      auto* views =
          kernel_state_->memory()->TranslateVirtual<StatsView*>(views_guest);
      std::memset(views, 0, views_size);

      // Read spec view_ids - each spec starts with a view_id u32
      // X_USER_STATS_SPEC layout: {view_id: u32, num_column_ids: u32,
      // column_ids: u16[64]} = 0x88 bytes
      const uint32_t spec_stride = 0x88;
      auto* specs_base =
          kernel_state_->memory()->TranslateVirtual<uint8_t*>(specs_addr);
      for (uint32_t i = 0; i < specs_count; i++) {
        auto* spec_view_id = reinterpret_cast<xe::be<uint32_t>*>(
            specs_base + i * spec_stride);
        views[i].view_id = *spec_view_id;
        views[i].total_view_rows = 0;
        views[i].num_rows = 0;
        views[i].rows_ptr = 0;
      }

      results->num_views = specs_count;
      results->views_ptr = views_guest;

      return X_E_SUCCESS;
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

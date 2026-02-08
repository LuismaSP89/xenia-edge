/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include <cstring>

#include "third_party/fmt/include/fmt/format.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xenumerator.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/xbox.h"

#include <asio.hpp>

struct XONLINE_SERVICE_INFO {
  xe::be<uint32_t> id;
  in_addr ip;
  xe::be<uint16_t> port;
  xe::be<uint16_t> reserved;
};

// XLiveBase argument entry for parsing XLiveBase message arguments
struct X_ARGUMENT_ENTRY {
  xe::be<uint32_t> native_size;
  xe::be<uint64_t> argument_value_ptr;
};
static_assert_size(X_ARGUMENT_ENTRY, 0x10);

// Arguments for XFriendsCreateEnumerator (0x00058020)
struct X_CREATE_FRIENDS_ENUMERATOR {
  X_ARGUMENT_ENTRY user_index;
  X_ARGUMENT_ENTRY friends_starting_index;
  X_ARGUMENT_ENTRY friends_amount;
  X_ARGUMENT_ENTRY buffer_ptr;
  X_ARGUMENT_ENTRY handle_ptr;
};

// X_ONLINE_FRIEND structure for friends enumerator (0xC4 bytes)
// We only need this for the enumerator item size, not actual friend data
#pragma pack(push, 1)
struct X_ONLINE_FRIEND {
  xe::be<uint64_t> xuid;          // 0x00
  char gamertag[16];              // 0x08
  xe::be<uint32_t> state;         // 0x18
  uint8_t session_id[8];          // 0x1C (XNKID)
  xe::be<uint32_t> title_id;      // 0x24
  xe::be<uint64_t> ft_user_time;  // 0x28
  uint8_t xnkid_invite[8];        // 0x30 (XNKID)
  xe::be<uint64_t> game_invite_time;  // 0x38
  xe::be<uint32_t> cch_rich_presence; // 0x40
  char wsz_rich_presence[128];    // 0x44 (64 * sizeof(char16_t))
};
#pragma pack(pop)
static_assert_size(X_ONLINE_FRIEND, 0xC4);

// XLiveBase async task structures (from netplay's xnet.h)
struct BASE_ENDIAN_BUFFER {
  xe::be<uint32_t> BufferPtr;
  xe::be<uint32_t> BufferSize;
  xe::be<uint32_t> AvailableSize;
  xe::be<uint32_t> ConsumedSize;
  xe::be<int32_t> ReverseEndian;
};
static_assert_size(BASE_ENDIAN_BUFFER, 0x14);

struct XLIVE_ASYNC_TASK {
  xe::be<uint32_t> ordinal;
  xe::be<uint32_t> schema_data_ptr;
  xe::be<uint32_t> schema_index;
  xe::be<uint32_t> task_flags;
  xe::be<uint32_t> live_async_task_internal_ptr;
  xe::be<uint32_t> internal_task_size;
  xe::be<uint32_t> marshalled_request_ptr;
  xe::be<uint32_t> marshalled_request_size;
  xe::be<uint32_t> total_wire_buffer_size;
  xe::be<uint32_t> counter;
  xe::be<uint32_t> logon_id;
  xe::be<uint32_t> results_ptr;
  xe::be<uint32_t> results_size;
  BASE_ENDIAN_BUFFER wire_buffer;
  xe::be<uint32_t> overlapped_ptr;
};
static_assert_size(XLIVE_ASYNC_TASK, 0x4C);

struct XLIVEBASE_ASYNC_MESSAGE {
  xe::be<uint32_t> xlive_async_task_ptr;
  xe::be<uint64_t> current_numerator;
  xe::be<uint64_t> current_denominator;
  xe::be<uint64_t> last_numerator;
  xe::be<uint64_t> last_denominator;
};
static_assert_size(XLIVEBASE_ASYNC_MESSAGE, 0x28);

// Storage facility types for XStorageBuildServerPath
enum X_STORAGE_FACILITY : uint32_t {
  FACILITY_INVALID = 0,
  FACILITY_GAME_CLIP = 1,
  FACILITY_PER_TITLE = 2,
  FACILITY_PER_USER_TITLE = 3
};

// Args structure for XStorageBuildServerPath (message 0x00058035)
struct X_STORAGE_BUILD_SERVER_PATH {
  xe::be<uint32_t> user_index;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> storage_location;
  xe::be<uint32_t> storage_location_info_ptr;
  xe::be<uint32_t> storage_location_info_size;
  xe::be<uint32_t> file_name_ptr;
  xe::be<uint32_t> server_path_ptr;
  xe::be<uint32_t> server_path_length_ptr;
};
static_assert_size(X_STORAGE_BUILD_SERVER_PATH, 0x28);

// XOnlineGetTaskProgress argument structure (message 0x00058032)
struct X_GET_TASK_PROGRESS {
  xe::be<uint32_t> overlapped_ptr;
  xe::be<uint32_t> percent_complete_ptr;
  xe::be<uint32_t> numerator_ptr;
  xe::be<uint32_t> denominator_ptr;
};
static_assert_size(X_GET_TASK_PROGRESS, 0x10);

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  XELOGI("XLiveBase::DispatchMessageSync({:08X}, {:08X}, {:08X})", message,
         buffer_ptr, buffer_length);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      return 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      // Return NAT_OPEN for system link - matches netplay behavior
      xe::store_and_swap<uint32_t>(buffer, 1);  // XONLINE_NAT_OPEN
      return X_E_SUCCESS;
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058009: {
      // XContentGetMarketplaceCounts - returns marketplace offer counts
      XELOGD("XContentGetMarketplaceCounts({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058020: {
      // XFriendsCreateEnumerator - creates an enumerator for friends list
      // For system link (offline), we create an empty enumerator with 0 friends
      XELOGD("XFriendsCreateEnumerator({:08X}, {:08X})", buffer_ptr,
             buffer_length);

      if (!buffer_ptr || !buffer_length) {
        return X_E_INVALIDARG;
      }

      auto* friends_enum =
          kernel_state_->memory()->TranslateVirtual<X_CREATE_FRIENDS_ENUMERATOR*>(
              buffer_length);

      // Parse arguments
      uint32_t friends_amount = xe::load_and_swap<uint32_t>(
          kernel_state_->memory()->TranslateVirtual(static_cast<uint32_t>(
              friends_enum->friends_amount.argument_value_ptr)));
      uint32_t buffer_address =
          static_cast<uint32_t>(friends_enum->buffer_ptr.argument_value_ptr);
      uint32_t handle_address =
          static_cast<uint32_t>(friends_enum->handle_ptr.argument_value_ptr);

      if (!handle_address) {
        return X_E_INVALIDARG;
      }

      auto* handle_ptr =
          kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
              handle_address);
      // Set to 0 early in case we fail
      *handle_ptr = 0;

      if (!buffer_address) {
        return X_E_INVALIDARG;
      }

      auto* buffer_size_ptr =
          kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
              buffer_address);
      *buffer_size_ptr = 0;

      // Create an empty enumerator - games call XamEnumerate on the handle
      // Use friends_amount or default to 1 if 0
      size_t items_per_enum = friends_amount > 0 ? friends_amount : 1;
      auto e = make_object<XStaticEnumerator<X_ONLINE_FRIEND>>(kernel_state_,
                                                               items_per_enum);
      // Initialize with XLiveBase app_id (0xFC), message codes for enumerate
      auto result = e->Initialize(XUserIndexAny, app_id(), 0x58021, 0x58022, 0);
      if (XFAILED(result)) {
        return result;
      }

      // Don't add any items - this is an empty friends list for system link
      // The enumerator will return 0 items when enumerated

      // Write output: buffer size = items_per_enumerate * item_size
      uint32_t friends_buffer_size =
          static_cast<uint32_t>(e->items_per_enumerate() * e->item_size());
      *buffer_size_ptr = friends_buffer_size;

      // Write output: enumerator handle
      *handle_ptr = e->handle();

      return X_E_SUCCESS;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058017: {
      XELOGD("UserFindUsers({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058035: {
      // XStorageBuildServerPath - builds a server path for Xbox Live storage
      // Games use this to check online storage availability.
      // We build a fake local path so the game thinks storage is reachable.
      XELOGD("XStorageBuildServerPath({:08X}, {:08X})", buffer_ptr,
             buffer_length);

      if (!buffer_ptr) {
        return X_E_INVALIDARG;
      }

      auto* args =
          memory_->TranslateVirtual<X_STORAGE_BUILD_SERVER_PATH*>(buffer_ptr);

      if (!args->file_name_ptr || !args->server_path_length_ptr) {
        return X_E_INVALIDARG;
      }

      // Read the requested filename
      uint8_t* filename_ptr =
          memory_->TranslateVirtual<uint8_t*>(
              static_cast<uint32_t>(args->file_name_ptr));
      const std::u16string filename =
          xe::load_and_swap<std::u16string>(filename_ptr);
      const std::string filename_str = xe::to_utf8(filename);

      // Build storage type string for logging
      const char* storage_type = "Unknown";
      uint32_t storage_loc =
          static_cast<uint32_t>(args->storage_location);
      if (storage_loc == FACILITY_GAME_CLIP) storage_type = "Game Clip";
      else if (storage_loc == FACILITY_PER_TITLE) storage_type = "Per Title";
      else if (storage_loc == FACILITY_PER_USER_TITLE)
        storage_type = "Per User Title";

      // Look up user XUID - match netplay's logic
      uint64_t xuid = 0;
      uint32_t user_index = static_cast<uint32_t>(args->user_index);

      if (user_index == XUserIndexNone) {
        xuid = static_cast<uint64_t>(args->xuid);
      }

      bool xuid_required = (storage_loc == FACILITY_PER_USER_TITLE ||
                            storage_loc == FACILITY_GAME_CLIP);

      if (!xuid && xuid_required && user_index < XUserMaxUserCount) {
        auto* profile =
            kernel_state_->xam_state()->profile_manager()->GetProfile(
                static_cast<uint8_t>(user_index));
        if (profile) {
          xuid = profile->GetOnlineXUID();
        }
      }

      XELOGI(
          "XStorageBuildServerPath: Filename: {}, Storage Type: {}, XUID: "
          "{:016X}",
          filename_str, storage_type, xuid);

      // Build a fake server path that satisfies the game's online check
      std::string server_path_str;
      if (storage_loc == FACILITY_PER_TITLE) {
        server_path_str = fmt::format("xstorage/title/{:08X}/{}",
                                      kernel_state_->title_id(), filename_str);
      } else if (storage_loc == FACILITY_PER_USER_TITLE) {
        server_path_str =
            fmt::format("xstorage/user/{:016X}/title/{:08X}/{}", xuid,
                        kernel_state_->title_id(), filename_str);
      } else if (storage_loc == FACILITY_GAME_CLIP) {
        server_path_str =
            fmt::format("xstorage/clips/title/{:08X}/{:016X}/{}",
                        kernel_state_->title_id(), xuid, filename_str);
      } else {
        server_path_str = fmt::format("xstorage/{}", filename_str);
      }

      // Write the path to the output buffer as UTF-16 (byte-swapped)
      if (args->server_path_ptr) {
        const std::u16string server_path_u16 = xe::to_utf16(server_path_str);
        char16_t* server_path_out =
            memory_->TranslateVirtual<char16_t*>(
                static_cast<uint32_t>(args->server_path_ptr));
        xe::string_util::copy_and_swap_truncating(
            server_path_out, server_path_u16, 255);
      }

      // Write the path length
      uint32_t* server_path_length =
          memory_->TranslateVirtual<uint32_t*>(
              static_cast<uint32_t>(args->server_path_length_ptr));
      *server_path_length =
          xe::byte_swap<uint32_t>(
              static_cast<uint32_t>(server_path_str.size() + 1));

      XELOGI("XStorageBuildServerPath: Built path: {}", server_path_str);
      return X_E_SUCCESS;
    }
    case 0x00050008:
    case 0x00050009: {
      // XStorageDownloadToMemory - games download regulation/save files
      // Zero the results struct and return "file not found" (matching netplay
      // behavior for missing files). The game uses this error to know there's
      // no saved data and proceeds to create defaults.
      XELOGD("XStorageDownloadToMemory({:08X}, {:08X})", buffer_ptr,
             buffer_length);

      if (buffer_ptr) {
        auto* async_msg =
            memory_->TranslateVirtual<XLIVEBASE_ASYNC_MESSAGE*>(buffer_ptr);
        uint32_t task_addr =
            static_cast<uint32_t>(async_msg->xlive_async_task_ptr);
        if (task_addr) {
          auto* task =
              memory_->TranslateVirtual<XLIVE_ASYNC_TASK*>(task_addr);
          uint32_t results_addr = static_cast<uint32_t>(task->results_ptr);
          uint32_t results_size = static_cast<uint32_t>(task->results_size);
          if (results_addr && results_size) {
            uint8_t* results =
                memory_->TranslateVirtual<uint8_t*>(results_addr);
            std::memset(results, 0, results_size);
            XELOGI(
                "XStorageDownloadToMemory: zeroed {} bytes of results at "
                "{:08X}",
                results_size, results_addr);
          }
        }
      }

      return 0x807D0006;  // X_ONLINE_E_STORAGE_FILE_NOT_FOUND
    }
    case 0x0005000A: {
      // XStorageEnumerate - Xbox Live cloud storage enumeration
      // Return success with 0 items (matches netplay behavior)
      XELOGD("XStorageEnumerate({:08X}, {:08X})", buffer_ptr, buffer_length);

      if (buffer_ptr) {
        auto* async_msg =
            memory_->TranslateVirtual<XLIVEBASE_ASYNC_MESSAGE*>(buffer_ptr);
        uint32_t task_addr =
            static_cast<uint32_t>(async_msg->xlive_async_task_ptr);
        if (task_addr) {
          auto* task =
              memory_->TranslateVirtual<XLIVE_ASYNC_TASK*>(task_addr);
          uint32_t results_addr = static_cast<uint32_t>(task->results_ptr);
          uint32_t results_size = static_cast<uint32_t>(task->results_size);
          if (results_addr && results_size) {
            uint8_t* results =
                memory_->TranslateVirtual<uint8_t*>(results_addr);
            std::memset(results, 0, results_size);
            XELOGI("XStorageEnumerate: zeroed {} bytes of results at {:08X}",
                   results_size, results_addr);
          }
        }
      }

      return X_E_SUCCESS;
    }
    case 0x0005000B: {
      // XStorageUploadFromMemory - upload to Xbox Live storage
      // Return success (matches netplay - upload accepted)
      XELOGD("XStorageUploadFromMemory({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00050036: {
      XELOGD("XOnlineQuerySearch({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058032: {
      assert_true(!buffer_length);
      XELOGD("XOnlineGetTaskProgress({:08X}, {:08X})", buffer_ptr,
             buffer_length);

      if (!buffer_ptr) {
        return X_E_INVALIDARG;
      }

      auto* task_progress =
          memory_->TranslateVirtual<X_GET_TASK_PROGRESS*>(buffer_ptr);

      if (task_progress->percent_complete_ptr) {
        auto* percent_complete = memory_->TranslateVirtual<uint32_t*>(
            static_cast<uint32_t>(task_progress->percent_complete_ptr));
        *percent_complete = 100;
      }

      if (task_progress->numerator_ptr) {
        auto* numerator = memory_->TranslateVirtual<uint64_t*>(
            static_cast<uint32_t>(task_progress->numerator_ptr));
        *numerator = 100;
      }

      if (task_progress->denominator_ptr) {
        auto* denominator = memory_->TranslateVirtual<uint64_t*>(
            static_cast<uint32_t>(task_progress->denominator_ptr));
        *denominator = 100;
      }

      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

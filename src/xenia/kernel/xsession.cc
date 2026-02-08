/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xsession.h"

#include <cstring>
#include <random>

#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/net_utils.h"
#include "xenia/kernel/xam/xam_state.h"

namespace xe {
namespace kernel {

XSession::XSession(KernelState* kernel_state)
    : XObject(kernel_state, Type::Session) {}

XSession::~XSession() = default;

X_STATUS XSession::Initialize() {
  auto native_object = CreateNative(sizeof(X_KSESSION));
  if (!native_object) {
    return X_STATUS_NO_MEMORY;
  }

  auto guest_object = reinterpret_cast<X_KSESSION*>(native_object);
  guest_object->handle = handle();

  return X_STATUS_SUCCESS;
}

X_RESULT XSession::CreateSession(uint32_t user_index, uint32_t public_slots,
                                 uint32_t private_slots, uint32_t flags,
                                 uint32_t session_info_ptr,
                                 uint32_t nonce_ptr) {
  XELOGI(
      "XSession::CreateSession: user_index={}, public_slots={}, "
      "private_slots={}, flags={:08X}",
      user_index, public_slots, private_slots, flags);

  // Initialize local details
  local_details_.UserIndexHost = user_index;
  local_details_.GameType = 0;
  local_details_.GameMode = 0;
  local_details_.Flags = flags;
  local_details_.ActualMemberCount = 0;
  local_details_.ReturnedMemberCount = 0;
  local_details_.eState = XSESSION_STATE::LOBBY;
  local_details_.SessionMembers_ptr = 0;

  // Set slots - needed for all session types including SYSTEMLINK (0x20)
  // which doesn't include HOST flag. Consistent with netplay implementation.
  local_details_.MaxPublicSlots = public_slots;
  local_details_.MaxPrivateSlots = private_slots;
  local_details_.AvailablePublicSlots = public_slots;
  local_details_.AvailablePrivateSlots = private_slots;

  // Generate random nonce
  std::random_device rd;
  std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);
  uint64_t nonce = dist(rd);
  local_details_.Nonce = nonce;

  // Write nonce to guest if provided
  if (nonce_ptr) {
    auto nonce_guest =
        kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(nonce_ptr);
    *nonce_guest = nonce;
  }

  // Zero out session info by default
  std::memset(&local_details_.sessionInfo, 0, sizeof(XSESSION_INFO));

  // Fill session info only when hosting
  if (session_info_ptr && (flags & HOST)) {
    auto session_info =
        kernel_state_->memory()->TranslateVirtual<XSESSION_INFO*>(
            session_info_ptr);

    // Generate session ID with system link mask (top byte = 0x00)
    // On Xbox 360, system link sessions have XNKID_SYSTEM_LINK (0x00) as the
    // top byte. Games check this to identify session type.
    session_info->sessionID.ab[0] = 0x00;
    for (int i = 1; i < 8; i++) {
      session_info->sessionID.ab[i] = static_cast<uint8_t>(dist(rd) & 0xFF);
    }

    // Generate identity exchange key (each byte = its index)
    // Matches netplay's GenerateIdentityExchangeKey
    for (int i = 0; i < 16; i++) {
      session_info->keyExchangeKey.ab[i] = static_cast<uint8_t>(i);
    }

    // Fill host address completely (matching netplay's IpGetConsoleXnAddr)
    auto adapter = xam::QueryActiveAdapter();
    if (adapter.found) {
      std::memset(&session_info->hostAddress, 0, sizeof(XNADDR));
      session_info->hostAddress.ina = adapter.ip_addr;
      session_info->hostAddress.inaOnline = adapter.ip_addr;
      session_info->hostAddress.wPortOnline = 36000;
      std::memcpy(session_info->hostAddress.abEnet, adapter.mac_addr, 6);
      session_info->hostAddress.abOnline[16] = 1;  // platform_type = Xbox360
    }

    // Store the filled session info
    local_details_.sessionInfo = *session_info;

    XELOGI(
        "XSession::CreateSession: sessionID="
        "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        session_info->sessionID.ab[0], session_info->sessionID.ab[1],
        session_info->sessionID.ab[2], session_info->sessionID.ab[3],
        session_info->sessionID.ab[4], session_info->sessionID.ab[5],
        session_info->sessionID.ab[6], session_info->sessionID.ab[7]);
    XELOGI(
        "XSession::CreateSession: keyExchangeKey="
        "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}"
        "{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        session_info->keyExchangeKey.ab[0], session_info->keyExchangeKey.ab[1],
        session_info->keyExchangeKey.ab[2], session_info->keyExchangeKey.ab[3],
        session_info->keyExchangeKey.ab[4], session_info->keyExchangeKey.ab[5],
        session_info->keyExchangeKey.ab[6], session_info->keyExchangeKey.ab[7],
        session_info->keyExchangeKey.ab[8], session_info->keyExchangeKey.ab[9],
        session_info->keyExchangeKey.ab[10], session_info->keyExchangeKey.ab[11],
        session_info->keyExchangeKey.ab[12], session_info->keyExchangeKey.ab[13],
        session_info->keyExchangeKey.ab[14], session_info->keyExchangeKey.ab[15]);
    XELOGI(
        "XSession::CreateSession: hostAddress: ina={:08X}, inaOnline={:08X}, "
        "wPortOnline={}, MAC={:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}, "
        "abOnline[16]={} (platform_type)",
        (uint32_t)session_info->hostAddress.ina,
        (uint32_t)session_info->hostAddress.inaOnline,
        (uint16_t)session_info->hostAddress.wPortOnline,
        session_info->hostAddress.abEnet[0], session_info->hostAddress.abEnet[1],
        session_info->hostAddress.abEnet[2], session_info->hostAddress.abEnet[3],
        session_info->hostAddress.abEnet[4], session_info->hostAddress.abEnet[5],
        session_info->hostAddress.abOnline[16]);
  }

  XELOGI(
      "XSession::CreateSession: success, available_public={}, "
      "available_private={}",
      (uint32_t)local_details_.AvailablePublicSlots,
      (uint32_t)local_details_.AvailablePrivateSlots);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::DeleteSession() {
  XELOGI("XSession::DeleteSession");
  local_details_.eState = XSESSION_STATE::DELETED;
  members_.clear();
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::StartSession() {
  XELOGI("XSession::StartSession");
  if (local_details_.eState == XSESSION_STATE::LOBBY) {
    local_details_.eState = XSESSION_STATE::INGAME;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::EndSession() {
  XELOGI("XSession::EndSession");
  if (local_details_.eState == XSESSION_STATE::INGAME) {
    local_details_.eState = XSESSION_STATE::REPORTING;
  }
  return X_ERROR_SUCCESS;
}

X_RESULT XSession::JoinSession(XGI_SESSION_MANAGE* data) {
  uint32_t count = data->array_count;
  bool is_local = (data->xuid_array_ptr == 0);

  XELOGI("XSession::JoinSession: count={}, is_local={}", count, is_local);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t xuid = 0;
    uint32_t user_index = 0;
    bool is_private = false;

    if (is_local) {
      // Local join - use indices array
      if (data->indices_array_ptr) {
        auto indices =
            kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
                data->indices_array_ptr);
        user_index = indices[i];

        // Get XUID from user profile
        auto profile =
            kernel_state_->xam_state()->GetUserProfile(user_index);
        if (profile) {
          xuid = profile->GetLogonXUID();
        }
      }
    } else {
      // Remote join - use xuids array
      if (data->xuid_array_ptr) {
        auto xuids =
            kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
                data->xuid_array_ptr);
        xuid = xuids[i];
      }
    }

    // Check private slots array
    if (data->private_slots_array_ptr) {
      auto private_slots =
          kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
              data->private_slots_array_ptr);
      is_private = (private_slots[i] != 0);
    }

    // Check if already a member
    if (members_.find(xuid) != members_.end()) {
      XELOGI("XSession::JoinSession: XUID {:016X} already a member", xuid);
      continue;
    }

    // Check slot availability
    if (is_private) {
      if (local_details_.AvailablePrivateSlots == 0) {
        XELOGI("XSession::JoinSession: no private slots available");
        return X_ERROR_FUNCTION_FAILED;
      }
      local_details_.AvailablePrivateSlots =
          local_details_.AvailablePrivateSlots - 1;
    } else {
      if (local_details_.AvailablePublicSlots == 0) {
        XELOGI("XSession::JoinSession: no public slots available");
        return X_ERROR_FUNCTION_FAILED;
      }
      local_details_.AvailablePublicSlots =
          local_details_.AvailablePublicSlots - 1;
    }

    // Add member
    XSESSION_MEMBER member = {};
    member.OnlineXUID = xuid;
    member.UserIndex = user_index;
    member.Flags = is_private ? static_cast<uint32_t>(MEMBER_FLAGS::PRIVATE_SLOT)
                              : 0;
    members_[xuid] = member;

    local_details_.ActualMemberCount = local_details_.ActualMemberCount + 1;

    XELOGI(
        "XSession::JoinSession: added XUID {:016X}, user_index={}, "
        "is_private={}",
        xuid, user_index, is_private);
  }

  XELOGI(
      "XSession::JoinSession: success, members={}, available_public={}, "
      "available_private={}",
      (uint32_t)local_details_.ActualMemberCount,
      (uint32_t)local_details_.AvailablePublicSlots,
      (uint32_t)local_details_.AvailablePrivateSlots);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::LeaveSession(XGI_SESSION_MANAGE* data) {
  uint32_t count = data->array_count;
  bool is_local = (data->xuid_array_ptr == 0);

  XELOGI("XSession::LeaveSession: count={}, is_local={}", count, is_local);

  for (uint32_t i = 0; i < count; i++) {
    uint64_t xuid = 0;

    if (is_local) {
      // Local leave - use indices array
      if (data->indices_array_ptr) {
        auto indices =
            kernel_state_->memory()->TranslateVirtual<xe::be<uint32_t>*>(
                data->indices_array_ptr);
        uint32_t user_index = indices[i];

        // Get XUID from user profile
        auto profile =
            kernel_state_->xam_state()->GetUserProfile(user_index);
        if (profile) {
          xuid = profile->GetLogonXUID();
        }
      }
    } else {
      // Remote leave - use xuids array
      if (data->xuid_array_ptr) {
        auto xuids =
            kernel_state_->memory()->TranslateVirtual<xe::be<uint64_t>*>(
                data->xuid_array_ptr);
        xuid = xuids[i];
      }
    }

    auto it = members_.find(xuid);
    if (it == members_.end()) {
      XELOGI("XSession::LeaveSession: XUID {:016X} not a member", xuid);
      continue;
    }

    // Return the slot
    bool was_private = it->second.IsPrivate();
    if (was_private) {
      local_details_.AvailablePrivateSlots =
          local_details_.AvailablePrivateSlots + 1;
    } else {
      local_details_.AvailablePublicSlots =
          local_details_.AvailablePublicSlots + 1;
    }

    members_.erase(it);
    local_details_.ActualMemberCount = local_details_.ActualMemberCount - 1;

    XELOGI("XSession::LeaveSession: removed XUID {:016X}", xuid);
  }

  XELOGI(
      "XSession::LeaveSession: success, members={}, available_public={}, "
      "available_private={}",
      (uint32_t)local_details_.ActualMemberCount,
      (uint32_t)local_details_.AvailablePublicSlots,
      (uint32_t)local_details_.AvailablePrivateSlots);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::GetSessionDetails(XGI_SESSION_DETAILS* data) {
  XELOGI("XSession::GetSessionDetails: details_ptr={:08X}, buffer_size={:08X}",
         (uint32_t)data->session_details_ptr,
         (uint32_t)data->details_buffer_size);

  if (!data->session_details_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto local_details_ptr =
      kernel_state_->memory()->TranslateVirtual<XSESSION_LOCAL_DETAILS*>(
          data->session_details_ptr);

  // Copy local details to guest buffer
  std::memcpy(local_details_ptr, &local_details_,
              sizeof(XSESSION_LOCAL_DETAILS));

  // Calculate how many members can fit in the buffer
  uint32_t buffer_size = data->details_buffer_size;
  uint32_t members_space = 0;
  if (buffer_size > sizeof(XSESSION_LOCAL_DETAILS)) {
    members_space = buffer_size - sizeof(XSESSION_LOCAL_DETAILS);
  }
  uint32_t max_members = members_space / sizeof(XSESSION_MEMBER);

  // Fill member array if space available
  if (max_members > 0 && !members_.empty()) {
    auto members_ptr =
        reinterpret_cast<XSESSION_MEMBER*>(local_details_ptr + 1);

    local_details_ptr->SessionMembers_ptr =
        kernel_state_->memory()->HostToGuestVirtual(members_ptr);

    uint32_t count = 0;
    for (const auto& [xuid, member] : members_) {
      if (count >= max_members) break;
      members_ptr[count] = member;
      count++;
    }

    local_details_ptr->ReturnedMemberCount = count;
  } else {
    local_details_ptr->SessionMembers_ptr = 0;
    local_details_ptr->ReturnedMemberCount = 0;
  }

  XELOGI(
      "XSession::GetSessionDetails: success, state={}, max_public={}, "
      "max_private={}, avail_public={}, avail_private={}, members={}",
      static_cast<uint32_t>(local_details_ptr->eState),
      (uint32_t)local_details_ptr->MaxPublicSlots,
      (uint32_t)local_details_ptr->MaxPrivateSlots,
      (uint32_t)local_details_ptr->AvailablePublicSlots,
      (uint32_t)local_details_ptr->AvailablePrivateSlots,
      (uint32_t)local_details_ptr->ActualMemberCount);

  return X_ERROR_SUCCESS;
}

X_RESULT XSession::ModifySession(XGI_SESSION_MODIFY* data) {
  XELOGI(
      "XSession::ModifySession: flags={:08X}, max_public={}, max_private={}",
      (uint32_t)data->flags, (uint32_t)data->maxPublicSlots,
      (uint32_t)data->maxPrivateSlots);

  local_details_.Flags = data->flags;

  // Update max slots if they're increasing or if current available allows
  uint32_t new_max_public = data->maxPublicSlots;
  uint32_t new_max_private = data->maxPrivateSlots;

  uint32_t used_public =
      local_details_.MaxPublicSlots - local_details_.AvailablePublicSlots;
  uint32_t used_private =
      local_details_.MaxPrivateSlots - local_details_.AvailablePrivateSlots;

  if (new_max_public >= used_public) {
    local_details_.MaxPublicSlots = new_max_public;
    local_details_.AvailablePublicSlots = new_max_public - used_public;
  }

  if (new_max_private >= used_private) {
    local_details_.MaxPrivateSlots = new_max_private;
    local_details_.AvailablePrivateSlots = new_max_private - used_private;
  }

  return X_ERROR_SUCCESS;
}

}  // namespace kernel
}  // namespace xe

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSESSION_H_
#define XENIA_KERNEL_XSESSION_H_

#include <map>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xobject.h"

namespace xe {
namespace kernel {

enum SessionFlags {
  HOST = 0x01,
  PRESENCE = 0x02,
  STATS = 0x04,
  MATCHMAKING = 0x08,
  ARBITRATION = 0x10,
  PEER_NETWORK = 0x20,
  SOCIAL_MATCHMAKING_ALLOWED = 0x80,
  INVITES_DISABLED = 0x0100,
  JOIN_VIA_PRESENCE_DISABLED = 0x0200,
  JOIN_IN_PROGRESS_DISABLED = 0x0400,
  JOIN_VIA_PRESENCE_FRIENDS_ONLY = 0x0800,
  UNKNOWN = 0x1000,  // 4156091D and 5841128F sets this flag?

  SINGLEPLAYER_WITH_STATS = PRESENCE | STATS | INVITES_DISABLED |
                            JOIN_VIA_PRESENCE_DISABLED |
                            JOIN_IN_PROGRESS_DISABLED,

  LIVE_MULTIPLAYER_STANDARD = PRESENCE | STATS | MATCHMAKING | PEER_NETWORK,
  LIVE_MULTIPLAYER_RANKED = LIVE_MULTIPLAYER_STANDARD | ARBITRATION,
  SYSTEMLINK = PEER_NETWORK,
  GROUP_LOBBY = PRESENCE | PEER_NETWORK,
  GROUP_GAME = STATS | MATCHMAKING | PEER_NETWORK,

  // HELPERS
  SYSTEMLINK_FEATURES = HOST | SYSTEMLINK,
  LIVE_FEATURES = PRESENCE | STATS | MATCHMAKING | ARBITRATION
};

inline bool IsOfflineSession(const SessionFlags flags) { return !flags; }

inline bool IsXboxLiveSession(const SessionFlags flags) {
  return !IsOfflineSession(flags) && flags & SessionFlags::LIVE_FEATURES;
}

// Session state enum
enum class XSESSION_STATE : uint32_t {
  LOBBY,
  REGISTRATION,
  INGAME,
  REPORTING,
  DELETED
};

// Session ID structure
struct XNKID {
  uint8_t ab[8];
  uint64_t as_uint64() const { return *reinterpret_cast<const uint64_t*>(&ab); }
};
static_assert_size(XNKID, 0x8);

// Session key structure
struct XNKEY {
  uint8_t ab[16];
};
static_assert_size(XNKEY, 0x10);

// Network address structure
struct XNADDR {
  xe::be<uint32_t> ina;           // IP address
  xe::be<uint32_t> inaOnline;     // Online IP (not used for system link)
  xe::be<uint16_t> wPortOnline;   // Online port (not used for system link)
  uint8_t abEnet[6];              // MAC address
  uint8_t abOnline[20];           // Online ID (not used for system link)
};
static_assert_size(XNADDR, 0x24);

// Session info structure
struct XSESSION_INFO {
  XNKID sessionID;
  XNADDR hostAddress;
  XNKEY keyExchangeKey;
};
static_assert_size(XSESSION_INFO, 0x3C);

// Member flags
enum class MEMBER_FLAGS : uint32_t { PRIVATE_SLOT = 0x01, ZOMBIE = 0x02 };

// Session member structure
struct XSESSION_MEMBER {
  xe::be<uint64_t> OnlineXUID;
  xe::be<uint32_t> UserIndex;
  xe::be<uint32_t> Flags;

  void SetPrivate() {
    Flags = Flags | static_cast<uint32_t>(MEMBER_FLAGS::PRIVATE_SLOT);
  }

  bool IsPrivate() const {
    return (Flags & static_cast<uint32_t>(MEMBER_FLAGS::PRIVATE_SLOT)) != 0;
  }
};
static_assert_size(XSESSION_MEMBER, 0x10);

// Native kernel session object (minimal - just holds handle reference)
struct X_KSESSION {
  xe::be<uint32_t> handle;
};
static_assert_size(X_KSESSION, 0x4);

// Session local details structure - returned by XSessionGetDetails
struct XSESSION_LOCAL_DETAILS {
  xe::be<uint32_t> UserIndexHost;
  xe::be<uint32_t> GameType;
  xe::be<uint32_t> GameMode;
  xe::be<uint32_t> Flags;
  xe::be<uint32_t> MaxPublicSlots;
  xe::be<uint32_t> MaxPrivateSlots;
  xe::be<uint32_t> AvailablePublicSlots;
  xe::be<uint32_t> AvailablePrivateSlots;
  xe::be<uint32_t> ActualMemberCount;
  xe::be<uint32_t> ReturnedMemberCount;
  XSESSION_STATE eState;
  xe::be<uint64_t> Nonce;
  XSESSION_INFO sessionInfo;
  XNKID xnkidArbitration;
  xe::be<uint32_t> SessionMembers_ptr;
};
static_assert_size(XSESSION_LOCAL_DETAILS, 0x80);

// XGI structures for session management
struct XGI_SESSION_CREATE {
  xe::be<uint32_t> obj_ptr;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> num_slots_public;
  xe::be<uint32_t> num_slots_private;
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> session_info_ptr;
  xe::be<uint32_t> nonce_ptr;
};
static_assert_size(XGI_SESSION_CREATE, 0x1C);

struct XGI_SESSION_DETAILS {
  xe::be<uint32_t> obj_ptr;
  xe::be<uint32_t> details_buffer_size;
  xe::be<uint32_t> session_details_ptr;
  xe::be<uint32_t> reserved1;
  xe::be<uint32_t> reserved2;
  xe::be<uint32_t> reserved3;
};
static_assert_size(XGI_SESSION_DETAILS, 0x18);

struct XGI_SESSION_MANAGE {
  xe::be<uint32_t> obj_ptr;
  xe::be<uint32_t> array_count;
  xe::be<uint32_t> xuid_array_ptr;
  xe::be<uint32_t> indices_array_ptr;
  xe::be<uint32_t> private_slots_array_ptr;
};
static_assert_size(XGI_SESSION_MANAGE, 0x14);

struct XGI_SESSION_MODIFY {
  xe::be<uint32_t> obj_ptr;
  xe::be<uint32_t> flags;
  xe::be<uint32_t> maxPublicSlots;
  xe::be<uint32_t> maxPrivateSlots;
};
static_assert_size(XGI_SESSION_MODIFY, 0x10);

struct XGI_SESSION_STATE {
  xe::be<uint32_t> obj_ptr;
  xe::be<uint32_t> flags;
  xe::be<uint64_t> session_nonce;
};
static_assert_size(XGI_SESSION_STATE, 0x10);

// Session search request structure
struct XGI_SESSION_SEARCH {
  xe::be<uint32_t> proc_index;
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> num_results;
  xe::be<uint16_t> num_props;
  xe::be<uint16_t> num_ctx;
  xe::be<uint32_t> props_ptr;
  xe::be<uint32_t> ctx_ptr;
  xe::be<uint32_t> results_buffer_size;
  xe::be<uint32_t> search_results_ptr;
};
static_assert_size(XGI_SESSION_SEARCH, 0x20);

// Individual search result (single session)
struct XSESSION_SEARCHRESULT {
  XSESSION_INFO info;
  xe::be<uint32_t> OpenPublicSlots;
  xe::be<uint32_t> OpenPrivateSlots;
  xe::be<uint32_t> FilledPublicSlots;
  xe::be<uint32_t> FilledPrivateSlots;
  xe::be<uint32_t> PropertyCount;
  xe::be<uint32_t> ContextCount;
  xe::be<uint32_t> Properties_ptr;  // Array of XUSER_PROPERTY
  xe::be<uint32_t> Contexts_ptr;    // Array of XUSER_CONTEXT
};
static_assert_size(XSESSION_SEARCHRESULT, 0x5C);

// Search results header
struct XSESSION_SEARCHRESULT_HEADER {
  xe::be<uint32_t> SearchResultsCount;
  xe::be<uint32_t> results_ptr;  // Points to first XSESSION_SEARCHRESULT
};
static_assert_size(XSESSION_SEARCHRESULT_HEADER, 0x8);

// XSession class - manages session state for system link
class XSession : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Session;

  XSession(KernelState* kernel_state);
  ~XSession() override;

  X_STATUS Initialize();

  // Session lifecycle
  X_RESULT CreateSession(uint32_t user_index, uint32_t public_slots,
                         uint32_t private_slots, uint32_t flags,
                         uint32_t session_info_ptr, uint32_t nonce_ptr);
  X_RESULT DeleteSession();
  X_RESULT StartSession();
  X_RESULT EndSession();

  // Member management
  X_RESULT JoinSession(XGI_SESSION_MANAGE* data);
  X_RESULT LeaveSession(XGI_SESSION_MANAGE* data);

  // Session queries
  X_RESULT GetSessionDetails(XGI_SESSION_DETAILS* data);
  X_RESULT ModifySession(XGI_SESSION_MODIFY* data);

  // Accessors
  bool IsHost() const { return (local_details_.Flags & HOST) != 0; }
  bool IsSystemLink() const {
    return (local_details_.Flags & PEER_NETWORK) != 0 &&
           (local_details_.Flags & ~(HOST | PEER_NETWORK)) == 0;
  }
  uint32_t GetAvailablePublicSlots() const {
    return local_details_.AvailablePublicSlots;
  }
  uint32_t GetAvailablePrivateSlots() const {
    return local_details_.AvailablePrivateSlots;
  }

 private:
  XSESSION_LOCAL_DETAILS local_details_{};
  std::map<uint64_t, XSESSION_MEMBER> members_{};
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSESSION_H_

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_AUDIO_MEDIA_PLAYER_QT_H_
#define XENIA_APU_AUDIO_MEDIA_PLAYER_QT_H_

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "xenia/apu/audio_system.h"
#include "xenia/base/mutex.h"
#include "xenia/kernel/xam/apps/xmp_app.h"
#include "xenia/xbox.h"

namespace xe {
namespace apu {

using XmpApp = kernel::xam::apps::XmpApp;

// Forward declaration of implementation class that contains Qt objects
class AudioMediaPlayerQtImpl;

class AudioMediaPlayerQt {
  // Grant impl access to private members
  friend class AudioMediaPlayerQtImpl;

 public:
  AudioMediaPlayerQt(apu::AudioSystem* audio_system,
                     kernel::KernelState* kernel_state);
  ~AudioMediaPlayerQt();

  void Setup();

  X_STATUS Play(uint32_t playlist_handle, uint32_t song_handle, bool force);
  X_STATUS Next();
  X_STATUS Previous();

  void Stop(bool change_state = false, bool force = true);
  void Pause();
  void Continue();

  XmpApp::State GetState() const { return state_; }

  bool IsIdle() const { return state_ == XmpApp::State::kIdle; }
  bool IsPlaying() const { return state_ == XmpApp::State::kPlaying; }
  bool IsPaused() const { return state_ == XmpApp::State::kPaused; }
  bool IsSongLoaded() const { return active_song_; }

  bool IsInRepeatMode() const {
    return repeat_mode_ == XmpApp::RepeatMode::kPlaylist;
  }

  bool IsLastSongInPlaylist() const;

  X_STATUS SetVolume(float volume);
  const std::atomic<float>* GetVolume() const { return &volume_; }

  void SetPlaybackMode(XmpApp::PlaybackMode playback_mode) {
    playback_mode_ = playback_mode;
  }
  XmpApp::PlaybackMode GetPlaybackMode() const { return playback_mode_; }

  void SetRepeatMode(XmpApp::RepeatMode repeat_mode) {
    repeat_mode_ = repeat_mode;
  }
  XmpApp::RepeatMode GetRepeatMode() { return repeat_mode_; }

  void SetPlaybackFlags(XmpApp::PlaybackFlags playback_flags) {
    playback_flags_ = playback_flags;
  }
  XmpApp::PlaybackFlags GetPlaybackFlags() const { return playback_flags_; }

  void SetPlaybackClient(XmpApp::PlaybackClient playback_client) {
    if (playback_client == XmpApp::PlaybackClient::kSystem) {
      return;
    }

    playback_client_ = playback_client;
  }

  XmpApp::PlaybackClient GetPlaybackClient() const { return playback_client_; }
  uint32_t GetDashInItState() const { return dash_init_state; }

  bool IsTitleInPlaybackControl() const {
    return playback_client_ == XmpApp::PlaybackClient::kTitle ||
           is_title_rendering_enabled_;
  }

  void SetCaptureCallback(uint32_t callback, uint32_t context,
                          bool title_render);

  void AddPlaylist(uint32_t handle, std::unique_ptr<XmpApp::Playlist> playlist);
  void RemovePlaylist(uint32_t handle);

  XmpApp::Song* GetCurrentSong() const { return active_song_; }

 private:
  void OnStateChanged();
  void PlayCurrentSong();
  bool LoadSongToMemory(std::vector<uint8_t>* buffer);

  XmpApp::State state_ = XmpApp::State::kIdle;
  XmpApp::PlaybackClient playback_client_ = XmpApp::PlaybackClient::kSystem;
  XmpApp::PlaybackMode playback_mode_ = XmpApp::PlaybackMode::kInOrder;
  XmpApp::RepeatMode repeat_mode_ = XmpApp::RepeatMode::kPlaylist;
  XmpApp::PlaybackFlags playback_flags_ = XmpApp::PlaybackFlags::kDefault;
  std::atomic<float> volume_ = 0.0f;
  uint32_t dash_init_state = 0;

  std::unordered_map<uint32_t, std::unique_ptr<XmpApp::Playlist>> playlists_;
  XmpApp::Playlist* active_playlist_;
  XmpApp::Song* active_song_;

  size_t current_song_handle_ = 0;

  uint32_t callback_ = 0;
  uint32_t callback_context_ = 0;
  bool is_title_rendering_enabled_ = false;

  AudioSystem* audio_system_ = nullptr;
  kernel::KernelState* kernel_state_ = nullptr;

  // Pimpl to hide Qt implementation details
  std::unique_ptr<AudioMediaPlayerQtImpl> impl_;

  xe::global_critical_region global_critical_region_;
};

}  // namespace apu
}  // namespace xe

#endif

/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2024 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/audio_media_player_qt.h"

#include <algorithm>
#include <random>

#include <QApplication>
#include <QAudioOutput>
#include <QBuffer>
#include <QMediaPlayer>
#include <QObject>

#include "xenia/apu/audio_system.h"
#include "xenia/base/logging.h"

DEFINE_bool(enable_xmp, true, "Enables Music Player playback.", "APU");
DEFINE_int32(xmp_default_volume, 70,
             "Default music volume if game doesn't set it [0-100].", "APU");

namespace xe {
namespace apu {

// Implementation class that contains Qt objects and inherits from QObject
class AudioMediaPlayerQtImpl : public QObject {
  Q_OBJECT

 public:
  AudioMediaPlayerQtImpl(AudioMediaPlayerQt* parent)
      : QObject(nullptr), parent_(parent) {
    media_player_ = new QMediaPlayer(this);
    audio_output_ = new QAudioOutput(this);
    media_player_->setAudioOutput(audio_output_);

    connect(media_player_, &QMediaPlayer::mediaStatusChanged, this,
            &AudioMediaPlayerQtImpl::OnMediaStatusChanged);
    connect(media_player_, &QMediaPlayer::playbackStateChanged, this,
            &AudioMediaPlayerQtImpl::OnPlaybackStateChanged);
    connect(media_player_, &QMediaPlayer::errorOccurred, this,
            &AudioMediaPlayerQtImpl::OnErrorOccurred);
  }

  ~AudioMediaPlayerQtImpl() {
    if (song_buffer_) {
      delete song_buffer_;
      song_buffer_ = nullptr;
    }
  }

  QMediaPlayer* media_player_ = nullptr;
  QAudioOutput* audio_output_ = nullptr;
  QBuffer* song_buffer_ = nullptr;
  std::vector<uint8_t> song_data_;
  AudioMediaPlayerQt* parent_ = nullptr;

 public slots:
  void PlaySongData(const std::vector<uint8_t>& data);
  void StopPlayback();
  void PausePlayback();
  void ResumePlayback();
  void SetVolume(float volume);

 private slots:
  void OnMediaStatusChanged(QMediaPlayer::MediaStatus status);
  void OnPlaybackStateChanged(QMediaPlayer::PlaybackState state);
  void OnErrorOccurred(QMediaPlayer::Error error, const QString& errorString);
};

AudioMediaPlayerQt::AudioMediaPlayerQt(apu::AudioSystem* audio_system,
                                       kernel::KernelState* kernel_state)
    : audio_system_(audio_system),
      kernel_state_(kernel_state),
      active_playlist_(nullptr),
      active_song_(nullptr) {
  // Create impl on Qt main thread
  QMetaObject::invokeMethod(
      qApp,
      [this]() {
        impl_ = std::make_unique<AudioMediaPlayerQtImpl>(this);
        // Move to main thread
        impl_->moveToThread(qApp->thread());
      },
      Qt::BlockingQueuedConnection);
}

AudioMediaPlayerQt::~AudioMediaPlayerQt() { Stop(); }

void AudioMediaPlayerQt::Setup() {
  if (!cvars::enable_xmp) {
    return;
  }

  // Set default volume
  if (volume_ == 0.0f) {
    volume_ = cvars::xmp_default_volume / 100.0f;
  }
  impl_->audio_output_->setVolume(volume_);
}

X_STATUS AudioMediaPlayerQt::Play(uint32_t playlist_handle,
                                  uint32_t song_handle, bool force) {
  auto playlist_itr = playlists_.find(playlist_handle);
  if (playlist_itr == playlists_.cend()) {
    XELOGE("XMP: Playlist {:08X} not found", playlist_handle);
    return X_STATUS_UNSUCCESSFUL;
  }

  active_playlist_ = playlist_itr->second.get();
  XELOGI("XMP: Playing playlist {:08X} '{}' with {} songs", playlist_handle,
         xe::to_utf8(active_playlist_->name), active_playlist_->songs.size());

  if (!IsIdle() && force) {
    Stop(false, force);
  }

  if (!song_handle) {
    active_song_ = active_playlist_->songs.cbegin()->get();
    XELOGI("XMP: Auto-selecting first song: '{}'",
           xe::to_utf8(active_song_->name));
    PlayCurrentSong();
    return X_STATUS_SUCCESS;
  }

  auto song_itr = std::find_if(
      active_playlist_->songs.cbegin(), active_playlist_->songs.cend(),
      [song_handle](const std::unique_ptr<XmpApp::Song>& song) {
        return song->handle == song_handle;
      });

  if (song_itr == active_playlist_->songs.cend()) {
    XELOGE("XMP: Song handle {:08X} not found in playlist", song_handle);
    return X_STATUS_UNSUCCESSFUL;
  }

  active_song_ = song_itr->get();
  XELOGI("XMP: Selected song: '{}'", xe::to_utf8(active_song_->name));
  PlayCurrentSong();
  return X_STATUS_SUCCESS;
}

void AudioMediaPlayerQt::PlayCurrentSong() {
  if (!active_song_) {
    return;
  }

  XELOGI("XMP: Loading song '{}' by '{}' from '{}'",
         xe::to_utf8(active_song_->name), xe::to_utf8(active_song_->artist),
         xe::to_utf8(active_song_->file_path));

  // Load song to memory on this thread
  std::vector<uint8_t> song_data;
  if (!LoadSongToMemory(&song_data)) {
    XELOGE("XMP: Failed to load song to memory");
    return;
  }

  XELOGI("XMP: Loaded {} bytes, starting playback", song_data.size());

  // Invoke playback on Qt thread
  QMetaObject::invokeMethod(
      impl_.get(),
      [this, song_data = std::move(song_data)]() {
        impl_->PlaySongData(song_data);
      },
      Qt::QueuedConnection);

  state_ = XmpApp::State::kPlaying;
  current_song_handle_ = active_song_->handle;
  OnStateChanged();
}

void AudioMediaPlayerQt::Pause() {
  if (!IsPlaying()) {
    return;
  }

  XELOGI("XMP: Pausing playback");
  QMetaObject::invokeMethod(impl_.get(), &AudioMediaPlayerQtImpl::PausePlayback,
                            Qt::QueuedConnection);
  state_ = XmpApp::State::kPaused;
  OnStateChanged();
}

void AudioMediaPlayerQt::Stop(bool change_state, bool force) {
  if (IsIdle()) {
    return;
  }

  XELOGI("XMP: Stopping playback");
  QMetaObject::invokeMethod(impl_.get(), &AudioMediaPlayerQtImpl::StopPlayback,
                            Qt::QueuedConnection);
  state_ = XmpApp::State::kIdle;
  active_song_ = nullptr;

  if (change_state) {
    OnStateChanged();
  }
}

void AudioMediaPlayerQt::Continue() {
  if (!IsPaused()) {
    return;
  }

  XELOGI("XMP: Resuming playback");
  QMetaObject::invokeMethod(impl_.get(),
                            &AudioMediaPlayerQtImpl::ResumePlayback,
                            Qt::QueuedConnection);
  state_ = XmpApp::State::kPlaying;
  OnStateChanged();
}

X_STATUS AudioMediaPlayerQt::Next() {
  if (!active_playlist_) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("XMP: Skipping to next song");

  if (active_song_) {
    Stop(false, false);
  }

  auto itr = std::find_if(active_playlist_->songs.cbegin(),
                          active_playlist_->songs.cend(),
                          [this](const std::unique_ptr<XmpApp::Song>& song) {
                            return song->handle == current_song_handle_;
                          });

  if (itr == active_playlist_->songs.cend()) {
    return X_STATUS_UNSUCCESSFUL;
  }

  itr = std::next(itr);

  if (itr != active_playlist_->songs.cend()) {
    active_song_ = itr->get();
  } else {
    active_song_ = active_playlist_->songs.cbegin()->get();
    XELOGI("XMP: Wrapped to start of playlist");
  }

  PlayCurrentSong();
  return X_STATUS_SUCCESS;
}

X_STATUS AudioMediaPlayerQt::Previous() {
  if (!active_playlist_) {
    return X_STATUS_UNSUCCESSFUL;
  }

  XELOGI("XMP: Going to previous song");

  if (active_song_) {
    Stop(false, false);
  }

  auto itr = std::find_if(active_playlist_->songs.cbegin(),
                          active_playlist_->songs.cend(),
                          [this](const std::unique_ptr<XmpApp::Song>& song) {
                            return song->handle == current_song_handle_;
                          });

  if (itr == active_playlist_->songs.cbegin()) {
    active_song_ = active_playlist_->songs.crbegin()->get();
    XELOGI("XMP: Wrapped to end of playlist");
  } else {
    itr = std::prev(itr);
    active_song_ = itr->get();
  }

  PlayCurrentSong();
  return X_STATUS_SUCCESS;
}

bool AudioMediaPlayerQt::LoadSongToMemory(std::vector<uint8_t>* buffer) {
  if (!active_song_) {
    return false;
  }

  vfs::File* vfs_file;
  vfs::FileAction file_action;
  X_STATUS result = kernel_state_->file_system()->OpenFile(
      nullptr, xe::to_utf8(active_song_->file_path),
      vfs::FileDisposition::kOpen, vfs::FileAccess::kGenericRead, false, true,
      &vfs_file, &file_action);

  if (result) {
    return false;
  }

  buffer->resize(vfs_file->entry()->size());
  size_t bytes_read = 0;
  result = vfs_file->ReadSync(
      std::span<uint8_t>(buffer->data(), vfs_file->entry()->size()), 0,
      &bytes_read);

  return !result;
}

void AudioMediaPlayerQt::AddPlaylist(
    uint32_t handle, std::unique_ptr<XmpApp::Playlist> playlist) {
  if (playlists_.count(handle) != 0) {
    XELOGW("XMP: Playlist {:08X} already exists", handle);
    return;
  }

  XELOGI("XMP: Adding playlist {:08X} '{}' with {} songs", handle,
         xe::to_utf8(playlist->name), playlist->songs.size());

  if (playback_mode_ == XmpApp::PlaybackMode::kShuffle) {
    XELOGI("XMP: Shuffling playlist");
    auto rng = std::default_random_engine{};
    std::shuffle(playlist->songs.begin(), playlist->songs.end(), rng);
  }

  playlists_.insert({handle, std::move(playlist)});
}

void AudioMediaPlayerQt::RemovePlaylist(uint32_t handle) {
  if (playlists_.count(handle) == 0) {
    XELOGW("XMP: Playlist {:08X} not found for removal", handle);
    return;
  }

  XELOGI("XMP: Removing playlist {:08X}", handle);

  if (active_playlist_ && active_song_) {
    Stop();
  }

  playlists_.erase(handle);
}

X_STATUS AudioMediaPlayerQt::SetVolume(float volume) {
  volume_.store(std::min(volume, 1.0f));

  XELOGI("XMP: Setting volume to {:.0f}%", volume_ * 100.0f);

  if (impl_) {
    QMetaObject::invokeMethod(
        impl_.get(), [this]() { impl_->SetVolume(volume_); },
        Qt::QueuedConnection);
  }

  return X_STATUS_SUCCESS;
}

bool AudioMediaPlayerQt::IsLastSongInPlaylist() const {
  if (!active_playlist_) {
    return false;
  }

  auto itr = std::find_if(active_playlist_->songs.cbegin(),
                          active_playlist_->songs.cend(),
                          [this](const std::unique_ptr<XmpApp::Song>& song) {
                            return song->handle == current_song_handle_;
                          });
  itr = std::next(itr);
  return itr == active_playlist_->songs.cend();
}

void AudioMediaPlayerQt::SetCaptureCallback(uint32_t callback, uint32_t context,
                                            bool title_render) {
  callback_ = 0;
  callback_context_ = context;
  is_title_rendering_enabled_ = false;
}

void AudioMediaPlayerQt::OnStateChanged() {
  kernel_state_->BroadcastNotification(kXNotificationXmpStateChanged,
                                       static_cast<uint32_t>(state_));
}

// AudioMediaPlayerQtImpl slot implementations
void AudioMediaPlayerQtImpl::PlaySongData(const std::vector<uint8_t>& data) {
  XELOGI("XMP: Qt thread received {} bytes to play", data.size());

  // Stop current playback
  media_player_->stop();

  // Clean up previous buffer
  if (song_buffer_) {
    song_buffer_->deleteLater();
    song_buffer_ = nullptr;
  }

  // Store song data
  song_data_ = data;

  // Create QBuffer from song data
  song_buffer_ = new QBuffer(this);
  song_buffer_->setData(reinterpret_cast<const char*>(song_data_.data()),
                        static_cast<qsizetype>(song_data_.size()));

  if (!song_buffer_->open(QIODevice::ReadOnly)) {
    XELOGE("XMP: Failed to open song buffer");
    song_buffer_->deleteLater();
    song_buffer_ = nullptr;
    return;
  }

  XELOGI("XMP: Buffer created and opened, starting QMediaPlayer");
  // Set source and play
  media_player_->setSourceDevice(song_buffer_, QUrl());
  media_player_->play();
}

void AudioMediaPlayerQtImpl::StopPlayback() { media_player_->stop(); }

void AudioMediaPlayerQtImpl::PausePlayback() { media_player_->pause(); }

void AudioMediaPlayerQtImpl::ResumePlayback() { media_player_->play(); }

void AudioMediaPlayerQtImpl::SetVolume(float volume) {
  if (audio_output_) {
    audio_output_->setVolume(volume);
  }
}

void AudioMediaPlayerQtImpl::OnMediaStatusChanged(
    QMediaPlayer::MediaStatus status) {
  switch (status) {
    case QMediaPlayer::EndOfMedia:
      // Song finished, play next
      if (!parent_->IsLastSongInPlaylist()) {
        parent_->Next();
      } else if (parent_->IsInRepeatMode() && parent_->active_playlist_) {
        // Restart playlist
        parent_->Play(parent_->active_playlist_->handle, 0, true);
      } else {
        parent_->Stop(true, true);
      }
      break;

    case QMediaPlayer::InvalidMedia:
      XELOGE("Invalid media format");
      parent_->Stop(true, true);
      break;

    case QMediaPlayer::LoadedMedia:
      XELOGD("Media loaded successfully");
      break;

    default:
      break;
  }
}

void AudioMediaPlayerQtImpl::OnPlaybackStateChanged(
    QMediaPlayer::PlaybackState qt_state) {
  // Qt's state changes are handled by our own state management
  // This is just for logging/debugging
  switch (qt_state) {
    case QMediaPlayer::PlayingState:
      XELOGD("Qt MediaPlayer: Playing");
      break;
    case QMediaPlayer::PausedState:
      XELOGD("Qt MediaPlayer: Paused");
      break;
    case QMediaPlayer::StoppedState:
      XELOGD("Qt MediaPlayer: Stopped");
      break;
  }
}

void AudioMediaPlayerQtImpl::OnErrorOccurred(QMediaPlayer::Error error,
                                             const QString& errorString) {
  XELOGE("Media player error: {} - {}", static_cast<int>(error),
         errorString.toStdString());
  parent_->Stop(true, true);
}

}  // namespace apu
}  // namespace xe

// MOC must be included at the end when Q_OBJECT is in a .cc file
#include "audio_media_player_qt.moc"

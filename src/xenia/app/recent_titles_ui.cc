/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2025 Xenia Canary. All rights reserved.                          *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/recent_titles_ui.h"
#include <chrono>
#include <thread>
#include "third_party/fmt/include/fmt/format.h"
#include "xenia/app/emulator_window.h"
#include "xenia/base/logging.h"
#include "xenia/base/string_util.h"
#include "xenia/base/system.h"
#include "xenia/base/utf8.h"
#include "xenia/emulator.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/user_tracker.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/ui/imgui_guest_notification.h"

namespace xe {
namespace app {

RecentTitlesUI::RecentTitlesUI(ui::ImGuiDrawer* imgui_drawer,
                               EmulatorWindow* emulator_window)
    : ui::ImGuiDialog(imgui_drawer), emulator_window_(emulator_window) {
  LoadRecentTitles();
}

RecentTitlesUI::~RecentTitlesUI() {
  for (auto& entry : title_icons_) {
    entry.second.release();
  }
}

void RecentTitlesUI::LoadRecentTitles() {
  recent_titles_.clear();

  // Get the recent titles directly from EmulatorWindow
  if (!emulator_window_) {
    return;
  }

  const auto& emulator_recent_titles =
      emulator_window_->GetRecentlyLaunchedTitles();

  for (const auto& entry : emulator_recent_titles) {
    // We'll get the title ID from the played games data when we match by name
    recent_titles_.push_back(
        {entry.title_name, entry.path_to_file, entry.last_run_time, 0, {}});
  }

  // Load icons if we have access to the emulator and user tracker
  if (emulator_window_ && emulator_window_->emulator() &&
      emulator_window_->emulator()->kernel_state() &&
      emulator_window_->emulator()->kernel_state()->xam_state()) {
    auto user_tracker = emulator_window_->emulator()
                            ->kernel_state()
                            ->xam_state()
                            ->user_tracker();

    if (user_tracker) {
      ui::IconsData icon_data;

      auto profile_manager = emulator_window_->emulator()
                                 ->kernel_state()
                                 ->xam_state()
                                 ->profile_manager();

      if (profile_manager) {
        // Check all logged in profiles
        for (uint8_t user_index = 0; user_index < 4; user_index++) {
          const auto profile = profile_manager->GetProfile(user_index);
          if (profile) {
            // Get all played titles for this profile
            auto played_titles = user_tracker->GetPlayedTitles(profile->xuid());

            // Match each recent title with played titles by name
            for (auto& recent_title : recent_titles_) {
              for (const auto& played_title : played_titles) {
                std::string played_name = xe::to_utf8(played_title.title_name);
                // Remove null terminator if present
                if (!played_name.empty() && played_name.back() == '\0') {
                  played_name.pop_back();
                }
                std::string trimmed_played = xe::string_util::trim(played_name);
                std::string trimmed_recent =
                    xe::string_util::trim(recent_title.title_name);

                if (trimmed_played == trimmed_recent) {
                  if (!played_title.icon.empty()) {
                    recent_title.icon = std::vector<uint8_t>(
                        played_title.icon.begin(), played_title.icon.end());
                    // Update the title ID from the played title
                    recent_title.title_id = played_title.id;
                    icon_data[recent_title.title_id] = recent_title.icon;
                  }
                  break;  // Found match for this recent title
                }
              }
            }
          }
        }
      }

      if (!icon_data.empty()) {
        title_icons_ = imgui_drawer()->LoadIcons(icon_data);
      }
    }
  }
}

void RecentTitlesUI::DrawTitleEntry(ImGuiIO& io, RecentTitleDisplay& entry,
                                    size_t index) {
  const auto start_position = ImGui::GetCursorPos();

  // First Column - Icon
  ImGui::TableSetColumnIndex(0);

  if (title_icons_.count(entry.title_id)) {
    ImGui::Image(
        reinterpret_cast<ImTextureID>(title_icons_.at(entry.title_id).get()),
        ui::default_image_icon_size);
  } else {
    ImGui::Dummy(ui::default_image_icon_size);
  }

  // Second Column - Title Info
  ImGui::TableNextColumn();
  ImGui::PushFont(imgui_drawer()->GetTitleFont());
  ImGui::TextUnformatted(entry.title_name.c_str());
  ImGui::PopFont();

  // Show file path
  std::string display_path = entry.path_to_file.string();
  if (display_path.length() > 60) {
    display_path = "..." + display_path.substr(display_path.length() - 57);
  }
  ImGui::TextUnformatted(display_path.c_str());

  ImGui::SetCursorPosY(start_position.y + ui::default_image_icon_size.y -
                       ImGui::GetTextLineHeight());

  if (entry.last_run_time != 0) {
    ImGui::TextUnformatted(
        fmt::format("Last played: {:%Y-%m-%d %H:%M}",
                    std::chrono::system_clock::time_point(
                        std::chrono::seconds(entry.last_run_time)))
            .c_str());
  } else {
    ImGui::TextUnformatted("Last played: Unknown");
  }

  ImGui::TableNextColumn();

  const ImVec2 end_draw_position =
      ImVec2(ImGui::GetCursorPos().x - start_position.x,
             ImGui::GetCursorPos().y - start_position.y);

  ImGui::SetCursorPos(start_position);

  // Use index for unique ID instead of title_id which might be 0
  if (ImGui::Selectable(fmt::format("##RecentTitle{}Selectable", index).c_str(),
                        selected_title_ == entry.title_id,
                        ImGuiSelectableFlags_SpanAllColumns,
                        end_draw_position)) {
    selected_title_ = entry.title_id;
    LaunchTitle(entry.path_to_file);
  }

  if (ImGui::BeginPopupContextItem(
          fmt::format("Recent Title Menu {}", index).c_str())) {
    selected_title_ = entry.title_id;

    if (ImGui::MenuItem("Launch")) {
      LaunchTitle(entry.path_to_file);
    }

    if (ImGui::MenuItem("Open containing folder")) {
      std::filesystem::path folder = entry.path_to_file.parent_path();
      std::thread path_open(LaunchFileExplorer, folder);
      path_open.detach();
    }

    ImGui::EndPopup();
  }
}

void RecentTitlesUI::LaunchTitle(const std::filesystem::path& path) {
  if (emulator_window_) {
    // Check if a child process is already running
    if (emulator_window_->HasRunningChildProcess()) {
      // Don't launch if a game is already running
      return;
    }
    // Launch the title - keep the dialog open
    emulator_window_->LaunchTitleInNewProcess(path);
  }
}

void RecentTitlesUI::OnDraw(ImGuiIO& io) {
  const auto window_position =
      ImVec2(GetIO().DisplaySize.x * 0.3f, GetIO().DisplaySize.y * 0.2f);

  ImGui::SetNextWindowPos(window_position, ImGuiCond_FirstUseEver);
  const auto xenia_window_size = ImGui::GetMainViewport()->Size;

  ImGui::SetNextWindowSizeConstraints(
      ImVec2(xenia_window_size.x * 0.3f, xenia_window_size.y * 0.2f),
      ImVec2(xenia_window_size.x * 0.5f, xenia_window_size.y * 0.6f));
  ImGui::SetNextWindowBgAlpha(0.9f);

  if (!ImGui::Begin("Recently Played Games", nullptr,
                    ImGuiWindowFlags_NoCollapse |
                        ImGuiWindowFlags_AlwaysAutoResize |
                        ImGuiWindowFlags_HorizontalScrollbar)) {
    ImGui::End();
    return;
  }

  if (!recent_titles_.empty()) {
    if (recent_titles_.size() > 5) {
      ImGui::Text("Search: ");
      ImGui::SameLine();
      ImGui::InputText("##Search", title_name_filter_, title_name_filter_size);
      ImGui::Separator();
    }

    if (ImGui::BeginTable("", 2, ImGuiTableFlags_BordersInnerH)) {
      ImGui::TableNextRow(0, ui::default_image_icon_size.y);
      size_t display_index = 0;
      for (auto& entry : recent_titles_) {
        std::string filter(title_name_filter_);
        if (!filter.empty()) {
          bool contains_filter =
              utf8::lower_ascii(entry.title_name)
                      .find(utf8::lower_ascii(filter)) != std::string::npos ||
              utf8::lower_ascii(entry.path_to_file.string())
                      .find(utf8::lower_ascii(filter)) != std::string::npos;

          if (!contains_filter) {
            continue;
          }
        }
        DrawTitleEntry(io, entry, display_index++);
      }
      ImGui::EndTable();
    }
  } else {
    // Align text to the center
    std::string no_entries_message = "No recently played games found.";

    ImGui::PushFont(imgui_drawer()->GetTitleFont());
    float windowWidth = ImGui::GetContentRegionAvail().x;
    ImVec2 textSize = ImGui::CalcTextSize(no_entries_message.c_str());
    float textOffsetX = (windowWidth - textSize.x) * 0.5f;
    if (textOffsetX > 0.0f) {
      ImGui::SetCursorPosX(ImGui::GetCursorPosX() + textOffsetX);
    }

    ImGui::Text("%s", no_entries_message.c_str());
    ImGui::PopFont();

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::Button("Open Game...")) {
      emulator_window_->FileOpen();
    }
  }

  ImGui::End();
}

}  // namespace app
}  // namespace xe

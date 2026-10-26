// fable2_launcher_dialog.h - the pre-launch settings menu, drawn by the SDK's
// own ImGui overlay inside the game window (so it works on every port that
// has a window). Header-only. Keyboard, mouse and gamepad (SDL, polled
// directly because the guest input system isn't running yet).
//
// The dialog deletes itself when closed (SDK ImGuiDialog contract), so it
// never outlives its callback: it hands the final choices to `done`.
#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#if defined(__has_include)
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#define FABLE2_LAUNCHER_HAS_SDL 1
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fable2::launcher {

struct Choice {
  std::string label;
  std::string value;
};

struct Item {
  std::string key;    // cvar name
  std::string label;  // shown to the player
  std::string help;   // one-line explanation
  std::vector<Choice> choices;
  int selected = 0;
  // Optional: an action row (no choices). Runs on Enter / A / click and returns a
  // one-line status that the menu shows.
  std::function<std::string()> action;
  // Optional: an editable text row (no choices). The string is shared with the
  // caller, so edits are visible to action callbacks immediately.
  std::shared_ptr<std::string> text;
  // Optional: applies the chosen value on Start (for settings that map to more
  // than one cvar, or to something that isn't a cvar).
  std::function<void(const std::string&)> apply;
  // Optional: called after the player changes this row (used by presets to move
  // other rows).
  std::function<void(std::vector<Item>&)> on_change;
};

class LauncherDialog final : public rex::ui::ImGuiDialog {
 public:
  using DoneFn = std::function<void(bool start, const std::vector<Item>&)>;

  LauncherDialog(rex::ui::ImGuiDrawer* drawer, std::string title, std::vector<Item> items,
                 DoneFn done)
      : rex::ui::ImGuiDialog(drawer),
        title_(std::move(title)),
        items_(std::move(items)),
        done_(std::move(done)) {}

  ~LauncherDialog() override {
#ifdef FABLE2_LAUNCHER_HAS_SDL
    if (pad_) SDL_CloseGamepad(pad_);
    if (pad_init_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
  }

 protected:
  void OnShow() override {}

  void OnDraw(ImGuiIO& io) override {
    if (finished_) return;  // Close() is pending; draw nothing more.

    Actions a;
    if (!editing_) {
      a = ReadKeyboard();
      MergePad(a);
    }

    const int rows = int(items_.size()) + 2;  // + Start, Quit
    const int kStart = int(items_.size());
    if (a.up) row_ = (row_ + rows - 1) % rows;
    if (a.down) row_ = (row_ + 1) % rows;
    if (row_ < kStart) {
      auto& it = items_[row_];
      const int n = int(it.choices.size());
      if (it.action && a.accept) status_ = it.action();
      if (it.text && a.accept) { editing_ = true; focus_edit_ = true; }
      if (n > 0 && a.left) { it.selected = (it.selected + n - 1) % n; Changed(row_); }
      if (n > 0 && (a.right || a.accept)) { it.selected = (it.selected + 1) % n; Changed(row_); }
    } else if (a.accept) {
      Finish(row_ == kStart);
      return;
    }

    const float s = std::clamp(io.DisplaySize.y / 1080.0f, 0.8f, 2.5f);

    // Dim the game window behind the menu.
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.02f, 0.02f, 0.92f));
    ImGui::Begin("##fable2_backdrop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    const float panel_w = std::min(io.DisplaySize.x - 40.0f, 860.0f * s);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(panel_w, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28 * s, 22 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f * s);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.10f, 0.08f, 0.06f, 0.97f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.78f, 0.62f, 0.30f, 0.85f));
    ImGui::Begin("##fable2_launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::SetWindowFontScale(1.25f * s);

    // Title, centred.
    {
      const float tw = ImGui::CalcTextSize(title_.c_str()).x;
      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - tw) * 0.5f);
      ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1.0f), "%s", title_.c_str());
      ImGui::Dummy(ImVec2(0, 10 * s));
    }

    const float inner_w = ImGui::GetContentRegionAvail().x;
    const float row_h = ImGui::GetTextLineHeight() * 1.5f;
    const ImVec4 gold(0.95f, 0.80f, 0.45f, 1.0f), grey(0.80f, 0.78f, 0.74f, 1.0f);

    for (int i = 0; i < int(items_.size()); ++i) {
      ImGui::PushID(i);
      const bool sel = (row_ == i);
      const ImVec2 p = ImGui::GetCursorPos();
      if (ImGui::Selectable("##row", sel, 0, ImVec2(inner_w, row_h))) {
        row_ = i;
        if (items_[i].action) status_ = items_[i].action();
        if (items_[i].text) { editing_ = true; focus_edit_ = true; }
        const int n = int(items_[i].choices.size());
        // Click the right half to change the value forward, left half selects.
        if (n > 0 && ImGui::GetMousePos().x > ImGui::GetWindowPos().x + panel_w * 0.5f) {
          items_[i].selected = (items_[i].selected + 1) % n;
          Changed(i);
        }
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        row_ = i;
        const int n = int(items_[i].choices.size());
        if (n > 0) { items_[i].selected = (items_[i].selected + n - 1) % n; Changed(i); }
      }
      ImGui::SetCursorPos(ImVec2(p.x + 8 * s, p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(sel ? gold : grey, "%s", items_[i].label.c_str());
      if (items_[i].text && editing_ && sel) {
        // Inline editor replaces the value text while editing.
        ImGui::SetCursorPos(ImVec2(p.x + inner_w * 0.32f, p.y));
        ImGui::SetNextItemWidth(inner_w * 0.66f);
        if (focus_edit_) { ImGui::SetKeyboardFocusHere(); focus_edit_ = false; }
        char buf[1024];
        std::snprintf(buf, sizeof buf, "%s", items_[i].text->c_str());
        if (ImGui::InputText("##edit", buf, sizeof buf, ImGuiInputTextFlags_EnterReturnsTrue)) {
          *items_[i].text = buf;
          editing_ = false;
        } else {
          *items_[i].text = buf;
          if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) editing_ = false;
        }
        ImGui::SetCursorPos(ImVec2(p.x, p.y + row_h + 2 * s));
        ImGui::PopID();
        continue;
      }
      std::string v = items_[i].text ? (items_[i].text->empty() ? std::string("(default)") : *items_[i].text)
                      : items_[i].action ? std::string("[ Enter ]")
                      : items_[i].choices.empty()
                          ? std::string("-")
                          : items_[i].choices[items_[i].selected].label;
      if (sel && !items_[i].action && !items_[i].text) v = "<  " + v + "  >";
      const float max_vw = inner_w * 0.62f;
      while (v.size() > 8 && ImGui::CalcTextSize(v.c_str()).x > max_vw) v.erase(v.begin(), v.begin() + 4), v.replace(0, 3, "...");
      const float vw = ImGui::CalcTextSize(v.c_str()).x;
      ImGui::SetCursorPos(ImVec2(p.x + inner_w - vw - 8 * s,
                                 p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(sel ? gold : grey, "%s", v.c_str());
      ImGui::SetCursorPos(ImVec2(p.x, p.y + row_h + 2 * s));
      ImGui::PopID();
    }

    ImGui::Dummy(ImVec2(0, 8 * s));
    const char* names[2] = {"Start Game", "Quit"};
    for (int b = 0; b < 2; ++b) {
      const int idx = kStart + b;
      ImGui::PushID(1000 + b);
      const bool sel = (row_ == idx);
      const ImVec2 p = ImGui::GetCursorPos();
      if (ImGui::Selectable("##btn", sel, 0, ImVec2(inner_w, row_h * 1.1f))) {
        row_ = idx;
        Finish(b == 0);
      }
      const float tw = ImGui::CalcTextSize(names[b]).x;
      ImGui::SetCursorPos(ImVec2(p.x + (inner_w - tw) * 0.5f,
                                 p.y + (row_h * 1.1f - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(sel ? gold : grey, "%s", names[b]);
      ImGui::SetCursorPos(ImVec2(p.x, p.y + row_h * 1.1f + 2 * s));
      ImGui::PopID();
      if (finished_) break;
    }

    if (!finished_) {
      ImGui::Dummy(ImVec2(0, 8 * s));
      ImGui::SetWindowFontScale(0.9f * s);
      std::string help = (row_ < int(items_.size())) ? items_[row_].help
                                                     : (row_ == kStart ? "Launch Fable II with these settings."
                                                                       : "Close without starting.");
      ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 28 * s);
      if (!status_.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", status_.c_str());
        ImGui::Dummy(ImVec2(0, 4 * s));
      }
      ImGui::TextColored(ImVec4(0.65f, 0.63f, 0.58f, 1.0f), "%s", help.c_str());
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, 4 * s));
      ImGui::TextColored(ImVec4(0.55f, 0.53f, 0.50f, 1.0f),
                         "Up/Down: select    Left/Right: change    Enter / A: confirm");
    }

    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
  }

 private:
  struct Actions {
    bool up = false, down = false, left = false, right = false, accept = false;
  };

  Actions ReadKeyboard() {
    Actions a;
    a.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow, true) || ImGui::IsKeyPressed(ImGuiKey_W, false);
    a.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow, true) || ImGui::IsKeyPressed(ImGuiKey_S, false);
    a.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true) || ImGui::IsKeyPressed(ImGuiKey_A, false);
    a.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow, true) || ImGui::IsKeyPressed(ImGuiKey_D, false);
    a.accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_Space, false);
    return a;
  }

#ifdef FABLE2_LAUNCHER_HAS_SDL
  void MergePad(Actions& a) {
    if (!pad_init_) {
      pad_init_ = true;
      if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) pad_init_ = false;
    }
    if (!pad_ && pad_init_ && ImGui::GetTime() >= next_scan_) {
      next_scan_ = ImGui::GetTime() + 1.0;
      int n = 0;
      if (SDL_JoystickID* ids = SDL_GetGamepads(&n)) {
        if (n > 0) pad_ = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
      }
    }
    if (!pad_) return;
    if (!SDL_GamepadConnected(pad_)) {
      SDL_CloseGamepad(pad_);
      pad_ = nullptr;
      return;
    }
    SDL_UpdateGamepads();
    auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
    const int ly = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTY);
    const int lx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
    const bool up = btn(SDL_GAMEPAD_BUTTON_DPAD_UP) || ly < -16000;
    const bool down = btn(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || ly > 16000;
    const bool left = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16000;
    const bool right = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16000;
    const bool ok = btn(SDL_GAMEPAD_BUTTON_SOUTH) || btn(SDL_GAMEPAD_BUTTON_START);

    // Edge + auto-repeat (0.4 s, then every 0.12 s) for directions.
    const double now = ImGui::GetTime();
    auto rep = [&](bool down_now, int idx) {
      bool fire = false;
      if (down_now) {
        if (!held_[idx]) { held_[idx] = true; hold_t_[idx] = now + 0.4; fire = true; }
        else if (now >= hold_t_[idx]) { hold_t_[idx] = now + 0.12; fire = true; }
      } else {
        held_[idx] = false;
      }
      return fire;
    };
    a.up |= rep(up, 0);
    a.down |= rep(down, 1);
    a.left |= rep(left, 2);
    a.right |= rep(right, 3);
    if (ok && !ok_held_) a.accept = true;
    ok_held_ = ok;
  }
#else
  void MergePad(Actions&) {}
#endif

  // A row changed: run its hook, or (for any other row) flip the preset back to Custom.
  void Changed(int idx) {
    if (items_[idx].on_change) {
      items_[idx].on_change(items_);
    } else if (items_[idx].key != "preset") {
      for (auto& it : items_)
        if (it.key == "preset") it.selected = 0;
    }
  }

  void Finish(bool start) {
    if (finished_) return;
    finished_ = true;
    Close();  // base deletes this object after OnDraw returns
    if (done_) done_(start, items_);
  }

  std::string title_;
  std::vector<Item> items_;
  DoneFn done_;
  int row_ = 0;
  std::string status_;
  bool editing_ = false;
  bool focus_edit_ = false;
  bool finished_ = false;

#ifdef FABLE2_LAUNCHER_HAS_SDL
  SDL_Gamepad* pad_ = nullptr;
  bool pad_init_ = false;
  double next_scan_ = 0.0;
  bool held_[4] = {false, false, false, false};
  double hold_t_[4] = {0, 0, 0, 0};
  bool ok_held_ = true;  // ignore a button that's already down when the menu opens
#endif
};

}  // namespace fable2::launcher

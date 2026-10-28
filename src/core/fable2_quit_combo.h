// fable2_quit_combo.h - a controller-friendly way to close the game without Alt+F4.
//
//   * Hold Start + Select (Back) for 3 seconds on a gamepad, or press Ctrl+Q on the keyboard.
//   * A "Quit Fable II?" box appears; choose Yes / No with the d-pad (or Left/Right) and
//     A / Enter to confirm, B / Esc to cancel. The mouse works too.
//
// Always-on overlay drawn through the SDK's ImGui layer. The gamepad is read directly
// through SDL (the same way the launch menu does), so it works while the game runs.
#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#if defined(__has_include)
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#define FABLE2_QUIT_HAS_SDL 1
#endif
#endif

#include <algorithm>
#include <functional>
#include <utility>

namespace fable2 {

class QuitComboDialog final : public rex::ui::ImGuiDialog {
 public:
  QuitComboDialog(rex::ui::ImGuiDrawer* drawer, std::function<void()> on_quit,
                  double hold_seconds = 3.0)
      : rex::ui::ImGuiDialog(drawer), on_quit_(std::move(on_quit)), hold_(hold_seconds) {}

  ~QuitComboDialog() override {
#ifdef FABLE2_QUIT_HAS_SDL
    if (pad_) SDL_CloseGamepad(pad_);
    if (pad_init_) SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
#endif
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (quitting_) return;
    const double now = ImGui::GetTime();
    const Pad p = ReadPad();
    const float s = std::clamp(io.DisplaySize.y / 1080.0f, 0.8f, 2.5f);

    if (!confirm_) {
      const bool ctrl = io.KeyCtrl || ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
      const bool kb = ctrl && ImGui::IsKeyPressed(ImGuiKey_Q, false);
      // After a cancel, the combo must be let go before it can count again.
      if (!p.combo) wait_release_ = false;
      if (p.combo && !wait_release_) {
        if (hold_start_ < 0) hold_start_ = now;
      } else {
        hold_start_ = -1;
      }
      if (kb || (hold_start_ >= 0 && now - hold_start_ >= hold_)) {
        confirm_ = true;
        sel_ = 1;  // "No" is the safe default
        armed_ = false;  // ignore input until the combo / keys are released
        hold_start_ = -1;
      } else if (hold_start_ >= 0 && now - hold_start_ > 0.4) {
        DrawHoldBar(io, s, float((now - hold_start_) / hold_));
      }
      prev_ = p;
      return;
    }

    // ---- Confirm box
    if (!armed_) {
      if (!p.combo && !p.a && !p.b) armed_ = true;
    } else {
      const bool left = (p.left && !prev_.left) || ImGui::IsKeyPressed(ImGuiKey_LeftArrow, false);
      const bool right = (p.right && !prev_.right) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
      const bool ok = (p.a && !prev_.a) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                      ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
      const bool cancel = (p.b && !prev_.b) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
      if (left) sel_ = 0;
      if (right) sel_ = 1;
      if (cancel) { Cancel(); prev_ = p; return; }
      if (ok) { if (sel_ == 0) { DoQuit(); prev_ = p; return; } Cancel(); prev_ = p; return; }
    }
    prev_ = p;
    if (DrawConfirm(io, s)) return;  // a click chose something
  }

 private:
  struct Pad {
    bool combo = false, a = false, b = false, left = false, right = false;
  };

  void Cancel() {
    confirm_ = false;
    wait_release_ = true;
    hold_start_ = -1;
  }
  void DoQuit() {
    quitting_ = true;
    confirm_ = false;
    if (on_quit_) on_quit_();
  }

  Pad ReadPad() {
    Pad r;
#ifdef FABLE2_QUIT_HAS_SDL
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
    if (pad_) {
      if (!SDL_GamepadConnected(pad_)) {
        SDL_CloseGamepad(pad_);
        pad_ = nullptr;
      } else {
        SDL_UpdateGamepads();
        auto btn = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad_, b); };
        const int lx = SDL_GetGamepadAxis(pad_, SDL_GAMEPAD_AXIS_LEFTX);
        r.combo = btn(SDL_GAMEPAD_BUTTON_START) && btn(SDL_GAMEPAD_BUTTON_BACK);
        r.a = btn(SDL_GAMEPAD_BUTTON_SOUTH);
        r.b = btn(SDL_GAMEPAD_BUTTON_EAST);
        r.left = btn(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || lx < -16000;
        r.right = btn(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || lx > 16000;
      }
    }
#endif
    return r;
  }

  static ImU32 C(float r, float g, float b, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
  }

  void DrawHoldBar(ImGuiIO& io, float s, float frac) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 70.0f * s),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.8f);
    ImGui::Begin("##fable2_quit_hold", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(1.5f * s);
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1.0f), "Keep holding Start + Select to quit...");
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 8.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), C(0.2f, 0.17f, 0.12f), h * 0.5f);
    dl->AddRectFilled(p, ImVec2(p.x + w * std::clamp(frac, 0.0f, 1.0f), p.y + h), C(0.93f, 0.78f, 0.42f), h * 0.5f);
    ImGui::Dummy(ImVec2(0, h + 2 * s));
    ImGui::End();
  }

  // Returns true if the player clicked a choice (the dialog may already be quitting).
  bool DrawConfirm(ImGuiIO& io, float s) {
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(36 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.065f, 0.052f, 0.040f, 0.98f));
    ImGui::Begin("##fable2_quit_confirm", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(1.8f * s);
    bool clicked = false;
    const char* title = "Quit Fable II?";
    const float bw = 150.0f * s, bh = ImGui::GetTextLineHeight() * 1.7f, gap = 24.0f * s;
    const float total = bw * 2 + gap;
    const float content_w = std::max(total, ImGui::CalcTextSize(title).x);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (content_w - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1.0f), "%s", title);
    ImGui::Dummy(ImVec2(0, 14 * s));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* names[2] = {"Yes", "No"};
    for (int i = 0; i < 2; ++i) {
      const float x = start.x + (content_w - total) * 0.5f + i * (bw + gap);
      const bool sel = (sel_ == i);
      const ImU32 fill = sel ? C(0.27f, 0.53f, 0.16f) : C(0.10f, 0.08f, 0.06f);
      dl->AddRectFilled(ImVec2(x, start.y), ImVec2(x + bw, start.y + bh), fill, bh * 0.25f);
      dl->AddRect(ImVec2(x, start.y), ImVec2(x + bw, start.y + bh),
                  sel ? C(0.93f, 0.78f, 0.42f) : C(0.62f, 0.50f, 0.27f), bh * 0.25f, 0, (sel ? 2.0f : 1.2f) * s);
      ImGui::SetCursorScreenPos(ImVec2(x, start.y));
      ImGui::PushID(i);
      if (ImGui::InvisibleButton("##b", ImVec2(bw, bh)) && armed_) {
        sel_ = i;
        clicked = true;
      }
      ImGui::PopID();
      const float tw = ImGui::CalcTextSize(names[i]).x;
      dl->AddText(ImVec2(x + (bw - tw) * 0.5f, start.y + (bh - ImGui::GetTextLineHeight()) * 0.5f),
                  sel ? C(1.0f, 0.93f, 0.70f) : C(0.84f, 0.79f, 0.69f), names[i]);
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + bh + 12 * s));
    ImGui::SetWindowFontScale(1.2f * s);
    const char* hint = "Left / Right: choose     A / Enter: confirm     B / Esc: cancel";
    const float hw = ImGui::CalcTextSize(hint).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (content_w - hw) * 0.5f));
    ImGui::TextColored(ImVec4(0.62f, 0.58f, 0.52f, 1.0f), "%s", hint);

    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::GetForegroundDrawList()->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y),
                                            C(0.93f, 0.78f, 0.42f, 0.95f), 8 * s, 0, 2.0f * s);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    if (clicked) {
      if (sel_ == 0) DoQuit(); else Cancel();
    }
    return clicked;
  }

  std::function<void()> on_quit_;
  double hold_;
  double hold_start_ = -1.0;
  bool confirm_ = false, armed_ = false, wait_release_ = false, quitting_ = false;
  int sel_ = 1;
  Pad prev_;

#ifdef FABLE2_QUIT_HAS_SDL
  SDL_Gamepad* pad_ = nullptr;
  bool pad_init_ = false;
  double next_scan_ = 0.0;
#endif
};

}  // namespace fable2

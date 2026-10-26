// fable2_fps_counter.h - small always-on FPS readout in the corner of the game
// window, drawn through the SDK's ImGui overlay. Shows the game's own frame rate
// ("Guest") and how often the screen is actually refreshed ("Display").
#pragma once

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <functional>

namespace fable2 {

class FpsCounterDialog final : public rex::ui::ImGuiDialog {
 public:
  // guest_frame_time_us: returns the game's last frame time in microseconds
  // (<= 0 when unknown). `detailed` adds the frame time in milliseconds.
  FpsCounterDialog(rex::ui::ImGuiDrawer* drawer, bool detailed,
                   std::function<double()> guest_frame_time_us)
      : rex::ui::ImGuiDialog(drawer),
        detailed_(detailed),
        guest_us_(std::move(guest_frame_time_us)) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    // Smooth the instantaneous guest frame time so the number is readable.
    if (guest_us_) {
      const double us = guest_us_();
      if (us > 0.0) ema_us_ = (ema_us_ <= 0.0) ? us : ema_us_ * 0.92 + us * 0.08;
    }
    const float s = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    ImGui::SetNextWindowPos(ImVec2(10.0f * s, 10.0f * s));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fable2_fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(1.1f * s);
    char buf[96];
    if (ema_us_ > 0.0) {
      if (detailed_)
        std::snprintf(buf, sizeof buf, "Game  %.0f fps  (%.1f ms)", 1e6 / ema_us_, ema_us_ / 1000.0);
      else
        std::snprintf(buf, sizeof buf, "Game  %.0f fps", 1e6 / ema_us_);
    } else {
      std::snprintf(buf, sizeof buf, "Game  -- fps");
    }
    ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "%s", buf);
    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "Display  %.0f fps", io.Framerate);
    ImGui::End();
  }

 private:
  bool detailed_;
  std::function<double()> guest_us_;
  double ema_us_ = 0.0;
};

}  // namespace fable2

// fable2_launcher_dialog.h - the pre-launch settings menu, drawn by the SDK's
// own ImGui overlay inside the game window (so it works on every port that
// has a window). Header-only. Keyboard, mouse and gamepad (SDL, polled
// directly because the guest input system isn't running yet).
//
// The dialog deletes itself when closed (SDK ImGuiDialog contract), so it
// never outlives its callback: it hands the final choices to `done`.
#pragma once

#include <rex/ui/image_decode.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include <imgui.h>

#include "fable2_browser.h"

#if defined(__has_include)
#if __has_include(<SDL3/SDL.h>)
#include <SDL3/SDL.h>
#define FABLE2_LAUNCHER_HAS_SDL 1
#endif
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
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
  // Optional: opens the folder / file picker; the request says what to do with the choice.
  std::function<BrowseRequest()> browse;
  // Optional: an editable text row (no choices). The string is shared with the
  // caller, so edits are visible to action callbacks immediately.
  std::shared_ptr<std::string> text;
  // Optional: applies the chosen value on Start (for settings that map to more
  // than one cvar, or to something that isn't a cvar).
  std::function<void(const std::string&)> apply;
  // Optional: called after the player changes this row (used by presets to move
  // other rows).
  std::function<void(std::vector<Item>&)> on_change;
  // Menu page this row lives on (0 = first). Pages are switched with LB/RB, Tab or PageUp/PageDown.
  int page = 0;
  // Optional heading drawn above this row (rows after it share the heading until the next one).
  std::string section;
  // Optional: when it returns false the row is shown greyed out and can't be changed or run.
  std::function<bool(const std::vector<Item>&)> enabled;
  // Help text shown instead of `help` while the row is greyed out.
  std::string disabled_help;
};

namespace detail {
// Where optional artwork (launcher_bg.png / launcher_logo.png) is looked for.
struct ImageSource {
  rex::ui::ImmediateDrawer* drawer = nullptr;
  std::filesystem::path dir;
};
inline ImageSource& Source() {
  static ImageSource s;
  return s;
}
struct Img {
  rex::ui::ImmediateTexture* tex = nullptr;
  int w = 0, h = 0;
};
// Loads <dir>/<name> (PNG) as a texture, or returns an empty Img if it isn't there / won't decode.
// The texture is intentionally never freed: the dialog deletes itself while the GPU may
// still hold the last frame, and the process hard-exits anyway.
inline Img LoadPng(const char* name) {
  Img r;
  const ImageSource& s = Source();
  if (!s.drawer || s.dir.empty()) return r;
  std::error_code ec;
  const std::filesystem::path p = s.dir / name;
  if (!std::filesystem::is_regular_file(p, ec)) return r;
  std::ifstream f(p, std::ios::binary);
  if (!f) return r;
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty() || bytes.size() > (64u << 20)) return r;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba = rex::ui::DecodeImageRGBA(bytes.data(), bytes.size(), w, h);
  if (rgba.empty() || w <= 0 || h <= 0 || w > 8192 || h > 8192) return r;
  auto tex = s.drawer->CreateTexture(uint32_t(w), uint32_t(h), rex::ui::ImmediateTextureFilter::kLinear,
                                     false, rgba.data());
  if (!tex) return r;
  r.tex = tex.release();
  r.w = w;
  r.h = h;
  return r;
}
// Optional launcher_bg.layout: four numbers "x0 y0 x1 y1" (fractions of the background image) saying
// where the menu box goes. Used when the artwork already has a panel drawn in it.
inline bool LoadLayout(float out[4]) {
  const ImageSource& s = Source();
  if (s.dir.empty()) return false;
  std::ifstream f(s.dir / "launcher_bg.layout");
  float v[4];
  if (!f || !(f >> v[0] >> v[1] >> v[2] >> v[3])) return false;
  if (!(v[0] >= 0 && v[1] >= 0 && v[2] <= 1 && v[3] <= 1 && v[2] - v[0] > 0.1f && v[3] - v[1] > 0.1f)) return false;
  for (int i = 0; i < 4; ++i) out[i] = v[i];
  return true;
}
}  // namespace detail

// Optional menu font: launcher_font.ttf next to the exe. Loaded into the ImGui atlas at startup
// (call from ReXApp::OnConfigureFonts). Without the file the menu keeps the built-in font.
namespace detail {
inline ImFont*& MenuFont() {
  static ImFont* f = nullptr;
  return f;
}
constexpr float kMenuFontPx = 40.0f;  // size the font is rasterised at; the menu scales it down
}  // namespace detail

inline void LoadMenuFont(ImFontAtlas* atlas, const std::filesystem::path& dir) {
  if (!atlas || dir.empty()) return;
  std::error_code ec;
  const std::filesystem::path p = dir / "launcher_font.ttf";
  if (!std::filesystem::is_regular_file(p, ec)) return;
  const auto sz = std::filesystem::file_size(p, ec);
  if (ec || sz < 1024 || sz > (16u << 20)) return;
  {  // must look like a TrueType / OpenType file
    std::ifstream f(p, std::ios::binary);
    char m[4] = {};
    if (!f.read(m, 4)) return;
    const bool ok = (m[0] == 0 && m[1] == 1 && m[2] == 0 && m[3] == 0) || std::string(m, 4) == "OTTO" ||
                    std::string(m, 4) == "true";
    if (!ok) return;
  }
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2018, 0x201D, 0};
  ImFontConfig cfg;
  cfg.OversampleH = 2;
  cfg.OversampleV = 2;
  cfg.GlyphRanges = ranges;
  detail::MenuFont() = atlas->AddFontFromFileTTF(p.string().c_str(), detail::kMenuFontPx, &cfg);
}

// Call once before showing the menu: where to look for launcher_bg.png / launcher_logo.png.
inline void SetImageSource(rex::ui::ImmediateDrawer* drawer, std::filesystem::path dir) {
  detail::Source().drawer = drawer;
  detail::Source().dir = std::move(dir);
}

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

  static ImU32 C(float r, float g, float b, float a = 1.0f) {
    return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a));
  }
  static void Diamond(ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r),
                      ImVec2(c.x - r, c.y), col);
  }
  // A thin rule that fades out at both ends with a diamond in the middle.
  static void Divider(ImDrawList* dl, float x0, float x1, float y, float s) {
    const float xm = (x0 + x1) * 0.5f;
    const ImU32 g = C(0.93f, 0.78f, 0.42f, 0.9f), f = C(0.93f, 0.78f, 0.42f, 0.0f);
    dl->AddRectFilledMultiColor(ImVec2(x0, y - 0.8f * s), ImVec2(xm - 12 * s, y + 0.8f * s), f, g, g, f);
    dl->AddRectFilledMultiColor(ImVec2(xm + 12 * s, y - 0.8f * s), ImVec2(x1, y + 0.8f * s), g, f, f, g);
    Diamond(dl, ImVec2(xm, y), 5.0f * s, g);
    Diamond(dl, ImVec2(xm - 13 * s, y), 2.2f * s, g);
    Diamond(dl, ImVec2(xm + 13 * s, y), 2.2f * s, g);
  }

  void OnDraw(ImGuiIO& io) override {
    if (finished_) return;  // Close() is pending; draw nothing more.
    if (!imgs_loaded_) {
      imgs_loaded_ = true;
      bg_ = detail::LoadPng("launcher_bg.png");
      logo_ = detail::LoadPng("launcher_logo.png");
      has_layout_ = bg_.tex && detail::LoadLayout(lay_);
    }

    Actions a;
    if (!editing_) {
      a = ReadKeyboard();
      MergePad(a);
    }
    if (br_) {  // the picker owns the input while it is open
      Browser::Keys k{a.up, a.down, a.left, a.right, a.accept, a.back};
      a = Actions{};
      const auto r = br_->Update(k);
      HandleBrowser(r);
    }

    if (pending_page_ >= 0) { SetPage(pending_page_); pending_page_ = -1; }
    if (a.pg_prev) SetPage(page_ - 1);
    if (a.pg_next) SetPage(page_ + 1);
    std::vector<int> vis;  // items shown on this page
    for (int i = 0; i < int(items_.size()); ++i)
      if (items_[i].page == page_) vis.push_back(i);
    const int rows = int(vis.size()) + 2;  // + Start, Quit
    const int kStart = int(vis.size());
    if (a.up) row_ = (row_ + rows - 1) % rows;
    if (a.down) row_ = (row_ + 1) % rows;
    if (row_ < kStart && IsEnabled(vis[row_])) {
      const int ii = vis[row_];
      auto& it = items_[ii];
      const int n = int(it.choices.size());
      if (it.action && a.accept) status_ = it.action();
      if (it.browse && a.accept) br_ = std::make_unique<Browser>(it.browse());
      if (it.text && !it.browse && a.accept) { editing_ = true; focus_edit_ = true; }
      if (n > 0 && a.left) { it.selected = (it.selected + n - 1) % n; Changed(ii); }
      if (n > 0 && (a.right || a.accept)) { it.selected = (it.selected + 1) % n; Changed(ii); }
    } else if (row_ >= kStart && a.accept) {
      Finish(row_ == kStart);
      return;
    }

    float s = std::clamp(io.DisplaySize.y / 1080.0f, 0.8f, 2.5f);
    // Artwork is scaled to cover the whole window (cropped, never stretched).
    ImVec2 buv0(0, 0), buv1(1, 1), lay_min(0, 0), lay_max(0, 0);
    ImVec2 img_p0(0, 0), img_p1 = io.DisplaySize;  // where the artwork lands on screen
    if (bg_.tex) {
      const float da = io.DisplaySize.x / io.DisplaySize.y, ia = float(bg_.w) / float(bg_.h);
      if (ia > da) { const float k = da / ia; buv0.x = (1 - k) * 0.5f; buv1.x = buv0.x + k; }
      else if (has_layout_) {
        // Artwork with a built-in panel on a wider screen: keep all of it, dark bars at the sides.
        const float iw = io.DisplaySize.y * ia;
        img_p0.x = (io.DisplaySize.x - iw) * 0.5f;
        img_p1.x = img_p0.x + iw;
      } else { const float k = ia / da; buv0.y = (1 - k) * 0.5f; buv1.y = buv0.y + k; }
      if (has_layout_) {
        auto mx = [&](float f) { return img_p0.x + (f - buv0.x) / (buv1.x - buv0.x) * (img_p1.x - img_p0.x); };
        auto my = [&](float f) { return img_p0.y + (f - buv0.y) / (buv1.y - buv0.y) * (img_p1.y - img_p0.y); };
        lay_min = ImVec2(mx(lay_[0]), my(lay_[1]));
        lay_max = ImVec2(mx(lay_[2]), my(lay_[3]));
        s = (lay_max.y - lay_min.y) / 700.0f;  // everything in the box scales with its height
      }
    }
    const ImU32 gold = C(0.93f, 0.78f, 0.42f), gold_dim = C(0.62f, 0.50f, 0.27f);
    const ImVec4 txt_sel(1.0f, 0.93f, 0.70f, 1.0f), txt(0.84f, 0.79f, 0.69f, 1.0f);

    // ---- Backdrop: a warm dark gradient with darker edges (covers the game window).
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##fable2_backdrop", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    {
      ImDrawList* bg = ImGui::GetWindowDrawList();
      const ImVec2 d = io.DisplaySize;
      {
        const ImU32 top = C(0.17f, 0.11f, 0.07f, 0.98f), bot = C(0.035f, 0.025f, 0.02f, 0.99f);
        bg->AddRectFilledMultiColor(ImVec2(0, 0), d, top, top, bot, bot);
      }
      if (bg_.tex)
        bg->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(bg_.tex)), img_p0, img_p1, buv0, buv1);
      if (!bg_.tex) {
        const ImU32 dark = C(0, 0, 0, 0.55f), none = C(0, 0, 0, 0);
        bg->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(d.x * 0.28f, d.y), dark, none, none, dark);
        bg->AddRectFilledMultiColor(ImVec2(d.x * 0.72f, 0), d, none, dark, dark, none);
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    // ---- The panel.
    const float panel_w = std::min(io.DisplaySize.x - 40.0f, 1000.0f * s);
    if (has_layout_) {
      ImGui::SetNextWindowPos(lay_min);
      ImGui::SetNextWindowSize(ImVec2(lay_max.x - lay_min.x, lay_max.y - lay_min.y));
    } else {
      ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                              ImGuiCond_Always, ImVec2(0.5f, 0.5f));
      ImGui::SetNextWindowSize(ImVec2(panel_w, 0));
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40 * s, 28 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.065f, 0.052f, 0.040f, has_layout_ ? 0.99f : 0.965f));
    ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.05f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(0, 0, 0, 0));
    ImGui::Begin("##fable2_launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     (has_layout_ ? 0 : ImGuiWindowFlags_AlwaysAutoResize));
    // With the optional menu font, the menu text is drawn with it. The font is rasterised at
    // kMenuFontPx and scaled down; fk keeps the on-screen size close to the built-in font's.
    ImFont* menu_font = detail::MenuFont();
    const float fk = menu_font ? (10.0f / detail::kMenuFontPx) * 1.25f : 1.0f;
    if (menu_font) {
#if defined(IMGUI_VERSION_NUM) && IMGUI_VERSION_NUM >= 19200
      ImGui::PushFont(menu_font, detail::kMenuFontPx);
#else
      ImGui::PushFont(menu_font);
#endif
    }
    ImGui::SetWindowFontScale(1.7f * s * fk);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos();
    const float ww = ImGui::GetWindowWidth();
    const float base = ImGui::GetTextLineHeight();
    ImFont* font = ImGui::GetFont();

    // Title: the player's logo image if there is one, else big text + spaced subtitle.
    float centre_off = 0.0f;
    if (has_layout_) {
      // The artwork already has the title; centre the rows vertically in the box instead.
      centre_off = std::max(0.0f, (ImGui::GetWindowHeight() - 56 * s - content_h_) * 0.5f);
      ImGui::Dummy(ImVec2(0, centre_off));
    } else if (logo_.tex) {
      const float maxw = ww * 0.66f, maxh = base * 8.5f;
      const float k = std::min(maxw / float(logo_.w), maxh / float(logo_.h));
      const float lw = float(logo_.w) * k, lh = float(logo_.h) * k;
      const ImVec2 cp = ImGui::GetCursorScreenPos();
      const ImVec2 p0(wp.x + (ww - lw) * 0.5f, cp.y);
      dl->AddImage(ImTextureRef(reinterpret_cast<ImTextureID>(logo_.tex)), p0, ImVec2(p0.x + lw, p0.y + lh));
      ImGui::Dummy(ImVec2(0, lh + 8 * s));
      Divider(dl, wp.x + 40 * s, wp.x + ww - 40 * s, ImGui::GetCursorScreenPos().y + 4 * s, s);
      ImGui::Dummy(ImVec2(0, 16 * s));
    } else {
      std::string big = title_;
      for (auto& ch : big) ch = char(std::toupper((unsigned char)ch));
      const float tsz = base * 4.0f;
      const ImVec2 ts = font->CalcTextSizeA(tsz, 1e9f, 0.0f, big.c_str());
      const ImVec2 cp = ImGui::GetCursorScreenPos();
      const ImVec2 tp(wp.x + (ww - ts.x) * 0.5f, cp.y);
      dl->AddText(font, tsz, ImVec2(tp.x + 3 * s, tp.y + 4 * s), C(0, 0, 0, 0.75f), big.c_str());
      dl->AddText(font, tsz, tp, C(0.95f, 0.84f, 0.55f), big.c_str());
      ImGui::Dummy(ImVec2(0, ts.y + 2 * s));

      const char* sub = "ENHANCED LAUNCHER";
      const float ssz = base * 1.15f, gap = base * 0.30f;
      float total = 0;
      for (const char* c = sub; *c; ++c) total += font->CalcTextSizeA(ssz, 1e9f, 0, c, c + 1).x + gap;
      total -= gap;
      float x = wp.x + (ww - total) * 0.5f;
      const float y = ImGui::GetCursorScreenPos().y;
      for (const char* c = sub; *c; ++c) {
        dl->AddText(font, ssz, ImVec2(x, y), gold, c, c + 1);
        x += font->CalcTextSizeA(ssz, 1e9f, 0, c, c + 1).x + gap;
      }
      ImGui::Dummy(ImVec2(0, ssz + 8 * s));
      const ImVec2 dp = ImGui::GetCursorScreenPos();
      Divider(dl, wp.x + 40 * s, wp.x + ww - 40 * s, dp.y + 4 * s, s);
      ImGui::Dummy(ImVec2(0, 16 * s));
    }

    const float inner_w = ImGui::GetContentRegionAvail().x;
    const float row_h = ImGui::GetTextLineHeight() * 1.55f;

    // ---- Page tabs (only when there is more than one page): LB [ GAME | EXTRAS AND DEBUG ] RB
    if (PageCount() > 1) {
      const int np = PageCount();
      const float th = row_h * 0.95f;
      const float kw = ImGui::CalcTextSize("LB").x + 18 * s;
      const ImVec2 sp = ImGui::GetCursorScreenPos();
      const float tabs_w = inner_w - 2 * kw - 12 * s;
      const float tw = tabs_w / float(np);
      const float fsz = base * 0.62f;
      auto key_hint = [&](const char* t, float x) {
        const ImVec2 ks = font->CalcTextSizeA(fsz, 1e9f, 0.0f, t);
        const ImVec2 a0(x + (kw - ks.x) * 0.5f - 6 * s, sp.y + (th - ks.y) * 0.5f - 2 * s);
        dl->AddRect(a0, ImVec2(a0.x + ks.x + 12 * s, a0.y + ks.y + 4 * s), gold_dim, 3 * s, 0, 1.2f * s);
        dl->AddText(font, fsz, ImVec2(a0.x + 6 * s, a0.y + 2 * s), gold, t);
      };
      key_hint("LB", sp.x);
      key_hint("RB", sp.x + inner_w - kw);
      for (int t = 0; t < np; ++t) {
        const float x0 = sp.x + kw + 6 * s + tw * float(t);
        const bool on = (t == page_);
        if (on) dl->AddRectFilled(ImVec2(x0, sp.y), ImVec2(x0 + tw, sp.y + th), C(0.93f, 0.78f, 0.42f, 0.12f));
        dl->AddLine(ImVec2(x0, sp.y + th), ImVec2(x0 + tw, sp.y + th), on ? gold : C(0.62f, 0.50f, 0.27f, 0.35f), (on ? 2.5f : 1.0f) * s);
        const char* nm = PageName(t);
        float total = 0;
        const float gap = fsz * 0.18f;
        for (const char* c = nm; *c; ++c) total += font->CalcTextSizeA(fsz, 1e9f, 0, c, c + 1).x + gap;
        total -= gap;
        float x = x0 + (tw - total) * 0.5f;
        const float y = sp.y + (th - fsz) * 0.5f;
        for (const char* c = nm; *c; ++c) {
          dl->AddText(font, fsz, ImVec2(x, y), on ? C(1.0f, 0.93f, 0.70f) : C(0.62f, 0.58f, 0.52f), c, c + 1);
          x += font->CalcTextSizeA(fsz, 1e9f, 0, c, c + 1).x + gap;
        }
        ImGui::SetCursorScreenPos(ImVec2(x0, sp.y));
        ImGui::PushID(2000 + t);
        if (ImGui::InvisibleButton("##tab", ImVec2(tw, th))) pending_page_ = t;
        ImGui::PopID();
      }
      ImGui::SetCursorScreenPos(ImVec2(sp.x, sp.y));
      ImGui::Dummy(ImVec2(inner_w, th + 4 * s));  // grows the window; the tab buttons above are placed by position
    }

    const ImVec4 dis(0.42f, 0.39f, 0.35f, 1.0f);
    for (int k = 0; k < kStart; ++k) {
      const int i = vis[k];
      const bool en = IsEnabled(i);
      if (!items_[i].section.empty()) {
        const float fsz = base * 0.62f, gap = fsz * 0.22f;
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        float x = hp.x + 20 * s;
        for (char ch : items_[i].section) {
          const char up = char(std::toupper((unsigned char)ch));
          dl->AddText(font, fsz, ImVec2(x, hp.y + 4 * s), gold_dim, &up, &up + 1);
          x += font->CalcTextSizeA(fsz, 1e9f, 0, &up, &up + 1).x + gap;
        }
        ImGui::Dummy(ImVec2(0, fsz + 10 * s));
      }
      ImGui::PushID(i);
      const bool sel = (row_ == k);
      const ImVec2 p = ImGui::GetCursorPos();
      const ImVec2 sp = ImGui::GetCursorScreenPos();
      if (sel) {
        // Green highlight bar that fades out at both ends, gold rules, diamond tips.
        const ImU32 gb = C(0.30f, 0.52f, 0.16f, 0.95f), gf = C(0.16f, 0.30f, 0.10f, 0.0f);
        const float mx = sp.x + inner_w * 0.5f;
        dl->AddRectFilledMultiColor(ImVec2(sp.x, sp.y), ImVec2(mx, sp.y + row_h), gf, gb, gb, gf);
        dl->AddRectFilledMultiColor(ImVec2(mx, sp.y), ImVec2(sp.x + inner_w, sp.y + row_h), gb, gf, gf, gb);
        const ImU32 rl = C(0.93f, 0.78f, 0.42f, 0.85f);
        dl->AddLine(ImVec2(sp.x + 14 * s, sp.y), ImVec2(sp.x + inner_w - 14 * s, sp.y), rl, 1.2f * s);
        dl->AddLine(ImVec2(sp.x + 14 * s, sp.y + row_h), ImVec2(sp.x + inner_w - 14 * s, sp.y + row_h), rl, 1.2f * s);
        Diamond(dl, ImVec2(sp.x + 6 * s, sp.y + row_h * 0.5f), row_h * 0.16f, gold);
        Diamond(dl, ImVec2(sp.x + inner_w - 6 * s, sp.y + row_h * 0.5f), row_h * 0.16f, gold);
      } else {
        dl->AddLine(ImVec2(sp.x + 10 * s, sp.y + row_h + 1 * s),
                    ImVec2(sp.x + inner_w - 10 * s, sp.y + row_h + 1 * s), C(0.62f, 0.50f, 0.27f, 0.30f), 1.0f);
      }
      if (ImGui::Selectable("##row", sel, 0, ImVec2(inner_w, row_h))) {
        row_ = k;
        if (en && items_[i].action) status_ = items_[i].action();
        if (en && items_[i].browse) br_ = std::make_unique<Browser>(items_[i].browse());
        if (en && items_[i].text && !items_[i].browse) { editing_ = true; focus_edit_ = true; }
        const int n = int(items_[i].choices.size());
        // Click the right half to change the value forward, left half selects.
        if (en && n > 0 && ImGui::GetMousePos().x > ImGui::GetWindowPos().x + panel_w * 0.5f) {
          items_[i].selected = (items_[i].selected + 1) % n;
          Changed(i);
        }
      }
      if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        row_ = k;
        const int n = int(items_[i].choices.size());
        if (en && n > 0) { items_[i].selected = (items_[i].selected + n - 1) % n; Changed(i); }
      }
      ImGui::SetCursorPos(ImVec2(p.x + 20 * s, p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(!en ? dis : sel ? txt_sel : txt, "%s", items_[i].label.c_str());
      if (items_[i].text && !items_[i].browse && editing_ && sel) {
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
                      : (items_[i].action || (items_[i].browse && !items_[i].text)) ? std::string("[ Enter ]")
                      : items_[i].choices.empty()
                          ? std::string("-")
                          : items_[i].choices[items_[i].selected].label;
      if (sel && en && !items_[i].action && !items_[i].text && !items_[i].browse) v = "<  " + v + "  >";
      const float max_vw = inner_w * 0.62f;
      while (v.size() > 8 && ImGui::CalcTextSize(v.c_str()).x > max_vw) v.erase(v.begin(), v.begin() + 4), v.replace(0, 3, "...");
      const float vw = ImGui::CalcTextSize(v.c_str()).x;
      ImGui::SetCursorPos(ImVec2(p.x + inner_w - vw - 20 * s,
                                 p.y + (row_h - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(!en ? dis : sel ? txt_sel : txt, "%s", v.c_str());
      ImGui::SetCursorPos(ImVec2(p.x, p.y + row_h + 2 * s));
      ImGui::PopID();
    }

    // ---- Buttons.
    ImGui::Dummy(ImVec2(0, 10 * s));
    Divider(dl, wp.x + 40 * s, wp.x + ww - 40 * s, ImGui::GetCursorScreenPos().y, s);
    ImGui::Dummy(ImVec2(0, 14 * s));
    const char* names[2] = {"Start Game", "Quit"};
    const float bw = inner_w * 0.50f, bh = row_h * 1.35f;
    for (int b = 0; b < 2; ++b) {
      const int idx = kStart + b;
      ImGui::PushID(1000 + b);
      const bool sel = (row_ == idx);
      const ImVec2 p = ImGui::GetCursorPos();
      const ImVec2 sp = ImGui::GetCursorScreenPos();
      const float x = sp.x + (inner_w - bw) * 0.5f;
      const float rd = bh * 0.28f;
      const ImU32 fill = (b == 0) ? (sel ? C(0.27f, 0.53f, 0.16f) : C(0.15f, 0.31f, 0.09f))
                                  : (sel ? C(0.22f, 0.17f, 0.12f) : C(0.10f, 0.08f, 0.06f));
      dl->AddRectFilled(ImVec2(x, sp.y), ImVec2(x + bw, sp.y + bh), fill, rd);
      dl->AddRect(ImVec2(x, sp.y), ImVec2(x + bw, sp.y + bh), sel ? gold : gold_dim, rd, 0, (sel ? 2.0f : 1.3f) * s);
      Diamond(dl, ImVec2(x, sp.y + bh * 0.5f), bh * 0.13f, sel ? gold : gold_dim);
      Diamond(dl, ImVec2(x + bw, sp.y + bh * 0.5f), bh * 0.13f, sel ? gold : gold_dim);
      ImGui::SetCursorScreenPos(ImVec2(x, sp.y));
      if (ImGui::InvisibleButton("##btn", ImVec2(bw, bh))) {
        row_ = idx;
        Finish(b == 0);
      }
      const float tw = ImGui::CalcTextSize(names[b]).x;
      ImGui::SetCursorPos(ImVec2(p.x + (inner_w - tw) * 0.5f,
                                 p.y + (bh - ImGui::GetTextLineHeight()) * 0.5f));
      ImGui::TextColored(sel ? txt_sel : txt, "%s", names[b]);
      ImGui::SetCursorPos(ImVec2(p.x, p.y + bh + 8 * s));
      ImGui::PopID();
      if (finished_) break;
    }

    if (!finished_) {
      ImGui::Dummy(ImVec2(0, 4 * s));
      ImGui::SetWindowFontScale(1.4f * s * fk);
      std::string help;
      if (row_ < kStart) {
        const auto& hi = items_[vis[row_]];
        help = (!IsEnabled(vis[row_]) && !hi.disabled_help.empty()) ? hi.disabled_help : hi.help;
      } else {
        help = (row_ == kStart) ? "Launch Fable II with these settings." : "Close without starting.";
      }
      ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - 40 * s);
      if (!status_.empty()) {
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "%s", status_.c_str());
        ImGui::Dummy(ImVec2(0, 4 * s));
      }
      ImGui::TextColored(ImVec4(0.72f, 0.68f, 0.60f, 1.0f), "%s", help.c_str());
      ImGui::PopTextWrapPos();
      ImGui::Dummy(ImVec2(0, 10 * s));
      Divider(dl, wp.x + 120 * s, wp.x + ww - 120 * s, ImGui::GetCursorScreenPos().y, s * 0.7f);
      ImGui::Dummy(ImVec2(0, 10 * s));
      const char* hint = "Up/Down: select      Left/Right: change      Enter / A: confirm";
      const float hw = ImGui::CalcTextSize(hint).x;
      ImGui::SetCursorPosX((ImGui::GetWindowWidth() - hw) * 0.5f);
      ImGui::TextColored(ImVec4(0.62f, 0.58f, 0.52f, 1.0f), "%s", hint);
    }

    content_h_ = ImGui::GetCursorPosY() - 28 * s - centre_off;

    // ---- Frame: double gold border with corner ornaments (drawn last, on top).
    {
      const ImVec2 ws = ImGui::GetWindowSize();
      if (ws.y > 80.0f && !br_) {
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        const ImVec2 a0 = wp, a1(wp.x + ws.x, wp.y + ws.y);
        fg->AddRect(a0, a1, C(0.93f, 0.78f, 0.42f, 0.95f), 8 * s, 0, 2.2f * s);
        const float in = 7 * s;
        fg->AddRect(ImVec2(a0.x + in, a0.y + in), ImVec2(a1.x - in, a1.y - in),
                    C(0.62f, 0.50f, 0.27f, 0.75f), 4 * s, 0, 1.0f * s);
        const ImVec2 cs[4] = {a0, ImVec2(a1.x, a0.y), a1, ImVec2(a0.x, a1.y)};
        for (const ImVec2& c : cs) Diamond(fg, c, 7.0f * s, C(0.95f, 0.80f, 0.45f));
        Diamond(fg, ImVec2((a0.x + a1.x) * 0.5f, a0.y), 6.0f * s, C(0.95f, 0.80f, 0.45f));
        Diamond(fg, ImVec2((a0.x + a1.x) * 0.5f, a1.y), 6.0f * s, C(0.95f, 0.80f, 0.45f));
      }
    }

    if (br_) {  // dim the menu behind the picker
      ImDrawList* wd = ImGui::GetWindowDrawList();
      wd->PushClipRectFullScreen();
      wd->AddRectFilled(ImVec2(0, 0), io.DisplaySize, C(0, 0, 0, 0.6f));
      wd->PopClipRect();
    }
    if (menu_font) ImGui::PopFont();
    ImGui::End();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(3);

    // Corner reminder: how to close the game once it is running.
    if (!br_) {
      const char* t = "Close the game any time: hold Start + Select for 3 seconds  (keyboard: Ctrl+Q)";
      const float fs_ = 20.0f * s;
      ImFont* f = ImGui::GetFont();
      const ImVec2 sz = f->CalcTextSizeA(fs_, 1e9f, 0.0f, t);
      const ImVec2 pos(io.DisplaySize.x - sz.x - 22 * s, io.DisplaySize.y - sz.y - 16 * s);
      ImDrawList* fg = ImGui::GetForegroundDrawList();
      fg->AddRectFilled(ImVec2(pos.x - 10 * s, pos.y - 5 * s), ImVec2(pos.x + sz.x + 10 * s, pos.y + sz.y + 5 * s), C(0, 0, 0, 0.55f), 5 * s);
      fg->AddText(f, fs_, pos, C(0.90f, 0.82f, 0.62f), t);
    }
    if (br_) br_->Draw(io, s);
  }

 private:
  struct Actions {
    bool up = false, down = false, left = false, right = false, accept = false, back = false;
    bool pg_prev = false, pg_next = false;
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
    a.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false);
    const bool shift = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    a.pg_prev = ImGui::IsKeyPressed(ImGuiKey_PageUp, false) || (shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
    a.pg_next = ImGui::IsKeyPressed(ImGuiKey_PageDown, false) || (!shift && ImGui::IsKeyPressed(ImGuiKey_Tab, false));
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
    const bool bk = btn(SDL_GAMEPAD_BUTTON_EAST);
    if (bk && !back_held_) a.back = true;
    back_held_ = bk;
    const bool lb = btn(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), rb = btn(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    if (lb && !lb_held_) a.pg_prev = true;
    if (rb && !rb_held_) a.pg_next = true;
    lb_held_ = lb;
    rb_held_ = rb;
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

  void HandleBrowser(Browser::Result r) {
    if (r == Browser::Result::kOpen && br_) r = br_->TakeClick();
    if (r == Browser::Result::kPicked) {
      const auto pick = br_->request().on_pick;
      const auto path = br_->picked();
      br_.reset();
      if (pick) status_ = pick(path);
    } else if (r == Browser::Result::kCancelled) {
      br_.reset();
    }
  }

  bool IsEnabled(int i) const { return !items_[i].enabled || items_[i].enabled(items_); }

  int PageCount() const {
    int n = 1;
    for (const auto& it : items_) n = std::max(n, it.page + 1);
    return n;
  }

  static const char* PageName(int p) {
    static const char* names[] = {"GAME", "EXTRAS AND DEBUG"};
    return (p >= 0 && p < 2) ? names[p] : "MORE";
  }

  void SetPage(int p) {
    const int n = PageCount();
    p = ((p % n) + n) % n;
    if (p == page_) return;
    page_ = p;
    row_ = 0;
    status_.clear();
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
  int row_ = 0;   // index into the rows visible on the current page, then Start, Quit
  int page_ = 0;
  int pending_page_ = -1;  // set by a click on a tab, applied at the start of the next frame
  std::string status_;
  bool editing_ = false;
  std::unique_ptr<Browser> br_;
  bool focus_edit_ = false;
  bool finished_ = false;
  bool imgs_loaded_ = false;
  detail::Img bg_, logo_;
  bool has_layout_ = false;
  float lay_[4] = {0, 0, 1, 1};
  float content_h_ = 0.0f;

#ifdef FABLE2_LAUNCHER_HAS_SDL
  SDL_Gamepad* pad_ = nullptr;
  bool pad_init_ = false;
  double next_scan_ = 0.0;
  bool held_[4] = {false, false, false, false};
  double hold_t_[4] = {0, 0, 0, 0};
  bool back_held_ = true;
  bool lb_held_ = true, rb_held_ = true;
  bool ok_held_ = true;  // ignore a button that's already down when the menu opens
#endif
};

}  // namespace fable2::launcher

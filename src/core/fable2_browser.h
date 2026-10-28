// fable2_browser.h - a small controller-friendly folder / file picker drawn with ImGui.
// Used by the launch menu for "Save folder" and "Import saves". No typing needed:
// d-pad / stick up-down to move, A / Enter to open or choose, B / Esc to go back or cancel,
// left/right to jump a page. The mouse works too.
#pragma once

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <vector>

namespace fable2::launcher {
namespace fs = std::filesystem;

// What a menu row asks for when the player opens the picker.
struct BrowseRequest {
  enum Mode { kFolder, kFile } mode = kFolder;
  std::string title;
  std::filesystem::path start;
  std::string ext;  // kFile: only files with this extension, e.g. ".zip"
  std::function<std::string(const std::filesystem::path&)> on_pick;  // returns a status line
};

class Browser {
 public:
  enum class Result { kOpen, kPicked, kCancelled };
  struct Keys { bool up = false, down = false, left = false, right = false, accept = false, back = false; };

  explicit Browser(BrowseRequest req) : req_(std::move(req)) {
    std::error_code ec;
    cur_ = req_.start;
    if (cur_.empty() || !fs::is_directory(cur_, ec)) {
      // Walk up to the nearest folder that exists; fall back to the places list.
      fs::path p = cur_;
      while (!p.empty() && p.has_parent_path() && p != p.parent_path() && !fs::is_directory(p, ec)) p = p.parent_path();
      cur_ = (!p.empty() && fs::is_directory(p, ec)) ? p : fs::path();
    }
    Rebuild();
  }

  const BrowseRequest& request() const { return req_; }
  const fs::path& picked() const { return picked_; }

  Result Update(const Keys& k) {
    const int n = int(ents_.size());
    if (n == 0) { if (k.back) return Result::kCancelled; return Result::kOpen; }
    if (k.up) Move(-1);
    if (k.down) Move(+1);
    if (k.left) Move(-8);
    if (k.right) Move(+8);
    if (k.back) return GoUpOrCancel();
    if (k.accept) return Activate(sel_);
    return Result::kOpen;
  }

  void Draw(ImGuiIO& io, float s) {
    ImGui::GetBackgroundDrawList();  // keep the list valid even if nothing else draws
    ImGui::SetNextWindowFocus();
    const float w = std::min(io.DisplaySize.x * 0.92f, 1100.0f * s);
    const float h = std::min(io.DisplaySize.y * 0.86f, 820.0f * s);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * s, 20 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.065f, 0.052f, 0.040f, 0.99f));
    ImGui::Begin("##fable2_browser", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoMove);
    ImGui::SetWindowFontScale(1.6f * s);
    ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.45f, 1.0f), "%s", req_.title.c_str());
    ImGui::SetWindowFontScale(1.2f * s);
    std::string path = cur_.empty() ? std::string("Places") : cur_.string();
    const float maxw = ImGui::GetContentRegionAvail().x;
    if (ImGui::CalcTextSize(path.c_str()).x > maxw) {
      while (path.size() > 8 && ImGui::CalcTextSize(("..." + path).c_str()).x > maxw) path.erase(path.begin());
      path = "..." + path;
    }
    ImGui::TextColored(ImVec4(0.72f, 0.66f, 0.55f, 1.0f), "%s", path.c_str());
    ImGui::Dummy(ImVec2(0, 6 * s));

    ImGui::SetWindowFontScale(1.45f * s);
    const float foot = ImGui::GetTextLineHeight() * 1.2f + 14 * s;
    ImGui::BeginChild("##list", ImVec2(0, -foot), false, ImGuiWindowFlags_NoNav);
    ImGui::SetWindowFontScale(1.45f * s);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rh = ImGui::GetTextLineHeight() * 1.5f;
    for (int i = 0; i < int(ents_.size()); ++i) {
      const bool sel = (i == sel_);
      const ImVec2 p = ImGui::GetCursorScreenPos();
      const float rw = ImGui::GetContentRegionAvail().x;
      if (sel) {
        dl->AddRectFilled(p, ImVec2(p.x + rw, p.y + rh), ImGui::ColorConvertFloat4ToU32(ImVec4(0.27f, 0.53f, 0.16f, 1.0f)), rh * 0.2f);
        dl->AddRect(p, ImVec2(p.x + rw, p.y + rh), ImGui::ColorConvertFloat4ToU32(ImVec4(0.93f, 0.78f, 0.42f, 1.0f)), rh * 0.2f, 0, 1.6f * s);
      }
      ImGui::PushID(i);
      if (ImGui::Selectable("##e", false, 0, ImVec2(rw, rh))) {
        sel_ = i;
        pending_click_ = i;
      }
      ImGui::PopID();
      const ImU32 col = ImGui::ColorConvertFloat4ToU32(sel ? ImVec4(1.0f, 0.93f, 0.70f, 1.0f) : ImVec4(0.84f, 0.79f, 0.69f, 1.0f));
      const float ty = p.y + (rh - ImGui::GetTextLineHeight()) * 0.5f;
      dl->AddText(ImVec2(p.x + 14 * s, ty), col, ents_[i].label.c_str());
      if (!ents_[i].detail.empty()) {
        const float dw = ImGui::CalcTextSize(ents_[i].detail.c_str()).x;
        dl->AddText(ImVec2(p.x + rw - dw - 14 * s, ty), ImGui::ColorConvertFloat4ToU32(ImVec4(0.62f, 0.58f, 0.52f, 1.0f)), ents_[i].detail.c_str());
      }
      if (sel && scroll_to_sel_) { ImGui::SetScrollHereY(0.5f); }
    }
    scroll_to_sel_ = false;
    ImGui::EndChild();

    ImGui::SetWindowFontScale(1.15f * s);
    ImGui::TextColored(ImVec4(0.62f, 0.58f, 0.52f, 1.0f), "%s",
                       req_.mode == BrowseRequest::kFolder
                           ? "Up / Down: move     A / Enter: open or choose     B / Esc: back     Left / Right: page"
                           : "Up / Down: move     A / Enter: open or import     B / Esc: back     Left / Right: page");
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddRect(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ImGui::ColorConvertFloat4ToU32(ImVec4(0.93f, 0.78f, 0.42f, 0.95f)), 8 * s, 0, 2.0f * s);
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
  }

  // Set by Draw() when a row was clicked; the owner turns it into a result.
  Result TakeClick() {
    if (pending_click_ < 0) return Result::kOpen;
    const int i = pending_click_;
    pending_click_ = -1;
    return Activate(i);
  }

 private:

  struct Entry {
    enum Kind { kUse, kUp, kDir, kFile } kind = kDir;
    std::string label, detail;
    fs::path path;
  };

  static std::string Ago(fs::file_time_type t) {
    using namespace std::chrono;
    const auto sys = time_point_cast<system_clock::duration>(t - fs::file_time_type::clock::now() + system_clock::now());
    const std::time_t tt = system_clock::to_time_t(sys);
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", std::localtime(&tt));
    return b;
  }

  static fs::path Home() {
    const char* h = std::getenv("HOME");
    if (!h || !*h) h = std::getenv("USERPROFILE");
    return fs::path(h ? h : ".");
  }

  void AddPlaces() {
    std::error_code ec;
    auto add = [&](const std::string& label, const fs::path& p) {
      if (p.empty() || !fs::is_directory(p, ec)) return;
      for (auto& e : ents_) if (e.path == p) return;
      Entry e; e.kind = Entry::kDir; e.label = label; e.path = p; ents_.push_back(e);
    };
    add("Home", Home());
    add("Fable2-Saves (default)", Home() / "Fable2-Saves");
    add("Downloads", Home() / "Downloads");
    add("Documents", Home() / "Documents");
    add("Desktop", Home() / "Desktop");
#ifdef _WIN32
    for (char d = 'C'; d <= 'Z'; ++d) { std::string r = std::string(1, d) + ":\\"; add("Drive " + r, fs::path(r)); }
#else
    const char* user = std::getenv("USER");
    for (const char* base : {"/run/media", "/media"}) {
      std::vector<fs::path> roots;
      if (user) roots.push_back(fs::path(base) / user);
      roots.push_back(fs::path(base));
      for (const auto& r : roots) {
        for (auto it = fs::directory_iterator(r, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec))
          if (it->is_directory(ec)) add("Drive: " + it->path().filename().string(), it->path());
        ec.clear();
      }
    }
    add("/mnt", "/mnt");
    add("Computer (/)", "/");
#endif
  }

  void Rebuild() {
    ents_.clear();
    std::error_code ec;
    if (cur_.empty()) {
      AddPlaces();
    } else {
      if (req_.mode == BrowseRequest::kFolder) {
        Entry u; u.kind = Entry::kUse; u.label = ">> Use this folder"; u.path = cur_; ents_.push_back(u);
      }
      Entry up; up.kind = Entry::kUp; up.label = ".. (up)"; up.path = cur_.parent_path(); ents_.push_back(up);
      std::vector<Entry> dirs, files;
      for (auto it = fs::directory_iterator(cur_, fs::directory_options::skip_permission_denied, ec);
           !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        if (it->is_directory(e2)) {
          Entry d; d.kind = Entry::kDir; d.label = name + "/"; d.path = it->path(); dirs.push_back(d);
        } else if (req_.mode == BrowseRequest::kFile && it->path().extension() == req_.ext && it->is_regular_file(e2)) {
          Entry f; f.kind = Entry::kFile; f.label = name; f.path = it->path();
          auto t = fs::last_write_time(it->path(), e2);
          f.detail = e2 ? "" : Ago(t);
          files.push_back(f);
        }
      }
      auto by_name = [](const Entry& a, const Entry& b) { return a.label < b.label; };
      std::sort(dirs.begin(), dirs.end(), by_name);
      std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) { return a.detail > b.detail; });  // newest first
      ents_.insert(ents_.end(), dirs.begin(), dirs.end());
      ents_.insert(ents_.end(), files.begin(), files.end());
      if (req_.mode == BrowseRequest::kFile && files.empty()) {
        // Keep the list useful: nothing to import here.
        Entry n; n.kind = Entry::kUp; n.label = "(no " + req_.ext + " files in this folder)"; n.path = cur_; n.detail = "";
        n.kind = Entry::kDir; n.path = cur_;  // selecting it just reloads
        ents_.push_back(n);
      }
    }
    // Start on the first real choice, or on the folder we just came from.
    sel_ = 0;
    if (!focus_name_.empty()) {
      for (int i = 0; i < int(ents_.size()); ++i)
        if (ents_[i].path == focus_name_) { sel_ = i; break; }
      focus_name_.clear();
    } else if (req_.mode == BrowseRequest::kFile && ents_.size() > 1) {
      for (int i = 0; i < int(ents_.size()); ++i) if (ents_[i].kind == Entry::kFile) { sel_ = i; break; }
    }
    scroll_to_sel_ = true;
  }

  void Move(int d) {
    const int n = int(ents_.size());
    if (n == 0) return;
    if (std::abs(d) == 1) sel_ = (sel_ + d + n) % n;
    else sel_ = std::clamp(sel_ + d, 0, n - 1);
    scroll_to_sel_ = true;
  }

  Result GoUpOrCancel() {
    if (cur_.empty()) return Result::kCancelled;
    const fs::path parent = cur_.parent_path();
    focus_name_ = cur_;
    if (parent.empty() || parent == cur_) cur_.clear(); else cur_ = parent;
    Rebuild();
    return Result::kOpen;
  }

  Result Activate(int i) {
    if (i < 0 || i >= int(ents_.size())) return Result::kOpen;
    const Entry e = ents_[i];
    switch (e.kind) {
      case Entry::kUse:
      case Entry::kFile: picked_ = e.path; return Result::kPicked;
      case Entry::kUp: return GoUpOrCancel();
      case Entry::kDir: cur_ = e.path; Rebuild(); return Result::kOpen;
    }
    return Result::kOpen;
  }

  BrowseRequest req_;
  fs::path cur_, picked_, focus_name_;
  std::vector<Entry> ents_;
  int sel_ = 0, pending_click_ = -1;
  bool scroll_to_sel_ = true;
};

}  // namespace fable2::launcher

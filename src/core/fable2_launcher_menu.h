// fable2_launcher_menu.h - items, saved choices and cvar plumbing for the
// pre-launch menu (fable2_launcher_dialog.h). Used by Fable2App::OnFinalizePaths.
//
//  * Initial selections: fable2_launcher.conf next to the exe, else the cvar's
//    current value (so --flags / config files show through), else a default.
//  * Start: every choice is applied with SetFlagByName and saved to the conf.
//  * Skip the menu with --no-launcher, or FABLE2_NO_LAUNCHER=1.
#pragma once

#include <rex/cvar.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "fable2_config.h"
#include "fable2_launcher_dialog.h"
#include "fable2_saves.h"

namespace fable2::launcher_menu {
namespace detail {

inline std::string Norm(std::string v) {
  std::transform(v.begin(), v.end(), v.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  if (v == "1") return "true";
  if (v == "0") return "false";
  return v;
}

inline std::map<std::string, std::string> LoadSaved(const std::filesystem::path& p) {
  std::map<std::string, std::string> m;
  std::ifstream f(p);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    const auto eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
    while (!v.empty() && (v.back() == '\r' || v.back() == ' ')) v.pop_back();
    m[k] = v;
  }
  return m;
}

}  // namespace detail

inline std::filesystem::path ExeDir() {
  std::error_code ec;
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
  if (n) return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
  auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) return p.parent_path();
#endif
  return std::filesystem::current_path(ec);
}

// Tells wrappers (play_fable2.sh / fable2-launch) that the menu is done, so the
// "pin to 2 cores for the first 25 s" startup workaround starts its clock now.
inline void SignalGo() {
#ifndef _WIN32
  const char* dir = std::getenv("XDG_RUNTIME_DIR");
  const std::string d = (dir && *dir) ? dir : "/tmp";
  std::ofstream(d + "/fable2-go." + std::to_string(::getpid())) << "go\n";
#endif
}

// True when the menu should be shown.
inline bool Enabled() {
  if (const char* e = std::getenv("FABLE2_NO_LAUNCHER"); e && *e && *e != '0') return false;
  return detail::Norm(rex::cvar::GetFlagByName("launcher")) != "false";
}

// 0 = off, 1 = simple, 2 = detailed. Read by Fable2App after Apply().
inline int g_fps_mode = 0;
inline int FpsMode() { return g_fps_mode; }

inline launcher::Item* Find(std::vector<launcher::Item>& v, const std::string& key) {
  for (auto& it : v)
    if (it.key == key) return &it;
  return nullptr;
}
inline void Pick(std::vector<launcher::Item>& v, const std::string& key, const std::string& value) {
  if (auto* it = Find(v, key))
    for (size_t i = 0; i < it->choices.size(); ++i)
      if (it->choices[i].value == value) it->selected = int(i);
}

// Frame rate -> cvars. 30 = the original lock; 60/120 = host limiter with the
// guest's 30 fps vblank lock removed; native = no limiter (VSync caps it at the
// display's refresh rate).
inline void ApplyFrameRate(const std::string& v) {
  const bool locked = (v == "30");
  rex::cvar::SetFlagByName("guest_vblank_pacing", locked ? "true" : "false");
  rex::cvar::SetFlagByName("frame_limit", (v == "native") ? "0" : v);
}

inline std::vector<launcher::Item> BuildItems(const std::filesystem::path& save_root = {}) {
  using launcher::Item;
  const auto saved = detail::LoadSaved(ExeDir() / "fable2_launcher.conf");
  auto cur = [](const char* k) { return detail::Norm(rex::cvar::GetFlagByName(k)); };

  std::vector<Item> items;
  {
    Item it{"preset", "Preset",
            "Original (Xbox 360) matches the console release. Enhanced is a good starting point for a modern PC. Changing any row below switches this back to Custom.",
            {{"Custom", "custom"}, {"Original (Xbox 360)", "original"}, {"Enhanced", "enhanced"}}, 0};
    it.on_change = [](std::vector<Item>& v) {
      auto* p = Find(v, "preset");
      if (!p) return;
      const std::string& s = p->choices[p->selected].value;
      if (s == "original") {
        Pick(v, "resolution_scale", "1"); Pick(v, "anisotropic_override", "0");
        Pick(v, "frame_rate", "30"); Pick(v, "motion_blur", "on"); Pick(v, "vsync", "true");
      } else if (s == "enhanced") {
        Pick(v, "resolution_scale", "2"); Pick(v, "anisotropic_override", "5");
        Pick(v, "frame_rate", "30"); Pick(v, "motion_blur", "off"); Pick(v, "vsync", "true");
      }
    };
    items.push_back(std::move(it));
  }
  items.push_back({"resolution_scale", "Render scale",
       "Internal render resolution. Higher is sharper but much heavier on the GPU.",
       {{"720p (1280x720)", "1"}, {"1440p (2560x1440)", "2"}, {"4K (3840x2160)", "3"}, {"5K (5120x2880) - very heavy", "4"}},
       0});
  items.push_back({"anisotropic_override", "Anisotropic filtering",
       "Keeps textures sharp at oblique angles (floors, roads). Costs very little on modern GPUs.",
       {{"Off", "0"}, {"2x", "2"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}},
       2});
  {
    Item it{"frame_rate", "Frame rate",
            "30 is the original lock. 60 / 120 / Display refresh remove it (experimental: game speed is still being verified). Display refresh follows your monitor when VSync is on.",
            {{"30 (original)", "30"}, {"60", "60"}, {"120", "120"}, {"Display refresh", "native"}}, 0};
    it.apply = ApplyFrameRate;
    items.push_back(std::move(it));
  }
  {
    Item it{"motion_blur", "Motion blur",
            "The game's full-screen camera blur. Off removes it entirely.",
            {{"On (original)", "on"}, {"Off", "off"}}, 0};
    it.apply = [](const std::string& v) { fable2::config::SetDisableMotionBlur(v == "off"); };
    items.push_back(std::move(it));
  }
  {
    Item it{"unlock_website", "Website bonus items",
            "The bonus items from the original Fable II website promotions. On gives them to every character.",
            {{"On", "on"}, {"Off", "off"}}, 0};
    it.apply = [](const std::string& v) { fable2::config::SetUnlockWebsite(v == "on"); };
    items.push_back(std::move(it));
  }
  {
    Item it{"unlock_ce", "Collector's Edition items",
            "The extra items from the Collector's Edition. On gives them to every character.",
            {{"On", "on"}, {"Off", "off"}}, 0};
    it.apply = [](const std::string& v) { fable2::config::SetUnlockCe(v == "on"); };
    items.push_back(std::move(it));
  }
  {
    Item it{"fps_counter", "FPS counter",
            "Shows the game's frame rate (and the display's) in the corner of the screen. Detailed adds the frame time in milliseconds.",
            {{"Off", "off"}, {"On", "on"}, {"Detailed", "detailed"}}, 0};
    it.apply = [](const std::string& v) { g_fps_mode = (v == "on") ? 1 : (v == "detailed") ? 2 : 0; };
    items.push_back(std::move(it));
  }
  items.push_back({"fullscreen", "Fullscreen", "Play fullscreen or in a window.",
       {{"On", "true"}, {"Off", "false"}}, 1});
  items.push_back({"vsync", "VSync",
       "Tear-free display sync. Turn off to uncap the frame rate (may tear).",
       {{"On", "true"}, {"Off", "false"}}, 0});

  if (!save_root.empty()) {
    auto dir = std::make_shared<std::string>();
    if (auto s = saved.find("saves_dir"); s != saved.end()) *dir = s->second;
    Item sd{"saves_dir", "Save folder",
            "Where exported saves go and where Import looks first. Press A / Enter to browse for a folder (no typing). Default: ~/Fable2-Saves.",
            {}, 0};
    sd.text = dir;
    sd.browse = [dir]() {
      launcher::BrowseRequest r;
      r.mode = launcher::BrowseRequest::kFolder;
      r.title = "Choose the save folder";
      r.start = saves::TransferDir(*dir);
      r.on_pick = [dir](const std::filesystem::path& p) { *dir = p.string(); return "Save folder set to " + p.string(); };
      return r;
    };
    Item ex{"", "Export saves", "Zips your saves into the save folder above, ready to copy to another device.",
            {}, 0, [save_root, dir] { return saves::DoExport(save_root, *dir); }};
    Item im{"", "Import saves",
            "Pick a saves .zip to load (your current saves are backed up first). Browse to a USB stick or another folder if needed.",
            {}, 0};
    im.browse = [save_root, dir]() {
      launcher::BrowseRequest r;
      r.mode = launcher::BrowseRequest::kFile;
      r.title = "Choose a saves .zip to import";
      r.ext = ".zip";
      r.start = saves::TransferDir(*dir);
      r.on_pick = [save_root, dir](const std::filesystem::path& p) { return saves::DoImportFile(save_root, p, *dir); };
      return r;
    };
    items.push_back(std::move(sd));
    items.push_back(std::move(ex));
    items.push_back(std::move(im));
  }

  // Initial selections: saved choice, else current state.
  for (auto& it : items) {
    if (it.choices.empty()) continue;
    std::string want;
    if (auto s = saved.find(it.key); s != saved.end()) {
      want = detail::Norm(s->second);
    } else if (it.key == "frame_rate") {
      const std::string lim = cur("frame_limit");
      want = (cur("guest_vblank_pacing") != "false") ? "30" : (lim == "60" || lim == "120") ? lim : "native";
    } else if (it.key == "motion_blur") {
      want = fable2::config::Get().disable_motion_blur ? "off" : "on";
    } else if (it.key == "unlock_website") {
      want = fable2::config::Get().unlock_website ? "on" : "off";
    } else if (it.key == "unlock_ce") {
      want = fable2::config::Get().unlock_ce ? "on" : "off";
    } else if (it.key == "preset") {
      want = "custom";
    } else if (it.key == "fps_counter") {
      want = "off";
    } else {
      want = cur(it.key.c_str());
    }
    for (size_t i = 0; i < it.choices.size(); ++i)
      if (detail::Norm(it.choices[i].value) == want) { it.selected = int(i); break; }
  }
  return items;
}

// Applies and saves the chosen values.
inline void Apply(const std::vector<launcher::Item>& items) {
  std::ofstream out(ExeDir() / "fable2_launcher.conf", std::ios::trunc);
  out << "# Written by the Fable II launch menu. Delete this file to reset it.\n";
  for (const auto& it : items) {
    if (it.text) { out << it.key << '=' << *it.text << '\n'; continue; }
    if (it.choices.empty() || it.key.empty()) continue;
    const std::string& v = it.choices[it.selected].value;
    if (it.key == "preset") {  // only a label in the menu, not a cvar
      out << it.key << '=' << v << '\n';
      continue;
    }
    if (it.apply) {
      it.apply(v);
      REXSYS_INFO("[launcher] {} = '{}'", it.key, v);
    } else if (!rex::cvar::SetFlagByName(it.key, v)) {
      REXSYS_WARN("[launcher] cvar {} rejected value '{}'", it.key, v);
    } else {
      REXSYS_INFO("[launcher] {} = '{}'", it.key, v);
    }
    out << it.key << '=' << v << '\n';
  }
}

}  // namespace fable2::launcher_menu

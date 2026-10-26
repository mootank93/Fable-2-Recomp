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
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include "fable2_launcher_dialog.h"

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

inline std::vector<launcher::Item> BuildItems() {
  using launcher::Item;
  std::vector<Item> items = {
      {"resolution_scale", "Render scale",
       "Internal render resolution multiplier. Higher is sharper but much heavier on the GPU.",
       {{"720p (1280x720)", "1"}, {"1440p (2560x1440)", "2"}, {"4K (3840x2160)", "3"}, {"5K (5120x2880) - very heavy", "4"}},
       0},
      {"anisotropic_override", "Anisotropic filtering",
       "Keeps textures sharp at oblique angles (floors, roads). Costs very little on modern GPUs.",
       {{"Off", "0"}, {"2x", "2"}, {"4x", "3"}, {"8x", "4"}, {"16x", "5"}},
       2},
      {"fullscreen", "Fullscreen", "Play fullscreen or in a window.",
       {{"On", "true"}, {"Off", "false"}}, 1},
      {"vsync", "VSync",
       "Syncs to the display refresh rate. Turn off to uncap the frame rate (may tear).",
       {{"On", "true"}, {"Off", "false"}}, 0},
  };
  const auto saved = detail::LoadSaved(ExeDir() / "fable2_launcher.conf");
  for (auto& it : items) {
    std::string want;
    if (auto s = saved.find(it.key); s != saved.end()) {
      want = detail::Norm(s->second);
    } else {
      want = detail::Norm(rex::cvar::GetFlagByName(it.key));
    }
    for (size_t i = 0; i < it.choices.size(); ++i) {
      if (detail::Norm(it.choices[i].value) == want) {
        it.selected = int(i);
        break;
      }
    }
  }
  return items;
}

// Applies and saves the chosen values. Returns the value chosen for `key`.
inline void Apply(const std::vector<launcher::Item>& items) {
  std::ofstream out(ExeDir() / "fable2_launcher.conf", std::ios::trunc);
  out << "# Written by the Fable II launch menu. Delete this file to reset it.\n";
  for (const auto& it : items) {
    if (it.choices.empty()) continue;
    const std::string& v = it.choices[it.selected].value;
    if (!rex::cvar::SetFlagByName(it.key, v)) {
      REXSYS_WARN("[launcher] cvar {} rejected value '{}'", it.key, v);
    } else {
      REXSYS_INFO("[launcher] {} = '{}'", it.key, v);
    }
    out << it.key << '=' << v << '\n';
  }
}

}  // namespace fable2::launcher_menu

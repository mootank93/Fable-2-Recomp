// fable2_debug.h - "Debug mode" for the launch menu.
//
//  * Debug mode (launch menu row): shows a live panel in the corner (game and
//    display frame rate, a frame-time graph, CPU use, memory, thread count) and
//    switches on extra graphics logging (GPU debug markers, and the Vulkan
//    validation layer when it is installed).
//  * Export debug report (launch menu row): writes one text file with the
//    system, graphics driver, settings, a summary of the last game run and the
//    end of its log, so a bug report is one file instead of pasted terminal
//    output. The home folder is shown as "~".
#pragma once

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>

#include <imgui.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <unistd.h>
extern char** environ;
#endif

#include "fable2_saves.h"  // Timestamp()

namespace fable2::debug {
namespace fs = std::filesystem;

inline bool g_enabled = false;
inline bool Enabled() { return g_enabled; }

namespace detail {

inline std::string ReadAll(const fs::path& p, size_t max_bytes = 64u * 1024 * 1024) {
  // Streams the file instead of seeking: /proc files report no size, and seeking in them
  // can return -1. Keeps only the last max_bytes.
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  std::string s;
  char buf[8192];
  while (f.read(buf, sizeof buf) || f.gcount() > 0) {
    s.append(buf, size_t(f.gcount()));
    if (s.size() > max_bytes * 2) s.erase(0, s.size() - max_bytes);
  }
  if (s.size() > max_bytes) s.erase(0, s.size() - max_bytes);
  return s;
}

inline std::vector<std::string> Lines(const std::string& text) {
  std::vector<std::string> v;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) {
    if (!l.empty() && l.back() == '\r') l.pop_back();
    v.push_back(std::move(l));
  }
  return v;
}

inline std::string Trim(std::string s) {
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n')) s.pop_back();
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
  return s.substr(i);
}

// Value after "key" + ':' in a /proc style file ("model name : X"), else "".
inline std::string ProcValue(const std::string& text, const std::string& key) {
  for (const auto& l : Lines(text)) {
    if (l.rfind(key, 0) != 0) continue;
    const auto c = l.find(':');
    if (c != std::string::npos) return Trim(l.substr(c + 1));
  }
  return {};
}

inline std::string Env(const char* k) {
  const char* v = std::getenv(k);
  return (v && *v) ? v : "";
}

// Hides the user's home folder in anything we write.
inline std::string Redact(std::string s) {
  const std::string home = Env("HOME");
  if (home.size() > 1) {
    for (size_t pos = 0; (pos = s.find(home, pos)) != std::string::npos;) {
      s.replace(pos, home.size(), "~");
      pos += 1;
    }
  }
  return s;
}

inline bool Contains(const std::string& s, const char* needle) {
  return s.find(needle) != std::string::npos;
}

inline bool ValidationLayerInstalled() {
#if defined(__linux__)
  static const char* const kPaths[] = {
      "/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
      "/usr/share/vulkan/implicit_layer.d/VkLayer_khronos_validation.json",
      "/usr/local/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json",
      "/etc/vulkan/explicit_layer.d/VkLayer_khronos_validation.json"};
  std::error_code ec;
  for (const char* p : kPaths)
    if (fs::exists(p, ec)) return true;
#endif
  return false;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// System summary (key: value lines).
// ---------------------------------------------------------------------------
inline std::vector<std::string> SystemSummary() {
  std::vector<std::string> out;
  auto add = [&](const std::string& k, const std::string& v) {
    out.push_back(k + ": " + (v.empty() ? "unknown" : v));
  };
#if defined(__aarch64__)
  add("Game build", "native aarch64 (ARM64)");
#elif defined(__x86_64__) || defined(_M_X64)
  add("Game build", "x86-64");
#else
  add("Game build", "other");
#endif
#if defined(__linux__)
  {
    std::string pretty;
    for (const auto& l : detail::Lines(detail::ReadAll("/etc/os-release", 1 << 16)))
      if (l.rfind("PRETTY_NAME=", 0) == 0) {
        pretty = l.substr(12);
        pretty.erase(std::remove(pretty.begin(), pretty.end(), '"'), pretty.end());
      }
    add("OS", pretty);
    std::istringstream kv(detail::ReadAll("/proc/version", 4096));
    std::string a, b, c;
    kv >> a >> b >> c;
    add("Kernel", c);
    const std::string cpuinfo = detail::ReadAll("/proc/cpuinfo", 1 << 20);
    std::string cpu = detail::ProcValue(cpuinfo, "model name");
    if (cpu.empty()) cpu = detail::ProcValue(cpuinfo, "Hardware");
    if (cpu.empty()) cpu = detail::ProcValue(cpuinfo, "CPU part");
    add("CPU", cpu);
    add("Logical cores", std::to_string(std::thread::hardware_concurrency()));
    const std::string mem = detail::ProcValue(detail::ReadAll("/proc/meminfo", 1 << 16), "MemTotal");
    if (!mem.empty()) {
      const double kb = std::atof(mem.c_str());
      char b2[32];
      std::snprintf(b2, sizeof b2, "%.1f GB", kb / 1024.0 / 1024.0);
      add("Memory", b2);
    }
  }
  add("Session", detail::Env("XDG_SESSION_TYPE"));
  add("Desktop", detail::Env("XDG_CURRENT_DESKTOP"));
  add("Wayland display", detail::Env("WAYLAND_DISPLAY").empty() ? "no" : "yes");
  add("Under gamescope", detail::Env("GAMESCOPE_WAYLAND_DISPLAY").empty() ? "no" : "yes");
  add("Vulkan validation layer installed", detail::ValidationLayerInstalled() ? "yes" : "no");
  // Environment switches that change behaviour (REX_*, FABLE2_*, TU_DEBUG, SDL_*, MESA_*).
  std::vector<std::string> sw;
  for (char** e = environ; e && *e; ++e) {
    const std::string s = *e;
    if (s.rfind("REX_", 0) == 0 || s.rfind("FABLE2_", 0) == 0 || s.rfind("TU_DEBUG", 0) == 0 ||
        s.rfind("SDL_", 0) == 0 || s.rfind("MESA_", 0) == 0 || s.rfind("VK_", 0) == 0)
      sw.push_back(detail::Redact(s));
  }
  std::sort(sw.begin(), sw.end());
  for (const auto& s : sw) out.push_back("env " + s);
#else
  add("OS", "not Linux (no further details collected)");
#endif
  return out;
}

// Called when Debug mode is switched on in the launch menu (before the
// graphics system exists, so init-time switches still take effect).
inline void Activate() {
  g_enabled = true;
  const bool markers = rex::cvar::SetFlagByName("gpu_debug_markers", "true");
  bool validation = false;
  if (detail::ValidationLayerInstalled())
    validation = rex::cvar::SetFlagByName("vulkan_validation_enabled", "true");
  REXSYS_INFO("[debug] Debug mode on (gpu_debug_markers={}, vulkan_validation={})", markers,
              validation);
  try {
    for (const auto& l : SystemSummary()) REXSYS_INFO("[debug] {}", detail::Redact(l));
  } catch (...) {
    REXSYS_WARN("[debug] could not collect the system summary");
  }
}

// ---------------------------------------------------------------------------
// Debug report.
// ---------------------------------------------------------------------------
namespace detail {

constexpr const char* kRunMarker = "[native-build] configuration=";

inline bool IsHeartbeat(const std::string& l) {
  return l.find("WAIT_REG_MEM long-waits") != std::string::npos;
}

// Finds the log of the previous run (the current run only has its start-up
// lines). Returns the lines of that run and a short description of the source.
inline std::vector<std::string> PreviousRunLines(std::string* source) {
  std::vector<std::string> run;
  fs::path file;
  std::error_code ec;
  bool per_run_files = false;
  // The log the game is writing right now: FABLE2_LOG, else the --log_file option
  // (linux/play.sh and the installed launcher pass it), else the newest file in logs/.
  std::string configured = Env("FABLE2_LOG");
  if (configured.empty()) {
    try {
      configured = rex::cvar::GetFlagByName("log_file");
    } catch (...) {
    }
  }
  if (!configured.empty() && fs::exists(configured, ec)) {
    file = configured;
  } else {
    const fs::path logs = [] {
      std::error_code e;
#if defined(__linux__)
      auto p = fs::read_symlink("/proc/self/exe", e);
      if (!e) return p.parent_path() / "logs";
#endif
      return fs::current_path(e) / "logs";
    }();
    std::vector<std::pair<fs::file_time_type, fs::path>> found;
    for (auto& e : fs::directory_iterator(logs, ec)) {
      if (e.path().extension() == ".log") found.emplace_back(fs::last_write_time(e.path(), ec), e.path());
    }
    std::sort(found.begin(), found.end(), [](auto& a, auto& b) { return a.first > b.first; });
    if (found.empty()) {
      *source = "no log file found";
      return run;
    }
    per_run_files = true;
    file = found[found.size() >= 2 ? 1 : 0].second;  // second newest = previous run
  }
  const auto lines = Lines(ReadAll(file));
  std::vector<size_t> starts;
  for (size_t i = 0; i < lines.size(); ++i)
    if (lines[i].find(kRunMarker) != std::string::npos) starts.push_back(i);
  size_t from = 0, to = lines.size();
  size_t which = starts.empty() ? 1 : starts.size(), total = which;
  if (!starts.empty()) {
    size_t pick = starts.size() >= 2 && !per_run_files ? starts.size() - 2 : starts.size() - 1;
    from = starts[pick];
    to = (pick + 1 < starts.size()) ? starts[pick + 1] : lines.size();
    which = pick + 1;
  }
  run.assign(lines.begin() + std::ptrdiff_t(from), lines.begin() + std::ptrdiff_t(to));
  *source = Redact(file.string()) + " (run " + std::to_string(which) + " of " +
            std::to_string(total) + ")";
  return run;
}

}  // namespace detail

inline std::string BuildReport() {
  std::ostringstream r;
  r << "Fable II: Linux Edition - debug report\n";
  r << "Made: " << fable2::saves::Timestamp() << "\n";
  r << "Note: this file lists your system, graphics driver, settings and the end of the last\n"
       "game log. Your home folder is shown as ~. Read it before you share it.\n\n";

  r << "== System ==\n";
  for (const auto& l : SystemSummary()) r << detail::Redact(l) << "\n";

  r << "\n== Launch menu choices ==\n";
  {
    std::string conf;
#if defined(__linux__)
    std::error_code ec;
    auto p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) conf = detail::ReadAll(p.parent_path() / "fable2_launcher.conf", 1 << 16);
#endif
    r << (conf.empty() ? "(none saved yet)\n" : detail::Redact(conf));
  }

  std::string source;
  const auto run = detail::PreviousRunLines(&source);
  r << "\n== Last game run ==\nSource: " << source << "\n";
  size_t warn = 0, err = 0, beats = 0;
  std::vector<std::string> keys, tail;
  for (const auto& l : run) {
    if (detail::IsHeartbeat(l)) {
      ++beats;
      continue;
    }
    if (l.find("[warning]") != std::string::npos) ++warn;
    if (l.find("[error]") != std::string::npos) ++err;
    static const char* const kKeys[] = {
        "Vulkan device '", "driverName", "driverID", "SDL video driver", "OnControllerDeviceAdded",
        "GPU plugin",      "falling back", "unsupported", "Unhandled", "crash", "Illegal",
        "[error]",         "Window closing"};
    for (const char* k : kKeys)
      if (detail::Contains(l, k)) {
        if (keys.size() < 80) keys.push_back(l);
        break;
      }
    tail.push_back(l);
  }
  r << "Lines: " << run.size() << "  warnings: " << warn << "  errors: " << err
    << "  GPU heartbeat lines left out: " << beats << "\n";
  r << "\n-- Key lines --\n";
  for (const auto& l : keys) r << detail::Redact(l.substr(0, 300)) << "\n";
  r << "\n-- End of the log (last 120 lines, heartbeat lines left out) --\n";
  const size_t from = tail.size() > 120 ? tail.size() - 120 : 0;
  for (size_t i = from; i < tail.size(); ++i) r << detail::Redact(tail[i].substr(0, 300)) << "\n";
  return r.str();
}

inline std::string ExportReport(const fs::path& out_dir) {
  std::error_code ec;
  fs::create_directories(out_dir, ec);
  const fs::path out = out_dir / ("fable2-debug-report-" + fable2::saves::Timestamp() + ".txt");
  std::ofstream f(out, std::ios::trunc);
  if (!f) return "Could not write " + out.string();
  f << BuildReport();
  f.close();
  return "Debug report saved to " + out.string();
}

// ---------------------------------------------------------------------------
// Live panel.
// ---------------------------------------------------------------------------
class DebugDialog final : public rex::ui::ImGuiDialog {
 public:
  DebugDialog(rex::ui::ImGuiDrawer* drawer, std::function<double()> guest_frame_time_us)
      : rex::ui::ImGuiDialog(drawer), guest_us_(std::move(guest_frame_time_us)) {
    std::fill(std::begin(hist_), std::end(hist_), 0.0f);
  }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (guest_us_) {
      const double us = guest_us_();
      if (us > 0.0) {
        ema_us_ = (ema_us_ <= 0.0) ? us : ema_us_ * 0.92 + us * 0.08;
        hist_[pos_] = float(us / 1000.0);
        pos_ = (pos_ + 1) % kHist;
      }
    }
    SampleProcess();
    const float s = std::max(1.0f, io.DisplaySize.y / 1080.0f);
    ImGui::SetNextWindowPos(ImVec2(10.0f * s, 10.0f * s));
    ImGui::SetNextWindowBgAlpha(0.55f);
    ImGui::Begin("##fable2_debug", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
    ImGui::SetWindowFontScale(1.0f * s);
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.35f, 1.0f), "DEBUG MODE");
    if (ema_us_ > 0.0)
      ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "Game  %.0f fps  (%.1f ms)",
                         1e6 / ema_us_, ema_us_ / 1000.0);
    else
      ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.55f, 1.0f), "Game  -- fps");
    ImGui::TextColored(ImVec4(0.75f, 0.85f, 1.0f, 1.0f), "Display  %.0f fps", io.Framerate);
    ImGui::PlotLines("##ft", hist_, kHist, pos_, nullptr, 0.0f, 100.0f,
                     ImVec2(230.0f * s, 38.0f * s));
    if (cpu_pct_ >= 0)
      ImGui::Text("CPU  %d%%  (100%% = one core)", cpu_pct_);
    if (rss_mb_ >= 0) ImGui::Text("Memory  %d MB    Threads  %d", rss_mb_, threads_);
    ImGui::TextDisabled("F3 engine stats   ` console   F4 settings");
    ImGui::End();
  }

 private:
  static constexpr int kHist = 120;

  void SampleProcess() {
#if defined(__linux__)
    const double now = ImGui::GetTime();
    if (now - last_sample_ < 0.5) return;
    // CPU: user + system ticks of this process (fields 14 and 15 of /proc/self/stat).
    const std::string stat = detail::ReadAll("/proc/self/stat", 8192);
    const auto rp = stat.rfind(')');
    if (rp != std::string::npos) {
      std::istringstream in(stat.substr(rp + 1));
      std::string tok;
      unsigned long long ut = 0, st = 0;
      for (int i = 0; i < 13 && (in >> tok); ++i) {
        if (i == 11) ut = std::strtoull(tok.c_str(), nullptr, 10);
        if (i == 12) st = std::strtoull(tok.c_str(), nullptr, 10);
      }
      const unsigned long long ticks = ut + st;
      const double hz = double(sysconf(_SC_CLK_TCK));
      if (last_sample_ > 0.0 && hz > 0.0 && now > last_sample_)
        cpu_pct_ = int(double(ticks - last_ticks_) / hz / (now - last_sample_) * 100.0 + 0.5);
      last_ticks_ = ticks;
    }
    std::istringstream sm(detail::ReadAll("/proc/self/statm", 256));
    long long size_pages = 0, rss_pages = 0;
    sm >> size_pages >> rss_pages;
    rss_mb_ = int(rss_pages * (long long)sysconf(_SC_PAGESIZE) / (1024 * 1024));
    threads_ = std::atoi(detail::ProcValue(detail::ReadAll("/proc/self/status", 8192), "Threads").c_str());
    last_sample_ = now;
#endif
  }

  std::function<double()> guest_us_;
  double ema_us_ = 0.0;
  float hist_[kHist];
  int pos_ = 0;
  double last_sample_ = 0.0;
  unsigned long long last_ticks_ = 0;
  int cpu_pct_ = -1, rss_mb_ = -1, threads_ = 0;
};

}  // namespace fable2::debug

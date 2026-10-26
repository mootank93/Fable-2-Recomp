// fable2_saves.h - export / import the whole save folder as a single .zip.
// Writes "stored" (uncompressed) zips, which every OS opens; the folder is
// tiny (~200 KB). Import reads zips made by this code (stored entries only)
// and refuses unsafe paths.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fable2::saves {
namespace fs = std::filesystem;

inline uint32_t Crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t x = i;
      for (int k = 0; k < 8; ++k) x = (x & 1) ? 0xEDB88320u ^ (x >> 1) : x >> 1;
      tab[i] = x;
    }
    init = true;
  }
  c = ~c;
  for (size_t i = 0; i < n; ++i) c = tab[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return ~c;
}

namespace detail {
inline void P16(std::string& s, uint16_t v) { s.push_back(char(v)); s.push_back(char(v >> 8)); }
inline void P32(std::string& s, uint32_t v) { P16(s, uint16_t(v)); P16(s, uint16_t(v >> 16)); }
inline uint16_t G16(const std::string& s, size_t o) { return uint16_t(uint8_t(s[o]) | (uint8_t(s[o + 1]) << 8)); }
inline uint32_t G32(const std::string& s, size_t o) { return G16(s, o) | (uint32_t(G16(s, o + 2)) << 16); }

inline std::string ReadAll(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(f), {});
}

inline bool SafeName(const std::string& n) {
  if (n.empty() || n[0] == '/' || n[0] == '\\') return false;
  if (n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
  size_t i = 0;
  while (i <= n.size()) {
    size_t j = n.find('/', i);
    if (j == std::string::npos) j = n.size();
    if (n.substr(i, j - i) == "..") return false;
    i = j + 1;
  }
  return true;
}
}  // namespace detail

// Returns "" on success, else an error message. `count` = files written.
inline std::string ExportZip(const fs::path& root, const fs::path& out_zip, int* count = nullptr) {
  using namespace detail;
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec)) return "No save folder found yet - play and save first.";
  std::vector<fs::path> entries;
  for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    entries.push_back(it->path());
  std::sort(entries.begin(), entries.end());

  std::string body, central;
  int files = 0, n = 0;
  for (const auto& p : entries) {
    std::string name = fs::relative(p, root, ec).generic_string();
    const bool dir = fs::is_directory(p, ec);
    std::string data;
    if (dir) name += '/'; else data = ReadAll(p);
    const uint32_t crc = Crc32(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    const uint32_t off = uint32_t(body.size());
    const uint16_t dtime = 0, ddate = (1 << 5) | 1;  // 1980-01-01 (reproducible)
    auto hdr = [&](std::string& s, bool cd) {
      P32(s, cd ? 0x02014b50 : 0x04034b50);
      if (cd) P16(s, 20);  // made by
      P16(s, 20); P16(s, 0x0800); P16(s, 0);  // need, flags(utf8), stored
      P16(s, dtime); P16(s, ddate); P32(s, crc);
      P32(s, uint32_t(data.size())); P32(s, uint32_t(data.size()));
      P16(s, uint16_t(name.size())); P16(s, 0);
      if (cd) { P16(s, 0); P16(s, 0); P16(s, 0); P32(s, dir ? 0x10 : 0); P32(s, off); }
      s += name;
    };
    hdr(body, false);
    body += data;
    hdr(central, true);
    ++n;
    if (!dir) ++files;
  }
  std::string eocd;
  P32(eocd, 0x06054b50); P16(eocd, 0); P16(eocd, 0);
  P16(eocd, uint16_t(n)); P16(eocd, uint16_t(n));
  P32(eocd, uint32_t(central.size())); P32(eocd, uint32_t(body.size())); P16(eocd, 0);

  fs::create_directories(out_zip.parent_path(), ec);
  std::ofstream f(out_zip, std::ios::binary | std::ios::trunc);
  if (!f) return "Could not write " + out_zip.string();
  f << body << central << eocd;
  if (!f) return "Write failed.";
  if (count) *count = files;
  return "";
}

// Extracts into `root` (existing files are overwritten). All names are
// validated first, so a bad zip changes nothing.
inline std::string ImportZip(const fs::path& zip, const fs::path& root, int* count = nullptr) {
  using namespace detail;
  std::error_code ec;
  if (root.empty()) return "No save folder configured.";
  const std::string z = ReadAll(zip);
  if (z.size() < 22) return "Not a valid zip.";
  size_t e = std::string::npos;
  for (size_t i = z.size() - 22 + 1; i-- > 0;) {
    if (G32(z, i) == 0x06054b50) { e = i; break; }
    if (z.size() - i > 22 + 65535) break;
  }
  if (e == std::string::npos) return "Not a valid zip.";
  const uint16_t total = G16(z, e + 10);
  size_t cd = G32(z, e + 16);
  struct Ent { std::string name; uint32_t off, size, crc; };
  std::vector<Ent> ents;
  for (uint16_t i = 0; i < total; ++i) {
    if (cd + 46 > z.size() || G32(z, cd) != 0x02014b50) return "Zip directory is damaged.";
    if (G16(z, cd + 10) != 0) return "This zip is compressed. Use a zip exported by the port (or re-zip with 'store'/no compression).";
    Ent en;
    en.crc = G32(z, cd + 16); en.size = G32(z, cd + 24);
    const uint16_t nl = G16(z, cd + 28), xl = G16(z, cd + 30), cl = G16(z, cd + 32);
    en.off = G32(z, cd + 42);
    en.name = z.substr(cd + 46, nl);
    if (!SafeName(en.name)) return "Zip contains an unsafe path; refusing to import.";
    ents.push_back(en);
    cd += 46 + nl + xl + cl;
  }
  int files = 0;
  for (const auto& en : ents) {  // verify everything before touching disk
    if (en.name.back() == '/') continue;
    if (en.off + 30 > z.size()) return "Zip is damaged.";
    const size_t ds = en.off + 30 + G16(z, en.off + 26) + G16(z, en.off + 28);
    if (ds + en.size > z.size()) return "Zip is truncated.";
    if (Crc32(reinterpret_cast<const uint8_t*>(z.data() + ds), en.size) != en.crc) return "Zip failed its integrity check.";
  }
  for (const auto& en : ents) {
    const fs::path out = root / fs::path(en.name);
    if (en.name.back() == '/') { fs::create_directories(out, ec); continue; }
    fs::create_directories(out.parent_path(), ec);
    const size_t ds = en.off + 30 + G16(z, en.off + 26) + G16(z, en.off + 28);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    f.write(z.data() + ds, std::streamsize(en.size));
    if (!f) return "Could not write " + out.string();
    ++files;
  }
  if (count) *count = files;
  return "";
}

inline std::string Timestamp() {
  std::time_t t = std::time(nullptr);
  char b[32];
  std::strftime(b, sizeof b, "%Y%m%d-%H%M%S", std::localtime(&t));
  return b;
}

inline fs::path HomeDir() {
  const char* h = std::getenv("HOME");
  if (!h || !*h) h = std::getenv("USERPROFILE");
  return fs::path(h ? h : ".");
}

inline fs::path DefaultTransferDir() {
  if (const char* e = std::getenv("FABLE2_SAVES_DIR"); e && *e) return e;
  return HomeDir() / "Fable2-Saves";
}

// Folder the zips live in: the user's chosen text (with ~ expanded), else the default.
inline fs::path TransferDir(const std::string& chosen) {
  std::string s = chosen;
  while (!s.empty() && (s.back() == ' ' || s.back() == '\n')) s.pop_back();
  while (!s.empty() && s.front() == ' ') s.erase(s.begin());
  if (s.empty()) return DefaultTransferDir();
  if (s == "~") return HomeDir();
  if (s.rfind("~/", 0) == 0 || s.rfind("~\\", 0) == 0) return HomeDir() / s.substr(2);
  return fs::path(s);
}

inline fs::path NewestZip(const fs::path& dir) {
  std::error_code ec;
  fs::path best; fs::file_time_type bt{};
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (e.path().extension() != ".zip") continue;
    if (e.path().filename().string().rfind("backup-", 0) == 0) continue;
    auto t = fs::last_write_time(e.path(), ec);
    if (best.empty() || t > bt) { best = e.path(); bt = t; }
  }
  return best;
}

// Menu actions. Return a one-line status for the menu.
inline std::string DoExport(const fs::path& root, const std::string& chosen_dir) {
  const fs::path out = TransferDir(chosen_dir) / ("fable2-saves-" + Timestamp() + ".zip");
  int n = 0;
  if (auto err = ExportZip(root, out, &n); !err.empty()) return err;
  return "Exported " + std::to_string(n) + " files to " + out.string();
}

inline std::string DoImport(const fs::path& root, const std::string& chosen_dir) {
  const fs::path dir = TransferDir(chosen_dir);
  const fs::path zip = NewestZip(dir);
  if (zip.empty()) return "Put a saves .zip in " + dir.string() + " first.";
  std::error_code ec;
  if (fs::is_directory(root, ec)) {  // keep a copy of what we're about to replace
    ExportZip(root, dir / ("backup-before-import-" + Timestamp() + ".zip"));
  }
  int n = 0;
  if (auto err = ImportZip(zip, root, &n); !err.empty()) return err;
  return "Imported " + std::to_string(n) + " files from " + zip.filename().string() +
         " (your old saves were backed up).";
}

}  // namespace fable2::saves

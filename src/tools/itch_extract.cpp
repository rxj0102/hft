// Stream a Nasdaq ITCH 5.0 BinaryFILE from stdin and keep what a replay needs.
//
//   gzip -dc 12302019.NASDAQ_ITCH50.gz |
//       itch_extract --symbols AAPL,MSFT --out day.bin [--split-dir DIR --split-bytes N]
//
// Writes to --out, in the same framing, every system event (S), stock directory
// (R) and trading action (H) message, and every message whose stock locate
// belongs to one of the symbols. Full day files are several gigabytes, so this
// makes a small file the replay tool can map. With --split-dir the whole
// stream is also cut, at frame boundaries, into files of about N bytes, so the
// decoder can be run over every message of the day without the full file on
// disk at once.
//
// Prints, for the whole stream, the number of frames per message type and how
// many frames of each type had each length. The extractor does not validate
// lengths itself; that is left to the decoder under test.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::vector<std::string> split(const std::string& s, char sep) {
  std::vector<std::string> out;
  std::string cur;
  for (char ch : s) {
    if (ch == sep) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur.push_back(ch);
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

bool read_exact(std::FILE* f, unsigned char* buf, std::size_t n) {
  return std::fread(buf, 1, n, f) == n;
}

}  // namespace

int main(int argc, char** argv) {
  std::set<std::string> symbols;
  const char* out_path = nullptr;
  std::string split_dir;
  std::uint64_t split_bytes = 0;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    if (i + 1 >= argc) {
      std::fprintf(stderr, "missing value for %s\n", k.c_str());
      return 2;
    }
    const char* v = argv[++i];
    if (k == "--symbols") {
      for (auto& s : split(v, ',')) symbols.insert(s);
    } else if (k == "--out") {
      out_path = v;
    } else if (k == "--split-dir") {
      split_dir = v;
    } else if (k == "--split-bytes") {
      split_bytes = std::strtoull(v, nullptr, 10);
    } else {
      std::fprintf(stderr, "unknown option %s\n", k.c_str());
      return 2;
    }
  }
  if (!out_path) {
    std::fprintf(stderr,
                 "usage: itch_extract --symbols A,B --out FILE [--split-dir D --split-bytes N]\n");
    return 2;
  }
  std::FILE* out = std::fopen(out_path, "wb");
  if (!out) {
    std::perror(out_path);
    return 2;
  }
  std::FILE* part = nullptr;
  int part_no = 0;
  std::uint64_t part_size = 0;

  std::set<std::uint16_t> keep;
  std::map<std::string, std::uint16_t> found;
  std::uint64_t frames = 0, kept = 0, bytes = 0;
  std::map<char, std::map<unsigned, std::uint64_t>> lengths;
  std::vector<unsigned char> buf(1 << 16);
  unsigned char hdr[2];

  while (read_exact(stdin, hdr, 2)) {
    const unsigned len = (unsigned(hdr[0]) << 8) | hdr[1];
    if (len == 0 || !read_exact(stdin, buf.data(), len)) {
      std::fprintf(stderr, "truncated or empty frame after %llu frames\n",
                   static_cast<unsigned long long>(frames));
      return 1;
    }
    ++frames;
    bytes += 2 + len;
    const char type = static_cast<char>(buf[0]);
    ++lengths[type][len];
    const std::uint16_t locate = len >= 3 ? static_cast<std::uint16_t>((buf[1] << 8) | buf[2]) : 0;

    if (type == 'R' && len >= 19) {
      std::string sym(reinterpret_cast<const char*>(buf.data() + 11), 8);
      sym.erase(sym.find_last_not_of(' ') + 1);
      if (symbols.count(sym)) {
        keep.insert(locate);
        found[sym] = locate;
      }
    }
    if (type == 'S' || type == 'R' || type == 'H' || keep.count(locate)) {
      std::fwrite(hdr, 1, 2, out);
      std::fwrite(buf.data(), 1, len, out);
      ++kept;
    }
    if (!split_dir.empty()) {
      if (!part || part_size >= split_bytes) {
        if (part) std::fclose(part);
        char name[64];
        std::snprintf(name, sizeof name, "/part%03d.bin", part_no++);
        part = std::fopen((split_dir + name).c_str(), "wb");
        if (!part) {
          std::perror("split file");
          return 2;
        }
        part_size = 0;
      }
      std::fwrite(hdr, 1, 2, part);
      std::fwrite(buf.data(), 1, len, part);
      part_size += 2 + len;
    }
  }
  if (part) std::fclose(part);
  std::fclose(out);

  std::printf("frames %llu, bytes %llu, kept %llu\n", static_cast<unsigned long long>(frames),
              static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(kept));
  for (auto& [sym, loc] : found) std::printf("symbol %s locate %u\n", sym.c_str(), loc);
  for (auto& [type, by_len] : lengths) {
    for (auto& [len, n] : by_len)
      std::printf("type %c length %u frames %llu\n", type, len, static_cast<unsigned long long>(n));
  }
  if (!split_dir.empty()) std::printf("split into %d files\n", part_no);
  return 0;
}

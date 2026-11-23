// SPDX-License-Identifier: AGPL-3.0-or-later
//
// EVG Browser in a desktop window: SDL3 for the window and the pixels,
// libcurl for the network, stb_truetype for the type, stb_image and nanosvg
// for pictures.
//
// Everything the browser IS — HTML, CSS, layout, history, scrolling, links,
// the safety policy — is the Ranger program, compiled to C++ and included
// below. This file is only what a desktop knows and the program cannot:
//
//   * the window, the mouse, the wheel and the keyboard, handed to
//     `BrowserHost` (EVG's `EvgHost`) exactly as the web page hands them;
//   * the network: `BrowserApp` queues requests, worker threads fetch them
//     with libcurl, and the answers are delivered back on the main thread;
//   * the fonts: the same faces measure the text (EVGHostTextMeasurer) and
//     draw it, so a line breaks where its glyphs end;
//   * running the painter's output: the display list is painted by
//     RangerDOM's shared painter (the crate rangerdom_paint, Rust that
//     Ranger compiles to C++ here; the same code is RangerDOM's SDL3
//     playground painter), which makes triangles, textures and clips, and
//     SdlPainter below hands them to SDL_RenderGeometry.
//
// Build: native/sdl/build.sh (or `npm run sdl`).

#include "evg_browser.cpp"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <curl/curl.h>

// Type, pictures and screenshots from single-file libraries compiled in
// (third_party/README.md), so the only libraries needed are SDL3 and libcurl.
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#include "third_party/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb_image_write.h"
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"

#ifndef _WIN32
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace evgsdl {

// ---------------------------------------------------------------------------
// UTF-8
// ---------------------------------------------------------------------------

static bool nextCodepoint(const std::string& s, size_t& i, uint32_t& cp) {
  if (i >= s.size()) return false;
  unsigned char c = (unsigned char)s[i];
  int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
  if (extra < 0 || i + extra >= s.size() + (extra == 0 ? 1 : 0)) {
    cp = 0xFFFD;
    i += 1;
    return true;
  }
  cp = extra == 0 ? c : (c & (0x3F >> extra));
  for (int k = 1; k <= extra; k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
  i += extra + 1;
  return true;
}

static bool validUtf8(const std::string& s) {
  size_t i = 0, n = s.size();
  while (i < n) {
    unsigned char c = (unsigned char)s[i];
    int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
    if (extra < 0 || i + extra >= n + (extra == 0 ? 1 : 0)) return false;
    for (int k = 1; k <= extra; k++) {
      if (((unsigned char)s[i + k] & 0xC0) != 0x80) return false;
    }
    i += extra + 1;
  }
  return true;
}

// Latin-1 / Windows-1252 to UTF-8: what an older page that does not say
// otherwise is written in.
static std::string latin1ToUtf8(const std::string& s) {
  std::string out;
  out.reserve(s.size() + s.size() / 8);
  for (unsigned char c : s) {
    if (c < 0x80) {
      out.push_back((char)c);
    } else {
      out.push_back((char)(0xC0 | (c >> 6)));
      out.push_back((char)(0x80 | (c & 0x3F)));
    }
  }
  return out;
}

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// ---------------------------------------------------------------------------
// Fonts: one set of faces measures and draws
// ---------------------------------------------------------------------------

enum Face { SANS = 0, SANS_BOLD = 1, MONO = 2, MONO_BOLD = 3, FACE_COUNT = 4 };

static bool fileExists(const std::string& p) {
  FILE* f = std::fopen(p.c_str(), "rb");
  if (!f) return false;
  std::fclose(f);
  return true;
}

static std::string firstExisting(const std::vector<std::string>& paths) {
  for (auto& p : paths) if (fileExists(p)) return p;
  return "";
}

// A font file in memory, opened by stb_truetype.
struct FontFile {
  std::vector<unsigned char> data;
  stbtt_fontinfo info;
  bool ok = false;
  int ascent = 0, descent = 0, lineGap = 0;  // font units

  bool open(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
      std::fclose(f);
      return false;
    }
    data.resize((size_t)n);
    size_t got = std::fread(data.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n) return false;
    int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);  // a .ttc: its first face
    if (offset < 0 || !stbtt_InitFont(&info, data.data(), offset)) return false;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &lineGap);
    ok = true;
    return true;
  }

  // CSS sizes are em sizes: `font-size: 16px` makes the em 16 pixels.
  float scale(double px) const { return stbtt_ScaleForMappingEmToPixels(&info, (float)px); }

  bool has(uint32_t cp) const {
    int g = stbtt_FindGlyphIndex(&info, (int)cp);
    return g != 0 && !stbtt_IsGlyphEmpty(&info, g);
  }
};

// Colour emoji: the pictures are PNGs inside the font — the CBDT/CBLC tables
// (Noto Color Emoji, Linux and Android) or the sbix table (Apple Color Emoji).
// stb_truetype finds the glyph and its advance; this finds the PNG, and
// stb_image decodes it.
class ColorEmoji {
 public:
  FontFile font;
  bool ok = false;

  bool open(const std::string& path) {
    if (!font.open(path) && !openBitmapOnly(path)) return false;
    const unsigned char* d = font.data.data();
    unsigned int start = (unsigned int)font.info.fontstart;
    cblc = stbtt__find_table((stbtt_uint8*)d, start, "CBLC");
    cbdt = stbtt__find_table((stbtt_uint8*)d, start, "CBDT");
    sbix = stbtt__find_table((stbtt_uint8*)d, start, "sbix");
    ok = (cblc && cbdt) || sbix;
    return ok;
  }

  bool has(uint32_t cp) const { return ok && stbtt_FindGlyphIndex(&font.info, (int)cp) != 0; }

  // The PNG bytes of a glyph, or an empty span.
  bool png(int glyph, const unsigned char*& out, size_t& len) const {
    if (cblc && cbdt) return fromCbdt(glyph, out, len);
    if (sbix) return fromSbix(glyph, out, len);
    return false;
  }

 private:
  unsigned int cblc = 0, cbdt = 0, sbix = 0;

  // stbtt_InitFont wants outlines (glyf or CFF), and a bitmap-only face has
  // none. What this needs of stb_truetype is the character map and the
  // advances, so those are set up here the way stbtt_InitFont does.
  bool openBitmapOnly(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) {
      std::fclose(f);
      return false;
    }
    font.data.resize((size_t)n);
    size_t got = std::fread(font.data.data(), 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n) return false;
    stbtt_uint8* d = font.data.data();
    int start = stbtt_GetFontOffsetForIndex(d, 0);
    if (start < 0) return false;
    stbtt_fontinfo& info = font.info;
    std::memset(&info, 0, sizeof(info));
    info.data = d;
    info.fontstart = start;
    unsigned int cmap = stbtt__find_table(d, (stbtt_uint32)start, "cmap");
    info.head = (int)stbtt__find_table(d, (stbtt_uint32)start, "head");
    info.hhea = (int)stbtt__find_table(d, (stbtt_uint32)start, "hhea");
    info.hmtx = (int)stbtt__find_table(d, (stbtt_uint32)start, "hmtx");
    unsigned int maxp = stbtt__find_table(d, (stbtt_uint32)start, "maxp");
    if (!cmap || !info.head || !info.hhea || !info.hmtx) return false;
    info.numGlyphs = maxp ? (int)u16(maxp + 4) : 0xffff;
    unsigned int tables = u16(cmap + 2);
    for (unsigned int i = 0; i < tables; i++) {
      unsigned int rec = cmap + 4 + 8 * i;
      unsigned int platform = u16(rec), encoding = u16(rec + 2);
      if (platform == 3 && (encoding == 1 || encoding == 10)) info.index_map = (int)(cmap + u32(rec + 4));
      if (platform == 0 && !info.index_map) info.index_map = (int)(cmap + u32(rec + 4));
    }
    if (!info.index_map) return false;
    info.indexToLocFormat = (int)u16((unsigned int)info.head + 50);
    stbtt_GetFontVMetrics(&info, &font.ascent, &font.descent, &font.lineGap);
    font.ok = true;
    return true;
  }

  // big-endian reads, 0 past the end of the file
  unsigned int u32(unsigned int at) const {
    if (!inside(at, 4)) return 0;
    const unsigned char* p = font.data.data() + at;
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
  }
  unsigned int u16(unsigned int at) const {
    if (!inside(at, 2)) return 0;
    const unsigned char* p = font.data.data() + at;
    return ((unsigned int)p[0] << 8) | p[1];
  }
  bool inside(size_t at, size_t n) const { return at + n <= font.data.size(); }

  bool fromCbdt(int glyph, const unsigned char*& out, size_t& len) const {
    unsigned int numSizes = u32(cblc + 4);
    // the largest strike
    unsigned int best = 0, bestPpem = 0;
    for (unsigned int i = 0; i < numSizes; i++) {
      unsigned int rec = cblc + 8 + i * 48;
      unsigned int ppem = font.data[rec + 45];
      if (ppem >= bestPpem) { bestPpem = ppem; best = rec; }
    }
    if (!best) return false;
    unsigned int arr = cblc + u32(best);
    unsigned int count = u32(best + 8);
    for (unsigned int i = 0; i < count; i++) {
      unsigned int e = arr + i * 8;
      unsigned int first = u16(e), last = u16(e + 2);
      if (glyph < (int)first || glyph > (int)last) continue;
      unsigned int sub = arr + u32(e + 4);
      unsigned int indexFormat = u16(sub), imageFormat = u16(sub + 2);
      unsigned int dataOff = cbdt + u32(sub + 4);
      unsigned int at = 0, end = 0;
      unsigned int k = (unsigned int)glyph - first;
      if (indexFormat == 1) {
        at = dataOff + u32(sub + 8 + k * 4);
        end = dataOff + u32(sub + 8 + (k + 1) * 4);
      } else if (indexFormat == 3) {
        at = dataOff + u16(sub + 8 + k * 2);
        end = dataOff + u16(sub + 8 + (k + 1) * 2);
      } else if (indexFormat == 2) {
        unsigned int size = u32(sub + 8);
        at = dataOff + k * size;
        end = at + size;
      } else if (indexFormat == 4) {
        unsigned int n = u32(sub + 8);
        for (unsigned int j = 0; j < n; j++) {
          unsigned int pair = sub + 12 + j * 4;
          if ((int)u16(pair) == glyph) {
            at = dataOff + u16(pair + 2);
            end = dataOff + u16(pair + 6);
          }
        }
      } else if (indexFormat == 5) {
        unsigned int size = u32(sub + 8);
        unsigned int n = u32(sub + 20);
        for (unsigned int j = 0; j < n; j++) {
          if ((int)u16(sub + 24 + j * 2) == glyph) {
            at = dataOff + j * size;
            end = at + size;
          }
        }
      }
      if (!at || end <= at || !inside(at, end - at)) return false;
      // the image formats with PNG in them: 17 small metrics, 18 big, 19 none
      unsigned int head = imageFormat == 17 ? 5 : imageFormat == 18 ? 8 : imageFormat == 19 ? 0 : 1000;
      if (head == 1000) return false;
      unsigned int n = u32(at + head);
      if (!inside(at + head + 4, n)) return false;
      out = font.data.data() + at + head + 4;
      len = n;
      return true;
    }
    return false;
  }

  bool fromSbix(int glyph, const unsigned char*& out, size_t& len) const {
    unsigned int strikes = u32(sbix + 4);
    unsigned int best = 0, bestPpem = 0;
    for (unsigned int i = 0; i < strikes; i++) {
      unsigned int st = sbix + u32(sbix + 8 + i * 4);
      unsigned int ppem = u16(st);
      if (ppem >= bestPpem && ppem <= 160) { bestPpem = ppem; best = st; }
    }
    if (!best) return false;
    unsigned int at = best + u32(best + 4 + glyph * 4);
    unsigned int end = best + u32(best + 4 + (glyph + 1) * 4);
    if (end <= at + 8 || !inside(at, end - at)) return false;
    if (std::memcmp(font.data.data() + at + 4, "png ", 4) != 0) return false;
    out = font.data.data() + at + 8;
    len = end - at - 8;
    return true;
  }
};

class Fonts {
 public:
  FontFile face[FACE_COUNT];
  // faces that fill in what the main ones have no glyph for (symbols, other
  // scripts); a colour emoji face is bitmap-only and cannot be one of them
  std::vector<std::unique_ptr<FontFile>> fallbacks;
  ColorEmoji emoji;

  bool load() {
    const char* dir = std::getenv("EVG_FONT_DIR");
    std::string d = dir ? std::string(dir) + "/" : "";
    const std::vector<std::string> paths[FACE_COUNT] = {
      {d + "sans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf", "/System/Library/Fonts/Supplemental/Arial.ttf",
       "/Library/Fonts/Arial.ttf", "C:/Windows/Fonts/arial.ttf"},
      {d + "sans-bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
       "/Library/Fonts/Arial Bold.ttf", "C:/Windows/Fonts/arialbd.ttf"},
      {d + "mono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf", "/System/Library/Fonts/Supplemental/Courier New.ttf",
       "C:/Windows/Fonts/cour.ttf"},
      {d + "mono-bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf",
       "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf", "/System/Library/Fonts/Supplemental/Courier New Bold.ttf",
       "C:/Windows/Fonts/courbd.ttf"}};
    for (int f = 0; f < FACE_COUNT; f++) {
      for (auto& p : paths[f]) if (face[f].open(p)) break;
    }
    if (!face[SANS].ok) return false;
    for (int f = 1; f < FACE_COUNT; f++) {
      if (!face[f].ok) face[f] = (f == MONO_BOLD && face[MONO].ok) ? face[MONO] : face[SANS];
    }
    const std::vector<std::string> extra = {
      d + "fallback.ttf", "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoEmoji-Regular.ttf", "/usr/share/fonts/truetype/freefont/FreeSerif.ttf",
      "/System/Library/Fonts/Supplemental/Arial Unicode.ttf", "/System/Library/Fonts/Apple Symbols.ttf",
      "C:/Windows/Fonts/seguiemj.ttf", "C:/Windows/Fonts/seguisym.ttf"};
    for (auto& p : extra) {
      auto ff = std::make_unique<FontFile>();
      if (ff->open(p)) fallbacks.push_back(std::move(ff));
    }
    for (auto& p : {d + "emoji.ttf", std::string("/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf"),
                    std::string("/usr/share/fonts/noto/NotoColorEmoji.ttf"), std::string("/System/Library/Fonts/Apple Color Emoji.ttc")}) {
      if (emoji.open(p)) break;
    }
    return true;
  }

  static Face faceFor(const std::string& family, bool bold) {
    std::string f = lower(family);
    bool mono = f.find("courier") != std::string::npos || f.find("mono") != std::string::npos ||
                f.find("consolas") != std::string::npos;
    if (f.size() > 5 && f.compare(f.size() - 5, 5, "-bold") == 0) bold = true;
    if (mono) return bold ? MONO_BOLD : MONO;
    return bold ? SANS_BOLD : SANS;
  }

  // One stretch of text drawn with one font file.
  struct Run {
    const FontFile* font;
    std::vector<uint32_t> cps;
  };

  const FontFile* fontFor(Face f, uint32_t cp) const {
    const FontFile* main = &face[f];
    if (cp < 0x80) return main;
    // pictographs in colour when there is a colour face, whatever else has them
    if (cp >= 0x1F000 && emoji.has(cp)) return &emoji.font;
    if (main->has(cp)) return main;
    for (auto& fb : fallbacks) if (fb->has(cp)) return fb.get();
    if (emoji.has(cp)) return &emoji.font;
    return main;
  }

  bool isEmoji(const FontFile* f) const { return f == &emoji.font; }

  std::vector<Run> runs(const std::string& text, Face f) const {
    std::vector<Run> out;
    size_t i = 0;
    uint32_t cp = 0;
    while (nextCodepoint(text, i, cp)) {
      if (cp == 0xFE0F || cp == 0x200D) continue;  // emoji presentation marks draw nothing
      const FontFile* ff = fontFor(f, cp);
      // U+FE0F after a symbol asks for its emoji (colour) form
      size_t peek = i;
      uint32_t next = 0;
      if (nextCodepoint(text, peek, next) && next == 0xFE0F && emoji.has(cp)) ff = &emoji.font;
      if (out.empty() || out.back().font != ff) out.push_back(Run{ff, {}});
      out.back().cps.push_back(cp);
    }
    return out;
  }

  static double runWidth(const Run& r, double px) {
    float sc = r.font->scale(px);
    double w = 0.0;
    for (size_t k = 0; k < r.cps.size(); k++) {
      int adv = 0, lsb = 0;
      stbtt_GetCodepointHMetrics(&r.font->info, (int)r.cps[k], &adv, &lsb);
      w += adv * sc;
      if (k + 1 < r.cps.size()) w += stbtt_GetCodepointKernAdvance(&r.font->info, (int)r.cps[k], (int)r.cps[k + 1]) * sc;
    }
    return w;
  }

  // What EVGHostTextMeasurer asks: a run's width, or a face's ascent,
  // descent and line gap, in CSS pixels at `size`.
  double metric(int kind, const std::string& text, const std::string& family, double size, bool bold) const {
    Face f = faceFor(family, bold);
    if (kind == 0) {
      double w = 0.0;
      for (auto& r : runs(text, f)) w += runWidth(r, size);
      return w;
    }
    const FontFile& ff = face[f];
    float sc = ff.scale(size);
    if (kind == 1) return ff.ascent * sc;
    if (kind == 2) return -ff.descent * sc;
    return ff.lineGap > 0 ? ff.lineGap * sc : 0.0;
  }
};

// ---------------------------------------------------------------------------
// Network: libcurl on worker threads
// ---------------------------------------------------------------------------

struct Job {
  int id = 0;
  std::string kind, url;
};

struct Result {
  int id = 0;
  std::string kind, url;
  long status = 0;
  std::string finalUrl, contentType, body, error;
};

class Net {
 public:
  explicit Net(int workers) {
    for (int i = 0; i < workers; i++) threads.emplace_back([this] { run(); });
  }

  ~Net() {
    stopping = true;
    cv.notify_all();
    for (auto& t : threads) t.join();
  }

  void submit(Job j) {
    {
      std::lock_guard<std::mutex> lk(mu);
      jobs.push_back(std::move(j));
    }
    cv.notify_one();
  }

  bool poll(Result& out) {
    std::lock_guard<std::mutex> lk(mu);
    if (done.empty()) return false;
    out = std::move(done.front());
    done.pop_front();
    return true;
  }

  int busy() {
    std::lock_guard<std::mutex> lk(mu);
    return (int)jobs.size() + active;
  }

 private:
  std::vector<std::thread> threads;
  std::deque<Job> jobs;
  std::deque<Result> done;
  std::mutex mu;
  std::condition_variable cv;
  std::atomic<bool> stopping{false};
  int active = 0;

  static constexpr size_t MAX_BYTES = 8 * 1024 * 1024;

  struct Sink {
    std::string* body;
    bool tooBig = false;
  };

  static size_t onData(char* p, size_t size, size_t n, void* user) {
    Sink* s = (Sink*)user;
    size_t len = size * n;
    if (s->body->size() + len > MAX_BYTES) {
      s->tooBig = true;
      return 0;
    }
    s->body->append(p, len);
    return len;
  }

  static int onProgress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return ((std::atomic<bool>*)user)->load() ? 1 : 0;
  }

  void run() {
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [this] { return stopping.load() || !jobs.empty(); });
        if (stopping) return;
        job = std::move(jobs.front());
        jobs.pop_front();
        active++;
      }
      Result r = fetch(job);
      std::lock_guard<std::mutex> lk(mu);
      active--;
      done.push_back(std::move(r));
    }
  }

  Result fetch(const Job& job) {
    Result r;
    r.id = job.id;
    r.kind = job.kind;
    r.url = job.url;
    CURL* c = curl_easy_init();
    if (!c) {
      r.error = "libcurl ei käynnistynyt";
      return r;
    }
    Sink sink{&r.body};
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Accept-Language: fi,en;q=0.7");
    headers = curl_slist_append(headers, job.kind == "image" ? "Accept: image/webp,image/png,image/jpeg,image/gif,image/svg+xml"
                                                           : "Accept: text/html,text/css,*/*;q=0.5");
    curl_easy_setopt(c, CURLOPT_URL, job.url.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "EVG-Browser/0.1 (SDL3; +https://github.com/terotests/EVGBrowser)");
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 6L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    // web pages only: no file://, no ftp, not even after a redirect
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(c, CURLOPT_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
    curl_easy_setopt(c, CURLOPT_REDIR_PROTOCOLS, (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &Net::onData);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, &Net::onProgress);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &stopping);
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK || (rc == CURLE_WRITE_ERROR && sink.tooBig)) {
      curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &r.status);
      char* eff = nullptr;
      curl_easy_getinfo(c, CURLINFO_EFFECTIVE_URL, &eff);
      if (eff) r.finalUrl = eff;
      char* ct = nullptr;
      curl_easy_getinfo(c, CURLINFO_CONTENT_TYPE, &ct);
      if (ct) r.contentType = ct;
    } else {
      r.status = 0;
      r.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(c);
    return r;
  }
};

// Text as the app expects it: UTF-8, whatever the page was written in.
static std::string asUtf8(const std::string& body, const std::string& contentType) {
  std::string ct = lower(contentType);
  bool latin = ct.find("iso-8859") != std::string::npos || ct.find("windows-1252") != std::string::npos ||
               ct.find("latin1") != std::string::npos;
  if (!latin) {
    std::string head = lower(body.substr(0, 2048));
    latin = head.find("charset=iso-8859") != std::string::npos || head.find("charset=\"iso-8859") != std::string::npos ||
            head.find("charset=windows-1252") != std::string::npos;
  }
  if (latin || !validUtf8(body)) return latin1ToUtf8(body);
  return body;
}

// ---------------------------------------------------------------------------
// Pictures: decoded here, drawn by the painter
// ---------------------------------------------------------------------------

struct Image {
  bool ok = false;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // not premultiplied
};

// A picture's bytes as a texture: JPEG, PNG, GIF (first frame), BMP through
// stb_image; WebP through Ranger's WebPDecoder; SVG through nanosvg, rasterised at its
// own size (at most 2048 on a side). AVIF is not decoded or asked for.
static Image decodeImage(const std::string& bytes);

static Image decodeIco(const std::string& bytes) {
  auto u8 = [&](size_t i) -> unsigned { return i < bytes.size() ? (unsigned char)bytes[i] : 0u; };
  auto u16 = [&](size_t i) -> unsigned { return u8(i) | (u8(i + 1) << 8); };
  auto u32 = [&](size_t i) -> unsigned { return u16(i) | (u16(i + 2) << 16); };
  unsigned count = u16(4);
  int best = -1, bestScore = 1 << 30;
  for (unsigned k = 0; k < count && 6 + k * 16 + 16 <= bytes.size(); k++) {
    size_t e = 6 + k * 16;
    int size = u8(e) == 0 ? 256 : (int)u8(e);
    // nearest to 32, a bigger one before a smaller one as far off
    int score = size >= 32 ? (size - 32) * 2 : (32 - size) * 3;
    if (score < bestScore) { bestScore = score; best = (int)k; }
  }
  if (best < 0) return Image{};
  size_t e = 6 + (size_t)best * 16;
  size_t len = u32(e + 8), off = u32(e + 12);
  if (off >= bytes.size() || len == 0) return Image{};
  if (off + len > bytes.size()) len = bytes.size() - off;
  std::string data = bytes.substr(off, len);
  if (data.size() > 8 && data.compare(1, 3, "PNG") == 0) return decodeImage(data);
  if (data.size() < 40) return Image{};
  // BITMAPINFOHEADER: the height counts the AND mask too
  int32_t hh = (int32_t)(u8(off + 8) | (u8(off + 9) << 8) | (u8(off + 10) << 16) | (u8(off + 11) << 24));
  hh /= 2;
  data[8] = (char)(hh & 0xff); data[9] = (char)((hh >> 8) & 0xff);
  data[10] = (char)((hh >> 16) & 0xff); data[11] = (char)((hh >> 24) & 0xff);
  unsigned hdr = u32(off), bpp = u16(off + 14), colors = u32(off + 32);
  if (colors == 0 && bpp <= 8) colors = 1u << bpp;
  unsigned pixOff = 14 + hdr + colors * 4;
  std::string bmp = "BM";
  unsigned total = 14 + (unsigned)data.size();
  for (int i = 0; i < 4; i++) bmp.push_back((char)((total >> (8 * i)) & 0xff));
  bmp.append(4, '\0');
  for (int i = 0; i < 4; i++) bmp.push_back((char)((pixOff >> (8 * i)) & 0xff));
  bmp += data;
  return decodeImage(bmp);
}

static Image decodeImage(const std::string& bytes) {
  Image im;
  int w = 0, h = 0;
  std::vector<unsigned char> rgba;
  std::string head = lower(bytes.substr(0, 1024));
  if (head.find("<svg") != std::string::npos) {
    std::vector<char> text(bytes.begin(), bytes.end());
    text.push_back(0);
    NSVGimage* svg = nsvgParse(text.data(), "px", 96.0f);
    if (!svg) return im;
    float sw = svg->width > 0 ? svg->width : 300.0f, sh = svg->height > 0 ? svg->height : 150.0f;
    float k = std::min(1.0f, 2048.0f / std::max(sw, sh));
    w = std::max(1, (int)std::lround(sw * k));
    h = std::max(1, (int)std::lround(sh * k));
    rgba.assign((size_t)w * h * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, svg, 0, 0, k, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
  } else if (bytes.size() > 12 && bytes.compare(0, 4, "RIFF") == 0 && bytes.compare(8, 4, "WEBP") == 0) {
    // Ranger's own decoder (lib/image/WebPDecoder.rgr), compiled in above
    std::vector<uint8_t> in(bytes.begin(), bytes.end());
    auto dec = std::make_shared<WebPDecoder>();
    dec->maxDimension = 8192;
    std::shared_ptr<ImageBuffer> img = dec->decodeBytes(in);
    if (!dec->ok || !img) return im;
    w = img->width;
    h = img->height;
    rgba = std::move(img->pixels);
    if ((size_t)w * h * 4 != rgba.size()) return im;
  } else if (bytes.size() > 22 && bytes.compare(0, 4, std::string("\0\0\1\0", 4)) == 0) {
    // a favicon.ico: the entry nearest 32 px; a PNG inside is decoded as
    // one, a bitmap is given the BMP file header it lacks
    return decodeIco(bytes);
  } else {
    int n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return im;
    rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
  }
  if (w <= 0 || h <= 0 || rgba.size() != (size_t)w * h * 4) return im;
  im.ok = true;
  im.w = w;
  im.h = h;
  im.rgba = std::move(rgba);
  return im;
}

// The bytes of a data: URL (percent-encoded or base64), as inline <svg>
// pictures and small embedded images arrive.
static bool decodeDataUrl(const std::string& url, std::string& out) {
  size_t comma = url.find(',');
  if (url.compare(0, 5, "data:") != 0 || comma == std::string::npos) return false;
  std::string head = lower(url.substr(5, comma - 5));
  std::string body = url.substr(comma + 1);
  out.clear();
  if (head.find(";base64") != std::string::npos) {
    static const std::string abc = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned acc = 0;
    int bits = 0;
    for (char ch : body) {
      size_t v = abc.find(ch);
      if (v == std::string::npos) continue;
      acc = (acc << 6) | (unsigned)v;
      bits += 6;
      if (bits >= 8) {
        bits -= 8;
        out.push_back((char)((acc >> bits) & 0xff));
      }
    }
    return true;
  }
  for (size_t i = 0; i < body.size(); i++) {
    if (body[i] == '%' && i + 2 < body.size() && std::isxdigit((unsigned char)body[i + 1]) &&
        std::isxdigit((unsigned char)body[i + 2])) {
      out.push_back((char)std::strtol(body.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      out.push_back(body[i]);
    }
  }
  return true;
}

// The painter's output run with SDL3. DlPainter (the shared painter,
// compiled from RangerDOM's paint/ crate) makes a frame into textures to
// create, draws (triangles, textured or not), clips, and textures to
// destroy; this makes those calls and nothing else.
class SdlPainter {
 public:
  SDL_Renderer* ren = nullptr;
  std::shared_ptr<DlPainter> dl = DlPainter::new_();

  // the faces the layout measures with draw the text too
  void setFonts(const Fonts& fonts) {
    emojiFonts = &fonts;
    dl->add_face("sans", fonts.face[SANS].data);
    if (fonts.face[SANS_BOLD].data != fonts.face[SANS].data) dl->add_face("bold", fonts.face[SANS_BOLD].data);
    if (fonts.face[MONO].data != fonts.face[SANS].data) dl->add_face("mono", fonts.face[MONO].data);
    for (auto& fb : fonts.fallbacks) dl->add_face("fallback", fb->data);
  }

  void paint(const std::shared_ptr<EVGDisplayList>& list, double s) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_SetRenderClipRect(ren, nullptr);
    SDL_RenderClear(ren);
    dl->s = s;
    dl->top = 0.0;
    dl->begin();
    dl->draw_list(list);
    dl->end();
    run();
    // (pictures the list named that nobody gave: the app asks for every
    // picture it shows, so nothing is to be done about them here; colour
    // emoji the text asked for are given from the emoji face, and the
    // frame is drawn again with them)
    if (giveEmoji()) {
      SDL_SetRenderClipRect(ren, nullptr);
      SDL_RenderClear(ren);
      dl->begin();
      dl->draw_list(list);
      dl->end();
      run();
      giveEmoji();
    }
  }

  // the textures, before the renderer goes
  void release() {
    for (auto& kv : texs) SDL_DestroyTexture(kv.second);
    texs.clear();
  }

 private:
  const Fonts* emojiFonts = nullptr;
  std::unordered_map<int, SDL_Texture*> texs;
  std::vector<SDL_Vertex> verts;

  // "emoji:<codepoint>" in missing: the glyph's PNG from the colour face,
  // with its advance in ems; whether any was given
  bool giveEmoji() {
    bool gave = false;
    for (auto& m : dl->missing) {
      if (m.compare(0, 6, "emoji:") != 0 || !emojiFonts || !emojiFonts->emoji.ok) continue;
      int cp = std::atoi(m.c_str() + 6);
      const FontFile& f = emojiFonts->emoji.font;
      int g = stbtt_FindGlyphIndex(&f.info, cp);
      const unsigned char* png = nullptr;
      size_t len = 0;
      if (g == 0 || !emojiFonts->emoji.png(g, png, len)) continue;
      Image im = decodeImage(std::string((const char*)png, len));
      if (!im.ok) continue;
      int adv = 0, lsb = 0;
      stbtt_GetGlyphHMetrics(&f.info, g, &adv, &lsb);
      dl->set_emoji(cp, adv * f.scale(1.0), im.w, im.h, std::move(im.rgba));
      gave = true;
    }
    dl->missing.clear();
    return gave;
  }
  std::vector<int> idx;

  void run() {
    for (auto& t : dl->uploads) {
      SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, t->w, t->h);
      if (!tex) continue;
      SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
      SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
      SDL_UpdateTexture(tex, nullptr, t->px.data(), t->w * 4);
      texs[t->id] = tex;
    }
    const std::vector<double>& v = dl->verts;
    verts.resize(v.size() / 8);
    for (size_t k = 0; k < verts.size(); k++) {
      const double* q = &v[k * 8];
      verts[k] = SDL_Vertex{{(float)q[0], (float)q[1]}, {(float)q[2], (float)q[3], (float)q[4], (float)q[5]}, {(float)q[6], (float)q[7]}};
    }
    idx.assign(dl->idx.begin(), dl->idx.end());
    const std::vector<int>& ops = dl->ops;
    for (size_t o = 0; o + 6 <= ops.size(); o += 6) {
      int kind = ops[o], a = ops[o + 1], b = ops[o + 2], c = ops[o + 3], d = ops[o + 4], e = ops[o + 5];
      if (kind == 1) {  // draw: texture, first vertex, vertices, first index, indices
        SDL_Texture* tex = nullptr;
        if (a != 0) {
          auto it = texs.find(a);
          if (it == texs.end()) continue;
          tex = it->second;
        }
        SDL_RenderGeometry(ren, tex, verts.data() + b, c, idx.data() + d, e);
      } else if (kind == 2) {  // clip
        SDL_Rect r{a, b, c > 0 ? c : 0, d > 0 ? d : 0};
        if (r.w == 0 || r.h == 0) r = SDL_Rect{0, 0, 1, 1};  // an empty clip: nothing shows
        SDL_SetRenderClipRect(ren, &r);
      } else if (kind == 3) {
        SDL_SetRenderClipRect(ren, nullptr);
      }
    }
    for (int id : dl->frees) {
      auto it = texs.find(id);
      if (it != texs.end()) {
        SDL_DestroyTexture(it->second);
        texs.erase(it);
      }
    }
    dl->done();
  }
};

// ---------------------------------------------------------------------------
// The script realm, as a child process of this same binary
// ---------------------------------------------------------------------------
//
// A page's scripts run in `evg-browser --realm`, which keeps the page alive
// as long as it is shown: the browser's requests (src/RealmWire.rgr: the
// page to start, then the user's events and the time that passed) go in on
// its stdin and the answers come out on its stdout, each as one field,
// "<length>\n<bytes>"; neither side here reads them. A new page starts a
// new child. A request that takes too long kills the child, and a script
// that overflows the stack takes down only the child; the page stays as it
// was. The child has no window and makes no requests.

static void putField(std::string& out, const std::string& v) {
  out += std::to_string(v.size());
  out += '\n';
  out += v;
}

#ifndef _WIN32
static bool readExactly(int fd, char* buf, size_t n) {
  size_t got = 0;
  while (got < n) {
    ssize_t k = ::read(fd, buf + got, n - got);
    if (k <= 0) return false;
    got += (size_t)k;
  }
  return true;
}

// one field from a pipe; false at its end
static bool readField(int fd, std::string& v) {
  std::string len;
  char c;
  for (;;) {
    if (!readExactly(fd, &c, 1)) return false;
    if (c == '\n') break;
    len += c;
    if (len.size() > 20) return false;
  }
  size_t n = (size_t)std::strtoull(len.c_str(), nullptr, 10);
  v.assign(n, '\0');
  return n == 0 || readExactly(fd, &v[0], n);
}

static bool writeAll(int fd, const std::string& s) {
  size_t done = 0;
  while (done < s.size()) {
    ssize_t k = ::write(fd, s.data() + done, s.size() - done);
    if (k <= 0) return false;
    done += (size_t)k;
  }
  return true;
}
#endif

// The child's side: requests until its stdin closes.
static int realmMain() {
#ifndef _WIN32
  auto realm = std::make_shared<PageRealm>();
  std::string request;
  while (readField(0, request)) {
    std::string out;
    putField(out, realm->handle(request));
    if (!writeAll(1, out)) break;
  }
#endif
  return 0;
}

static std::string selfPath(const char* argv0) {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof buf;
  if (_NSGetExecutablePath(buf, &size) == 0) return buf;
#elif defined(__linux__)
  char buf[4096];
  ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    return buf;
  }
#endif
  return argv0 ? argv0 : "";
}

class RealmProcess {
 public:
  std::string exe;
  // a step's limit; starting runs all of the page's scripts (a React
  // bundle of 700 KB takes 10-15 s on CEr), so it gets a longer one
  double limitMs = 8000;
  double startLimitMs = 45000;

  bool available() const {
#ifdef _WIN32
    return false;
#else
    return !exe.empty();
#endif
  }

  // a request is out
  bool busy() const { return job >= 0; }

  void send(int jobId, bool start, const std::string& request) {
#ifndef _WIN32
    if (start || pid <= 0) {
      stop();
      if (!spawn()) return;
    }
    joinAll();
    job = jobId;
    jobStart = start;
    started = SDL_GetTicks();
    {
      std::lock_guard<std::mutex> lk(mu);
      finished = false;
      failed = false;
      reply.clear();
    }
    std::string payload;
    putField(payload, request);
    int toChild = toFd, fromChild = fromFd;
    // one thread writes the request and one reads the answer, so a large
    // page cannot fill a pipe with nobody draining the other end
    writer = std::thread([toChild, payload] { writeAll(toChild, payload); });
    reader = std::thread([this, fromChild] {
      std::string got;
      bool ok = readField(fromChild, got);
      std::lock_guard<std::mutex> lk(mu);
      reply = std::move(got);
      failed = !ok;
      finished = true;
    });
#endif
  }

  // Called every frame: hands a finished (or timed-out) answer to the app.
  void poll(const std::shared_ptr<BrowserApp>& app, bool& dirty) {
#ifndef _WIN32
    if (job < 0) return;
    bool fin = false, bad = false;
    std::string got;
    {
      std::lock_guard<std::mutex> lk(mu);
      fin = finished;
      bad = failed;
      if (fin) got = std::move(reply);
    }
    int j = job;
    if (!fin) {
      if ((double)(SDL_GetTicks() - started) > (jobStart ? startLimitMs : limitMs)) {
        stop();
        app->realmFailed(j, "aikaraja");
        dirty = true;
      }
      return;
    }
    joinAll();
    job = -1;
    if (bad) {
      stop();
      app->realmFailed(j, "the realm ended without an answer");
    } else {
      app->realmResult(j, got);
    }
    dirty = true;
#endif
  }

  void stop() {
#ifndef _WIN32
    if (pid > 0) {
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      pid = -1;
    }
    joinAll();
    if (toFd >= 0) ::close(toFd);
    if (fromFd >= 0) ::close(fromFd);
    toFd = fromFd = -1;
    job = -1;
#endif
  }

  ~RealmProcess() { stop(); }

 private:
#ifndef _WIN32
  pid_t pid = -1;
#else
  int pid = -1;
#endif
  int job = -1;
  bool jobStart = false;
  int toFd = -1, fromFd = -1;
  Uint64 started = 0;
  std::thread writer, reader;
  std::mutex mu;
  bool finished = false, failed = false;
  std::string reply;

  void joinAll() {
    if (writer.joinable()) writer.join();
    if (reader.joinable()) reader.join();
  }

#ifndef _WIN32
  bool spawn() {
    int in[2], out[2];
    if (::pipe(in) != 0) return false;
    if (::pipe(out) != 0) {
      ::close(in[0]);
      ::close(in[1]);
      return false;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addclose(&fa, in[1]);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    char* args[] = {(char*)exe.c_str(), (char*)"--realm", nullptr};
    pid_t p = -1;
    int rc = posix_spawn(&p, exe.c_str(), &fa, nullptr, args, environ);
    posix_spawn_file_actions_destroy(&fa);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) {
      ::close(in[1]);
      ::close(out[0]);
      return false;
    }
    pid = p;
    toFd = in[1];
    fromFd = out[0];
    return true;
  }
#endif
};

// ---------------------------------------------------------------------------
// Keys, as the app names them
// ---------------------------------------------------------------------------

static std::string keyName(SDL_Keycode k) {
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_DELETE: return "Delete";
    case SDLK_UP: return "ArrowUp";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_RIGHT: return "ArrowRight";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_SPACE: return " ";
    case SDLK_L: return "l";
    case SDLK_R: return "r";
    case SDLK_A: return "a";
    default: return "";
  }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

struct Options {
  std::string url = "about:home";
  std::vector<std::string> more;  // further addresses, each in a tab of its own
  int width = 1100, height = 800;
  std::string screenshot;
  std::string allow;
  bool open = false;
  bool scripts = true;
  std::string samples = "https://terotests.github.io/EVGBrowser/samples/";
  int frames = 0;
  int settleMs = 0;
};

static Options parseArgs(int argc, char** argv) {
  Options o;
  bool urlGiven = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
    if (a == "--width") o.width = std::atoi(next().c_str());
    else if (a == "--height") o.height = std::atoi(next().c_str());
    else if (a == "--screenshot") o.screenshot = next();
    else if (a == "--allow") o.allow = next();
    else if (a == "--no-js") o.scripts = false;
    else if (a == "--open" || a == "--unsafe") o.open = true;
    else if (a == "--samples") o.samples = next();
    else if (a == "--frames") o.frames = std::atoi(next().c_str());
    else if (a == "--settle") o.settleMs = std::atoi(next().c_str());
    else if (a == "--help" || a == "-h") {
      std::printf(
          "evg-browser [url ...] [options]   (each address in a tab)\n"
          "  --width W --height H     window size\n"
          "  --allow a.fi,b.org       also allow these sites (and their subdomains)\n"
          "  --open, --unsafe         any site: switch the allowlist off (EVG_OPEN=1 too)\n"
          "  --no-js                  do not run page scripts\n"
          "  --samples URL            where the sample pages are (default: the Pages demo)\n"
          "  --screenshot out.png     render headless once the page has loaded, then exit\n"
          "  --settle MS              how long to wait for quiet before the screenshot\n");
      std::exit(0);
    } else if (o.url == "about:home" && !urlGiven) {
      o.url = a;
      urlGiven = true;
    } else {
      o.more.push_back(a);
    }
  }
  return o;
}

static int run(int argc, char** argv) {
  Options opt = parseArgs(argc, argv);
  bool headless = !opt.screenshot.empty();

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  curl_global_init(CURL_GLOBAL_DEFAULT);

  Fonts fonts;
  if (!fonts.load()) {
    std::fprintf(stderr, "no font found: install DejaVu or Liberation fonts, or set EVG_FONT_DIR\n");
    return 1;
  }

  // The layout measures with the faces the painter draws with. Installed
  // before the app exists, because every layout reads the default when made.
  auto measurer = std::make_shared<EVGHostTextMeasurer>();
  measurer->attach(
      [&fonts](int kind, std::string text, std::string family, double size, bool bold, bool) -> double {
        return fonts.metric(kind, text, family, size, bold);
      },
      "sdl-ttf");
  EVGDefaultMeasurer::install(measurer);

  SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (headless ? SDL_WINDOW_HIDDEN : 0);
  SDL_Window* win = SDL_CreateWindow("EVG Browser", opt.width, opt.height, flags);
  if (!win) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
  if (!ren) ren = SDL_CreateRenderer(win, SDL_SOFTWARE_RENDERER);
  if (!ren) {
    std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_SetRenderVSync(ren, 1);
  SDL_StopTextInput(win);

  auto measureWindow = [&](int& w, int& h, double& scale) {
    int ow = 0, oh = 0;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
    scale = w > 0 ? (double)ow / w : 1.0;
  };
  int winW = 0, winH = 0;
  double scale = 1.0;
  measureWindow(winW, winH, scale);

  // the pictures asked for: their size, or failed (the pixels are the painter's)
  std::unordered_map<std::string, Image> images;
  SdlPainter painter;
  painter.ren = ren;
  painter.setFonts(fonts);


  auto host = std::make_shared<BrowserHost>();
  std::shared_ptr<BrowserApp> app = host->browser;
  // a decoded picture to the painter, its size (or the failure) to the app
  auto keepImage = [&](const std::string& url, Image im) {
    if (im.ok) painter.dl->set_image(url, im.w, im.h, im.rgba);
    im.rgba.clear();
    images[url] = im;
    if (im.ok) app->imageLoaded(url, im.w, im.h); else app->imageFailed(url);
  };
  // the sample pages that ship with the web demo, where Pages publishes them
  app->setSamplesBase(opt.samples);
  // page scripts run in a child process of this binary (--realm)
  RealmProcess realm;
  realm.exe = selfPath(argv[0]);
  app->setScriptsEnabled(opt.scripts && realm.available());
  if (opt.open || std::getenv("EVG_OPEN")) app->setAllowlistEnabled(false);
  if (!opt.allow.empty()) app->setAllowedSites(app->allowedSitesText() + "," + opt.allow);
  host->startAt(winW, winH, false, opt.url);
  for (auto& u : opt.more) app->newTab(u);
  if (!opt.more.empty()) app->selectTab(0);

  Net net(6);
  SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
  SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
  SDL_Cursor* ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
  SDL_Cursor* shown = arrow;

  bool dirty = true, quit = false, mouseDown = false;
  double lastX = 0, lastY = 0;
  Uint64 last = SDL_GetTicks(), lastMove = last;
  std::string textFocus, title;
  int frame = 0;
  Uint64 idleSince = 0;
  const Uint64 started = SDL_GetTicks();

  auto pump = [&]() {
    for (;;) {
      std::string line = app->nextRequest();
      if (line.empty()) break;
      size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
      Job j;
      j.id = std::atoi(line.substr(0, t1).c_str());
      j.kind = line.substr(t1 + 1, t2 - t1 - 1);
      j.url = line.substr(t2 + 1);
      if (j.kind == "image" && images.count(j.url)) {
        const Image& im = images[j.url];
        if (im.ok) app->imageLoaded(j.url, im.w, im.h); else app->imageFailed(j.url);
        continue;
      }
      std::string inlineBytes;
      if (j.kind == "image" && decodeDataUrl(j.url, inlineBytes)) {
        keepImage(j.url, decodeImage(inlineBytes));
        dirty = true;
        continue;
      }
      net.submit(j);
    }
  };

  auto deliver = [&]() {
    Result r;
    bool any = false;
    while (net.poll(r)) {
      any = true;
      if (r.kind == "image") {
        Image im;
        if (r.status >= 200 && r.status < 300 && !r.body.empty()) im = decodeImage(r.body);
        keepImage(r.url, std::move(im));
        continue;
      }
      if (r.status == 0) {
        app->deliver(r.id, 0, "", "", "Sivua ei saatu haettua: " + r.error);
      } else {
        app->deliver(r.id, (int)r.status, r.finalUrl, r.contentType, asUtf8(r.body, r.contentType));
      }
    }
    if (any) {
      dirty = true;
      pump();
    }
  };

  // Where the mouse is, in the points the layout uses: SDL3's events count
  // window points on every platform. EVG_MOUSE_SCALE divides them, for a
  // build that counts pixels.
  double mouseDiv = 1;
  if (const char* ms = std::getenv("EVG_MOUSE_SCALE")) mouseDiv = std::max(0.1, std::atof(ms));
  bool debugInput = std::getenv("EVG_DEBUG_INPUT") != nullptr;
  auto mousePoint = [&](float ex, float ey, double& x, double& y) {
    x = ex / mouseDiv;
    y = ey / mouseDiv;
    if (debugInput)
      std::fprintf(stderr, "mouse %.1f,%.1f -> %.1f,%.1f (window %dx%d, scale %.2f)\n", ex, ey, x, y, winW, winH, scale);
  };

  auto syncTextInput = [&]() {
    std::string f = host->focusedField();
    if (f == textFocus) return;
    textFocus = f;
    if (f.empty()) SDL_StopTextInput(win); else SDL_StartTextInput(win);
  };

  pump();
  int failures = 0;
  while (!quit) {
   try {
    SDL_Event e;
    bool got = SDL_WaitEventTimeout(&e, dirty ? 1 : 16);
    while (got) {
      switch (e.type) {
        case SDL_EVENT_QUIT:
          quit = true;
          break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: {
          int ow = winW, oh = winH;
          measureWindow(winW, winH, scale);
          if (winW != ow || winH != oh) host->resize(winW, winH);
          dirty = true;
          break;
        }
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_SHOWN:
          dirty = true;
          break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
          if (e.button.button == SDL_BUTTON_LEFT) {
            double px = 0, py = 0;
            mousePoint(e.button.x, e.button.y, px, py);
            mouseDown = true;
            lastX = px;
            lastY = py;
            lastMove = SDL_GetTicks();
            host->pressAt(px, py);
            dirty = true;
          } else if (e.button.button == SDL_BUTTON_X1) {
            app->back();
            dirty = true;
          } else if (e.button.button == SDL_BUTTON_X2) {
            app->forward();
            dirty = true;
          }
          break;
        case SDL_EVENT_MOUSE_BUTTON_UP:
          if (e.button.button == SDL_BUTTON_LEFT && mouseDown) {
            mouseDown = false;
            host->releasePress();
            syncTextInput();
            dirty = true;
          }
          break;
        case SDL_EVENT_MOUSE_MOTION: {
          double px = 0, py = 0;
          mousePoint(e.motion.x, e.motion.y, px, py);
          if (mouseDown) {
            Uint64 now = SDL_GetTicks();
            double dt = (double)std::max<Uint64>(1, now - lastMove);
            lastMove = now;
            if (host->panAt(px - lastX, py - lastY, dt)) dirty = true;
            lastX = px;
            lastY = py;
          } else if (host->hoverAt(px, py)) {
            dirty = true;
          }
          break;
        }
        case SDL_EVENT_MOUSE_WHEEL: {
          // e.wheel.y already follows the OS setting (macOS natural
          // scrolling included); FLIPPED only reports that the OS inverted
          // it. Undoing that made trackpad swipes scroll the wrong way.
          double dy = -e.wheel.y * 60.0;
          if (host->wheel(dy)) dirty = true;
          break;
        }
        case SDL_EVENT_TEXT_INPUT:
          if (!host->focusedField().empty()) {
            host->typeText(e.text.text);
            dirty = true;
          }
          break;
        case SDL_EVENT_KEY_DOWN: {
          SDL_Keymod mod = SDL_GetModState();
          bool ctrl = (mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
          bool shift = (mod & SDL_KMOD_SHIFT) != 0;
          SDL_Keycode k = e.key.key;
          bool inField = !host->focusedField().empty();
          if ((mod & SDL_KMOD_ALT) && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
            if (k == SDLK_LEFT) app->back(); else app->forward();
          } else if (ctrl && k == SDLK_V && inField) {
            char* clip = SDL_GetClipboardText();
            if (clip) {
              host->typeText(clip);
              SDL_free(clip);
            }
          } else if (ctrl && k == SDLK_Q) {
            quit = true;
          } else {
            std::string name = keyName(k);
            if (name == "l" || name == "r") {
              if (!ctrl) name = "";
            }
            // select all in a field; the page keeps its own ctrl+A
            if (name == "a" && !(ctrl && inField)) name = "";
            // a space is text in a field, and scrolls the page outside one
            if (name == " " && inField) name = "";
            if (!name.empty()) host->key(name, shift, ctrl);
          }
          syncTextInput();
          dirty = true;
          break;
        }
        default:
          break;
      }
      got = SDL_PollEvent(&e);
    }

    Uint64 now = SDL_GetTicks();
    double dt = (double)std::min<Uint64>(64, now - last);
    last = now;
    if (host->tick(dt)) dirty = true;
    deliver();
    pump();
    realm.poll(app, dirty);
    if (!realm.busy() && app->realmJobPending()) {
      int job = app->takeRealmJob();
      bool start = app->realmRequestIsStart();
      realm.send(job, start, app->realmRequest());
    }

    if (dirty) {
      std::shared_ptr<EVGDisplayList> dl = app->display();
      painter.paint(dl, scale);
      SDL_RenderPresent(ren);
      dirty = false;
      frame++;
      syncTextInput();
      std::string t = app->pageTitle();
      std::string want = t.empty() ? "EVG Browser" : t + " — EVG Browser";
      if (want != title) {
        title = want;
        SDL_SetWindowTitle(win, title.c_str());
      }
      std::string hov = app->hoveredId();
      SDL_Cursor* c = hov.empty() ? arrow : (hov == "addr" ? ibeam : hand);
      if (c != shown) {
        SDL_SetCursor(c);
        shown = c;
      }
    }

    if (headless) {
      // a screenshot once nothing is loading and nothing has moved for a
      // while — or after --frames frames, whichever comes first
      bool quiet = net.busy() == 0 && !app->isLoading() && app->pendingRequests() == 0 && !realm.busy() && !app->realmJobPending();
      if (quiet) {
        if (idleSince == 0) idleSince = now;
      } else {
        idleSince = 0;
      }
      bool settled = idleSince != 0 && now - idleSince > (Uint64)std::max(400, opt.settleMs);
      bool timedOut = now - started > 45000;
      if (settled || timedOut || (opt.frames > 0 && frame >= opt.frames)) {
        std::shared_ptr<EVGDisplayList> dl = app->display();
        painter.paint(dl, scale);
        int ok = 1, ow = 0, oh = 0;
        SDL_Surface* shot = SDL_RenderReadPixels(ren, nullptr);
        SDL_Surface* rgba = shot ? SDL_ConvertSurface(shot, SDL_PIXELFORMAT_RGBA32) : nullptr;
        if (rgba) {
          ow = rgba->w;
          oh = rgba->h;
          ok = stbi_write_png(opt.screenshot.c_str(), ow, oh, 4, rgba->pixels, rgba->pitch) ? 0 : 1;
          SDL_DestroySurface(rgba);
        }
        if (shot) SDL_DestroySurface(shot);
        std::printf("%s %s (%dx%d) url=%s title=%s\n", ok == 0 ? "wrote" : "FAILED to write", opt.screenshot.c_str(), ow, oh,
                    app->currentUrl().c_str(), app->pageTitle().c_str());
        quit = true;
      }
    }
   } catch (const std::exception& ex) {
    // A page the engine could not handle (for example a missing value in
    // the generated code: std::bad_optional_access). Show the error page
    // instead of aborting; give up only if even that keeps failing.
    std::string where = app->currentUrl();
    std::fprintf(stderr, "evg-browser: error while showing %s: %s\n", where.c_str(), ex.what());
    if (++failures > 5) {
      std::fprintf(stderr, "evg-browser: too many errors, stopping\n");
      break;
    }
    try {
      app->showFailure(std::string("Sivua ei voitu näyttää (") + ex.what() + ").\n" + where);
    } catch (const std::exception&) {
    }
    dirty = true;
   }
  }

  std::fflush(stdout);
  SDL_DestroyCursor(arrow);
  SDL_DestroyCursor(hand);
  SDL_DestroyCursor(ibeam);
  painter.release();
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  curl_global_cleanup();
  SDL_Quit();
  return 0;
}

}  // namespace evgsdl

int main(int argc, char** argv) {
  __g_argc = argc;
  __g_argv = argv;
  if (argc > 1 && std::string(argv[1]) == "--realm") return evgsdl::realmMain();
#ifndef _WIN32
  signal(SIGPIPE, SIG_IGN);  // a killed realm must not take the browser with it
#endif
  SDL_SetMainReady();
  return evgsdl::run(argc, argv);
}

// SPDX-License-Identifier: AGPL-3.0-or-later
//
// EVG Browser in a desktop window: SDL2 for the window and the pixels,
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
//   * a painter for EVGDisplayList: rounded boxes and borders as triangle
//     geometry, text runs, images with object-fit: cover, a clip stack.
//
// Build: native/sdl/build.sh (or `npm run sdl`).

#include "evg_browser.cpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <curl/curl.h>

// Type, pictures and screenshots from single-file libraries compiled in
// (third_party/README.md), so the only libraries needed are SDL2 and libcurl.
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
#define SIMPLEWEBP_IMPLEMENTATION
#define SIMPLEWEBP_DISABLE_STDIO
#include "third_party/simplewebp.h"

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
    curl_easy_setopt(c, CURLOPT_USERAGENT, "EVG-Browser/0.1 (SDL2; +https://github.com/terotests/EVGBrowser)");
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
// The painter: EVGDisplayList → SDL_Renderer
// ---------------------------------------------------------------------------

struct Image {
  SDL_Texture* tex = nullptr;
  int w = 0, h = 0;
};

// A picture's bytes as a texture: JPEG, PNG, GIF (first frame), BMP through
// stb_image; WebP through simplewebp; SVG through nanosvg, rasterised at its
// own size (at most 2048 on a side). AVIF is not decoded or asked for.
static Image decodeImage(SDL_Renderer* ren, const std::string& bytes) {
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
    std::string copy = bytes;  // the decoder reads it in place until unloaded
    simplewebp* wp = nullptr;
    if (simplewebp_load_from_memory(&copy[0], copy.size(), nullptr, &wp) != SIMPLEWEBP_NO_ERROR || !wp) return im;
    size_t ww = 0, wh = 0;
    simplewebp_get_dimensions(wp, &ww, &wh);
    if (ww == 0 || wh == 0 || ww > 8192 || wh > 8192) {
      simplewebp_unload(wp);
      return im;
    }
    w = (int)ww;
    h = (int)wh;
    rgba.assign((size_t)w * h * 4, 0);
    simplewebp_error err = simplewebp_decode(wp, rgba.data(), nullptr);
    simplewebp_unload(wp);
    if (err != SIMPLEWEBP_NO_ERROR) return im;
  } else {
    int n = 0;
    unsigned char* px = stbi_load_from_memory((const unsigned char*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return im;
    rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
  }
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
  if (!tex) return im;
  SDL_UpdateTexture(tex, nullptr, rgba.data(), w * 4);
  SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
  SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
  im.tex = tex;
  im.w = w;
  im.h = h;
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

class Painter {
 public:
  SDL_Renderer* ren = nullptr;
  Fonts* fonts = nullptr;
  double s = 1.0;  // device pixels per CSS pixel
  std::unordered_map<std::string, Image>* images = nullptr;

  void paint(const std::shared_ptr<EVGDisplayList>& dl, int width, int height) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_RenderSetClipRect(ren, nullptr);
    SDL_RenderClear(ren);
    clips.clear();
    viewW = width;
    viewH = height;
    for (auto& c : dl->cmds) {
      switch (c->kind) {
        case 0: rect(*c); break;
        case 1: border(*c); break;
        case 2: image(*c); break;
        case 3: text(*c); break;
        case 4: pushClip(*c); break;
        case 5: popClip(); break;
        case 7: stroke(*c); break;
        default: break;  // paths: a browser page without SVG has none
      }
    }
    SDL_RenderSetClipRect(ren, nullptr);
    if (textCache.size() > 4000) clearTextCache();
  }

  void clearTextCache() {
    for (auto& kv : textCache) SDL_DestroyTexture(kv.second.tex);
    textCache.clear();
    for (auto& kv : emojiCache) if (kv.second) SDL_DestroyTexture(kv.second);
    emojiCache.clear();
  }

  ~Painter() { clearTextCache(); }

 private:
  struct Tex {
    SDL_Texture* tex;
    int w, h, ascent;
  };
  std::unordered_map<std::string, Tex> textCache;
  std::unordered_map<int, SDL_Texture*> emojiCache;
  std::vector<SDL_Rect> clips;
  int viewW = 0, viewH = 0;

  static SDL_Color color(int r, int g, int b, double a) {
    int al = (int)std::lround(std::max(0.0, std::min(1.0, a)) * 255.0);
    return SDL_Color{(Uint8)r, (Uint8)g, (Uint8)b, (Uint8)al};
  }

  bool offscreen(const EVGDrawCmd& c) const {
    return (c.y + c.h) * s < 0 || c.y * s > viewH || (c.x + c.w) * s < 0 || c.x * s > viewW;
  }

  static void radii(const EVGDrawCmd& c, double out[4]) {
    double lim = std::min(c.w, c.h) / 2.0;
    out[0] = c.radius;
    out[1] = c.perCorner ? c.radiusTR : c.radius;
    out[2] = c.perCorner ? c.radiusBR : c.radius;
    out[3] = c.perCorner ? c.radiusBL : c.radius;
    for (int i = 0; i < 4; i++) out[i] = std::max(0.0, std::min(out[i], lim));
  }

  // The outline of a rounded rectangle: corners TL, TR, BR, BL, the same
  // number of points each whatever the radius, so two outlines can be
  // stitched into a band.
  void outline(double x, double y, double w, double h, const double r[4], std::vector<SDL_FPoint>& pts) const {
    const int SEG = 8;
    const double cx[4] = {x + r[0], x + w - r[1], x + w - r[2], x + r[3]};
    const double cy[4] = {y + r[0], y + r[1], y + h - r[2], y + h - r[3]};
    const double start[4] = {M_PI, 1.5 * M_PI, 0.0, 0.5 * M_PI};
    pts.clear();
    for (int k = 0; k < 4; k++) {
      for (int i = 0; i <= SEG; i++) {
        double a = start[k] + (M_PI / 2.0) * i / SEG;
        pts.push_back(SDL_FPoint{(float)((cx[k] + std::cos(a) * r[k]) * s), (float)((cy[k] + std::sin(a) * r[k]) * s)});
      }
    }
  }

  void rect(const EVGDrawCmd& c) {
    if (c.w <= 0 || c.h <= 0 || offscreen(c)) return;
    double r[4];
    radii(c, r);
    SDL_Color top = color(c.r, c.g, c.b, c.a);
    SDL_Color bottom = c.hasGrad ? color(c.r2, c.g2, c.b2, c.a2) : top;
    bool across = c.hasGrad && c.gradDir == 1;
    std::vector<SDL_FPoint> pts;
    outline(c.x, c.y, c.w, c.h, r, pts);
    std::vector<SDL_Vertex> v;
    auto shade = [&](const SDL_FPoint& p) {
      if (!c.hasGrad) return top;
      double t = across ? (p.x / s - c.x) / c.w : (p.y / s - c.y) / c.h;
      t = std::max(0.0, std::min(1.0, t));
      return SDL_Color{(Uint8)(top.r + (bottom.r - top.r) * t), (Uint8)(top.g + (bottom.g - top.g) * t),
                       (Uint8)(top.b + (bottom.b - top.b) * t), (Uint8)(top.a + (bottom.a - top.a) * t)};
    };
    SDL_FPoint mid{(float)((c.x + c.w / 2) * s), (float)((c.y + c.h / 2) * s)};
    SDL_Color midC = shade(mid);
    for (size_t i = 0; i < pts.size(); i++) {
      const SDL_FPoint& a = pts[i];
      const SDL_FPoint& b = pts[(i + 1) % pts.size()];
      v.push_back(SDL_Vertex{mid, midC, {0, 0}});
      v.push_back(SDL_Vertex{a, shade(a), {0, 0}});
      v.push_back(SDL_Vertex{b, shade(b), {0, 0}});
    }
    SDL_RenderGeometry(ren, nullptr, v.data(), (int)v.size(), nullptr, 0);
  }

  // CSS draws a border inside the box: a band from the edge inwards.
  void border(const EVGDrawCmd& c) {
    double t = c.thickness > 0 ? c.thickness : 1.0;
    if (c.w <= t || c.h <= t || offscreen(c)) return;
    double r[4], ri[4];
    radii(c, r);
    for (int i = 0; i < 4; i++) ri[i] = std::max(0.0, r[i] - t);
    std::vector<SDL_FPoint> outer, inner;
    outline(c.x, c.y, c.w, c.h, r, outer);
    outline(c.x + t, c.y + t, c.w - 2 * t, c.h - 2 * t, ri, inner);
    SDL_Color col = color(c.r, c.g, c.b, c.a);
    std::vector<SDL_Vertex> v;
    for (size_t i = 0; i < outer.size(); i++) {
      size_t j = (i + 1) % outer.size();
      v.push_back(SDL_Vertex{outer[i], col, {0, 0}});
      v.push_back(SDL_Vertex{outer[j], col, {0, 0}});
      v.push_back(SDL_Vertex{inner[i], col, {0, 0}});
      v.push_back(SDL_Vertex{inner[i], col, {0, 0}});
      v.push_back(SDL_Vertex{outer[j], col, {0, 0}});
      v.push_back(SDL_Vertex{inner[j], col, {0, 0}});
    }
    SDL_RenderGeometry(ren, nullptr, v.data(), (int)v.size(), nullptr, 0);
  }

  void stroke(const EVGDrawCmd& c) {
    if (c.pts.size() < 4) return;
    SDL_Color col = color(c.r, c.g, c.b, c.a);
    SDL_SetRenderDrawColor(ren, col.r, col.g, col.b, col.a);
    std::vector<SDL_FPoint> p;
    for (size_t i = 0; i + 1 < c.pts.size(); i += 2) p.push_back(SDL_FPoint{(float)(c.pts[i] * s), (float)(c.pts[i + 1] * s)});
    SDL_RenderDrawLinesF(ren, p.data(), (int)p.size());
  }

  // object-fit: cover — fill the box, crop what overflows, centred.
  void image(const EVGDrawCmd& c) {
    if (c.w <= 0 || c.h <= 0 || offscreen(c) || !images) return;
    auto it = images->find(c.src);
    if (it == images->end() || !it->second.tex) return;
    const Image& im = it->second;
    SDL_Rect src;
    if (c.hasCrop) {
      src = SDL_Rect{(int)(c.cropX * im.w), (int)(c.cropY * im.h), (int)(c.cropW * im.w), (int)(c.cropH * im.h)};
    } else {
      double k = std::max(c.w / im.w, c.h / im.h);
      double sw = c.w / k, sh = c.h / k;
      src = SDL_Rect{(int)((im.w - sw) / 2), (int)((im.h - sh) / 2), (int)std::lround(sw), (int)std::lround(sh)};
    }
    SDL_FRect dst{(float)(c.x * s), (float)(c.y * s), (float)(c.w * s), (float)(c.h * s)};
    SDL_RenderCopyF(ren, im.tex, &src, &dst);
  }

  // A run rasterised at `px` device pixels: white, with the glyphs in the
  // alpha, so one texture serves every colour through the colour mod.
  Tex* runTexture(const Fonts::Run& run, double px) {
    std::string key = std::to_string((uintptr_t)run.font) + "|" + std::to_string(px) + "|";
    for (uint32_t cp : run.cps) key += std::to_string(cp) + ",";
    auto it = textCache.find(key);
    if (it != textCache.end()) return &it->second;
    const stbtt_fontinfo* info = &run.font->info;
    float sc = run.font->scale(px);
    int ascentPx = (int)std::ceil(run.font->ascent * sc);
    int h = (int)std::ceil((run.font->ascent - run.font->descent) * sc) + 2;
    int w = (int)std::ceil(Fonts::runWidth(run, px)) + 4;
    if (w <= 0 || h <= 0 || w > 16384) return nullptr;
    std::vector<unsigned char> alpha((size_t)w * h, 0);
    std::vector<unsigned char> glyph;
    double x = 1.0;
    for (size_t k = 0; k < run.cps.size(); k++) {
      int g = stbtt_FindGlyphIndex(info, (int)run.cps[k]);
      int adv = 0, lsb = 0;
      stbtt_GetGlyphHMetrics(info, g, &adv, &lsb);
      float shift = (float)(x - std::floor(x));
      int x0, y0, x1, y1;
      stbtt_GetGlyphBitmapBoxSubpixel(info, g, sc, sc, shift, 0, &x0, &y0, &x1, &y1);
      int gw = x1 - x0, gh = y1 - y0;
      if (gw > 0 && gh > 0) {
        glyph.assign((size_t)gw * gh, 0);
        stbtt_MakeGlyphBitmapSubpixel(info, glyph.data(), gw, gh, gw, sc, sc, shift, 0, g);
        int ox = (int)std::floor(x) + x0, oy = ascentPx + y0;
        for (int yy = 0; yy < gh; yy++) {
          int ty = oy + yy;
          if (ty < 0 || ty >= h) continue;
          for (int xx = 0; xx < gw; xx++) {
            int tx = ox + xx;
            if (tx < 0 || tx >= w) continue;
            unsigned char v = glyph[(size_t)yy * gw + xx];
            unsigned char& d = alpha[(size_t)ty * w + tx];
            if (v > d) d = v;
          }
        }
      }
      x += adv * sc;
      if (k + 1 < run.cps.size()) x += stbtt_GetGlyphKernAdvance(info, g, stbtt_FindGlyphIndex(info, (int)run.cps[k + 1])) * sc;
    }
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
    if (!tex) return nullptr;
    std::vector<Uint32> px32((size_t)w * h);
    for (size_t k = 0; k < px32.size(); k++) px32[k] = ((Uint32)alpha[k] << 24) | 0x00FFFFFFu;
    SDL_UpdateTexture(tex, nullptr, px32.data(), w * 4);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return &(textCache[key] = Tex{tex, w, h, ascentPx});
  }

  // Colour emoji: each glyph's PNG, decoded once, drawn as wide as its
  // advance and sitting on the baseline the way the text around it does.
  double emojiRun(const Fonts::Run& run, double x, double baseline, double size, Uint8 alpha) {
    const stbtt_fontinfo* info = &run.font->info;
    float sc = run.font->scale(size);
    for (uint32_t cp : run.cps) {
      int g = stbtt_FindGlyphIndex(info, (int)cp);
      int adv = 0, lsb = 0;
      stbtt_GetGlyphHMetrics(info, g, &adv, &lsb);
      double w = adv * sc;
      SDL_Texture* tex = emojiTexture(g);
      if (tex) {
        int tw = 0, th = 0;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        double h = tw > 0 ? w * th / tw : w;
        SDL_FRect dst{(float)(x * s), (float)((baseline + size * 0.12 - h) * s), (float)(w * s), (float)(h * s)};
        SDL_SetTextureAlphaMod(tex, alpha);
        SDL_RenderCopyF(ren, tex, nullptr, &dst);
      }
      x += w;
    }
    return x;
  }

  SDL_Texture* emojiTexture(int glyph) {
    auto it = emojiCache.find(glyph);
    if (it != emojiCache.end()) return it->second;
    SDL_Texture* tex = nullptr;
    const unsigned char* png = nullptr;
    size_t len = 0;
    if (fonts->emoji.png(glyph, png, len)) {
      int w = 0, h = 0, n = 0;
      unsigned char* px = stbi_load_from_memory(png, (int)len, &w, &h, &n, 4);
      if (px) {
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, w, h);
        if (tex) {
          SDL_UpdateTexture(tex, nullptr, px, w * 4);
          SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
          SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);
        }
        stbi_image_free(px);
      }
    }
    emojiCache[glyph] = tex;
    return tex;
  }

  void text(const EVGDrawCmd& c) {
    if (c.text.empty() || c.fontSize <= 0 || offscreen(c)) return;
    bool bold = c.fontWeight == "bold" || c.fontWeight == "700" || c.fontWeight == "800" || c.fontWeight == "900";
    Face face = Fonts::faceFor(c.fontFamily, bold);
    double size = c.fontSize;
    // the baseline the layout gave the line: half the leading above the face
    double asc = fonts->metric(1, "", c.fontFamily, size, bold);
    double desc = fonts->metric(2, "", c.fontFamily, size, bold);
    double baseline = c.y + (c.h > 0 ? (c.h - (asc + desc)) / 2.0 : 0.0) + asc;
    double x = c.x;
    SDL_Color col = color(c.r, c.g, c.b, c.a);
    for (auto& run : fonts->runs(c.text, face)) {
      if (fonts->isEmoji(run.font)) {
        x = emojiRun(run, x, baseline, size, col.a);
        continue;
      }
      Tex* t = runTexture(run, size * s);
      if (t) {
        SDL_FRect dst{(float)(std::floor(x * s) - 1.0), (float)(std::round(baseline * s) - t->ascent), (float)t->w, (float)t->h};
        SDL_SetTextureColorMod(t->tex, col.r, col.g, col.b);
        SDL_SetTextureAlphaMod(t->tex, col.a);
        SDL_RenderCopyF(ren, t->tex, nullptr, &dst);
      }
      x += Fonts::runWidth(run, size);
    }
  }

  void applyClip() {
    if (clips.empty()) {
      SDL_RenderSetClipRect(ren, nullptr);
      return;
    }
    SDL_Rect r = clips.back();
    if (r.w <= 0 || r.h <= 0) r = SDL_Rect{0, 0, 1, 1};  // an empty clip: nothing shows
    SDL_RenderSetClipRect(ren, &r);
  }

  void pushClip(const EVGDrawCmd& c) {
    SDL_Rect r{(int)std::floor(c.x * s), (int)std::floor(c.y * s), (int)std::ceil(c.w * s), (int)std::ceil(c.h * s)};
    if (!clips.empty()) {
      SDL_Rect out;
      if (!SDL_IntersectRect(&clips.back(), &r, &out)) out = SDL_Rect{0, 0, 0, 0};
      r = out;
    }
    clips.push_back(r);
    applyClip();
  }

  void popClip() {
    if (!clips.empty()) clips.pop_back();
    applyClip();
  }
};

// ---------------------------------------------------------------------------
// The script realm, as a child process of this same binary
// ---------------------------------------------------------------------------
//
// A page's scripts run in `evg-browser --realm`: the job (URL, HTML, window
// size, external scripts) goes in on its stdin, the DOM ops come out on its
// stdout, each field as "<length>\n<bytes>". A child that runs past its time
// is killed, and a script that overflows the stack takes down only the
// child; the page stays as the server sent it. The child has no window and
// makes no requests.

static void putField(std::string& out, const std::string& v) {
  out += std::to_string(v.size());
  out += '\n';
  out += v;
}

static bool getField(const std::string& in, size_t& at, std::string& v) {
  size_t nl = in.find('\n', at);
  if (nl == std::string::npos) return false;
  size_t n = (size_t)std::strtoull(in.substr(at, nl - at).c_str(), nullptr, 10);
  if (nl + 1 + n > in.size()) return false;
  v.assign(in, nl + 1, n);
  at = nl + 1 + n;
  return true;
}

static std::string readAll(int fd) {
  std::string out;
  char buf[65536];
  for (;;) {
#ifndef _WIN32
    ssize_t n = ::read(fd, buf, sizeof buf);
#else
    long n = -1;
#endif
    if (n <= 0) break;
    out.append(buf, (size_t)n);
  }
  return out;
}

// The child's side: one job, then exit.
static int realmMain() {
#ifndef _WIN32
  std::string in = readAll(0);
  size_t at = 0;
  std::string magic, url, html, w, h, count;
  if (!getField(in, at, magic) || magic != "evg-realm-1" || !getField(in, at, url) || !getField(in, at, html) ||
      !getField(in, at, w) || !getField(in, at, h) || !getField(in, at, count)) {
    std::fputs("bad job\n", stderr);
    return 2;
  }
  auto realm = std::make_shared<JsRealm>();
  int n = std::atoi(count.c_str());
  for (int i = 0; i < n; i++) {
    std::string su, st;
    if (!getField(in, at, su) || !getField(in, at, st)) return 2;
    realm->addScript(su, st);
  }
  std::string ops = realm->run(html, url, std::atof(w.c_str()), std::atof(h.c_str()));
  std::string out;
  putField(out, "ok");
  putField(out, ops);
  putField(out, realm->summary());
  size_t done = 0;
  while (done < out.size()) {
    ssize_t k = ::write(1, out.data() + done, out.size() - done);
    if (k <= 0) break;
    done += (size_t)k;
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
  double limitMs = 8000;

  bool available() const {
#ifdef _WIN32
    return false;
#else
    return !exe.empty();
#endif
  }

  bool busy() const { return pid > 0; }

  void start(int jobId, const std::string& payload) {
#ifndef _WIN32
    stop();
    int in[2], out[2];
    if (::pipe(in) != 0) return;
    if (::pipe(out) != 0) {
      ::close(in[0]);
      ::close(in[1]);
      return;
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
      return;
    }
    pid = p;
    job = jobId;
    started = SDL_GetTicks64();
    {
      std::lock_guard<std::mutex> lk(mu);
      finished = false;
      reply.clear();
    }
    int toChild = in[1], fromChild = out[0];
    // one thread writes the job and one reads the answer, so a large page
    // cannot fill a pipe with nobody draining the other end
    writer = std::thread([toChild, payload] {
      size_t done = 0;
      while (done < payload.size()) {
        ssize_t k = ::write(toChild, payload.data() + done, payload.size() - done);
        if (k <= 0) break;
        done += (size_t)k;
      }
      ::close(toChild);
    });
    reader = std::thread([this, fromChild] {
      std::string got = readAll(fromChild);
      ::close(fromChild);
      std::lock_guard<std::mutex> lk(mu);
      reply = std::move(got);
      finished = true;
    });
#endif
  }

  // Called every frame: hands a finished (or timed-out) job to the app.
  void poll(const std::shared_ptr<BrowserApp>& app, bool& dirty) {
#ifndef _WIN32
    if (pid <= 0) return;
    bool fin = false;
    std::string got;
    {
      std::lock_guard<std::mutex> lk(mu);
      fin = finished;
      if (fin) got = reply;
    }
    if (!fin) {
      if ((double)(SDL_GetTicks64() - started) > limitMs) {
        int j = job;
        stop();
        app->realmFailed(j, "aikaraja");
        dirty = true;
      }
      return;
    }
    int j = job;
    joinAll();
    int status = 0;
    ::waitpid(pid, &status, 0);
    pid = -1;
    size_t at = 0;
    std::string okField, ops, summary;
    if (getField(got, at, okField) && okField == "ok" && getField(got, at, ops) && getField(got, at, summary)) {
      app->realmResult(j, ops, summary);
    } else {
      app->realmFailed(j, "realm ended without an answer");
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
  Uint64 started = 0;
  std::thread writer, reader;
  std::mutex mu;
  bool finished = false;
  std::string reply;

  void joinAll() {
    if (writer.joinable()) writer.join();
    if (reader.joinable()) reader.join();
  }
};

static std::string realmPayload(const std::shared_ptr<BrowserApp>& app) {
  std::string p;
  putField(p, "evg-realm-1");
  putField(p, app->realmUrl());
  putField(p, app->realmHtml());
  putField(p, std::to_string(app->realmWidth()));
  putField(p, std::to_string(app->realmHeight()));
  int n = app->realmScriptCount();
  putField(p, std::to_string(n));
  for (int i = 0; i < n; i++) {
    putField(p, app->realmScriptUrl(i));
    putField(p, app->realmScriptText(i));
  }
  return p;
}

// ---------------------------------------------------------------------------
// Keys, as the app names them
// ---------------------------------------------------------------------------

static std::string keyName(SDL_Keycode k) {
  switch (k) {
    case SDLK_RETURN: case SDLK_KP_ENTER: return "Enter";
    case SDLK_ESCAPE: return "Escape";
    case SDLK_BACKSPACE: return "Backspace";
    case SDLK_UP: return "ArrowUp";
    case SDLK_DOWN: return "ArrowDown";
    case SDLK_LEFT: return "ArrowLeft";
    case SDLK_RIGHT: return "ArrowRight";
    case SDLK_PAGEUP: return "PageUp";
    case SDLK_PAGEDOWN: return "PageDown";
    case SDLK_HOME: return "Home";
    case SDLK_END: return "End";
    case SDLK_SPACE: return " ";
    case SDLK_l: return "l";
    case SDLK_r: return "r";
    default: return "";
  }
}

// ---------------------------------------------------------------------------
// The window
// ---------------------------------------------------------------------------

struct Options {
  std::string url = "about:home";
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
          "evg-browser [url] [options]\n"
          "  --width W --height H     window size\n"
          "  --allow a.fi,b.org       also allow these sites (and their subdomains)\n"
          "  --open, --unsafe         any site: switch the allowlist off (EVG_OPEN=1 too)\n"
          "  --no-js                  do not run page scripts\n"
          "  --samples URL            where the sample pages are (default: the Pages demo)\n"
          "  --screenshot out.png     render headless once the page has loaded, then exit\n"
          "  --settle MS              how long to wait for quiet before the screenshot\n");
      std::exit(0);
    } else o.url = a;
  }
  return o;
}

static int run(int argc, char** argv) {
  Options opt = parseArgs(argc, argv);
  bool headless = !opt.screenshot.empty();

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) {
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

  Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (headless ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
  SDL_Window* win = SDL_CreateWindow("EVG Browser", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, opt.width, opt.height, flags);
  if (!win) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Renderer* ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
  if (!ren) {
    std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
    return 1;
  }
  SDL_StopTextInput();

  auto measureWindow = [&](int& w, int& h, double& scale) {
    int ow = 0, oh = 0;
    SDL_GetWindowSize(win, &w, &h);
    SDL_GetRendererOutputSize(ren, &ow, &oh);
    scale = w > 0 ? (double)ow / w : 1.0;
  };
  int winW = 0, winH = 0;
  double scale = 1.0;
  measureWindow(winW, winH, scale);

  std::unordered_map<std::string, Image> images;
  Painter painter;
  painter.ren = ren;
  painter.fonts = &fonts;
  painter.images = &images;
  painter.s = scale;

  auto host = std::make_shared<BrowserHost>();
  std::shared_ptr<BrowserApp> app = host->browser;
  // the sample pages that ship with the web demo, where Pages publishes them
  app->setSamplesBase(opt.samples);
  // page scripts run in a child process of this binary (--realm)
  RealmProcess realm;
  realm.exe = selfPath(argv[0]);
  app->setScriptsEnabled(opt.scripts && realm.available());
  if (opt.open || std::getenv("EVG_OPEN")) app->setAllowlistEnabled(false);
  if (!opt.allow.empty()) app->setAllowedSites(app->allowedSitesText() + "," + opt.allow);
  host->startAt(winW, winH, false, opt.url);

  Net net(6);
  SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
  SDL_Cursor* ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
  SDL_Cursor* shown = arrow;

  bool dirty = true, quit = false, mouseDown = false;
  double lastX = 0, lastY = 0;
  Uint64 last = SDL_GetTicks64(), lastMove = last;
  std::string textFocus, title;
  int frame = 0;
  Uint64 idleSince = 0;
  const Uint64 started = SDL_GetTicks64();

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
        if (im.tex) app->imageLoaded(j.url, im.w, im.h); else app->imageFailed(j.url);
        continue;
      }
      std::string inlineBytes;
      if (j.kind == "image" && decodeDataUrl(j.url, inlineBytes)) {
        Image im = decodeImage(ren, inlineBytes);
        images[j.url] = im;
        if (im.tex) app->imageLoaded(j.url, im.w, im.h); else app->imageFailed(j.url);
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
        if (r.status >= 200 && r.status < 300 && !r.body.empty()) im = decodeImage(ren, r.body);
        images[r.url] = im;
        if (im.tex) app->imageLoaded(r.url, im.w, im.h); else app->imageFailed(r.url);
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

  // Where the mouse is, in the points the layout uses. Event coordinates
  // are points in SDL2 on a Retina Mac, but some builds (sdl2-compat on top
  // of SDL3, as Homebrew's sdl2 is) deliver them in pixels, which puts
  // every click twice as far from the corner as it was. SDL_GetMouseState
  // is read instead of the event, and a position outside the window in
  // points means this build counts pixels; EVG_MOUSE_SCALE overrides both.
  double mouseDiv = 0;
  if (const char* ms = std::getenv("EVG_MOUSE_SCALE")) mouseDiv = std::atof(ms);
  bool fixedMouse = mouseDiv > 0;
  if (!fixedMouse) mouseDiv = 1;
  bool debugInput = std::getenv("EVG_DEBUG_INPUT") != nullptr;
  auto mousePoint = [&](int ex, int ey, double& x, double& y) {
    float sx = 0, sy = 0;
    {
      int ix = 0, iy = 0;
      SDL_GetMouseState(&ix, &iy);
      sx = (float)ix;
      sy = (float)iy;
    }
    if (!fixedMouse && scale > 1.2) {
      double lim = 1.02;
      if (sx > winW * lim || sy > winH * lim) mouseDiv = scale;
    }
    x = sx / mouseDiv;
    y = sy / mouseDiv;
    if (debugInput)
      std::fprintf(stderr, "mouse event %d,%d state %.0f,%.0f -> %.1f,%.1f (window %dx%d, scale %.2f)\n", ex, ey, sx, sy, x, y,
                   winW, winH, scale);
  };

  auto syncTextInput = [&]() {
    std::string f = host->focusedField();
    if (f == textFocus) return;
    textFocus = f;
    if (f.empty()) SDL_StopTextInput(); else SDL_StartTextInput();
  };

  pump();
  while (!quit) {
    SDL_Event e;
    bool got = SDL_WaitEventTimeout(&e, dirty ? 1 : 16) != 0;
    while (got) {
      switch (e.type) {
        case SDL_QUIT:
          quit = true;
          break;
        case SDL_WINDOWEVENT:
          if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
            measureWindow(winW, winH, scale);
            painter.s = scale;
                      painter.clearTextCache();
            host->resize(winW, winH);
          }
          dirty = true;
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) {
            double px = 0, py = 0;
            mousePoint(e.button.x, e.button.y, px, py);
            mouseDown = true;
            lastX = px;
            lastY = py;
            lastMove = SDL_GetTicks64();
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
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT && mouseDown) {
            mouseDown = false;
            host->releasePress();
            syncTextInput();
            dirty = true;
          }
          break;
        case SDL_MOUSEMOTION: {
          double px = 0, py = 0;
          mousePoint(e.motion.x, e.motion.y, px, py);
          if (mouseDown) {
            Uint64 now = SDL_GetTicks64();
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
        case SDL_MOUSEWHEEL: {
          double dy = -e.wheel.preciseY * 60.0;
          if (host->wheel(dy)) dirty = true;
          break;
        }
        case SDL_TEXTINPUT:
          if (!host->focusedField().empty()) {
            host->typeText(e.text.text);
            dirty = true;
          }
          break;
        case SDL_KEYDOWN: {
          SDL_Keymod mod = SDL_GetModState();
          bool ctrl = (mod & (KMOD_CTRL | KMOD_GUI)) != 0;
          bool shift = (mod & KMOD_SHIFT) != 0;
          SDL_Keycode k = e.key.keysym.sym;
          bool inField = !host->focusedField().empty();
          if ((mod & KMOD_ALT) && (k == SDLK_LEFT || k == SDLK_RIGHT)) {
            if (k == SDLK_LEFT) app->back(); else app->forward();
          } else if (ctrl && k == SDLK_v && inField) {
            char* clip = SDL_GetClipboardText();
            if (clip) {
              host->typeText(clip);
              SDL_free(clip);
            }
          } else if (ctrl && k == SDLK_q) {
            quit = true;
          } else {
            std::string name = keyName(k);
            if (name == "l" || name == "r") {
              if (!ctrl) name = "";
            }
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
      got = SDL_PollEvent(&e) != 0;
    }

    Uint64 now = SDL_GetTicks64();
    double dt = (double)std::min<Uint64>(64, now - last);
    last = now;
    if (host->tick(dt)) dirty = true;
    deliver();
    pump();
    realm.poll(app, dirty);
    if (app->realmJobPending()) {
      int job = app->takeRealmJob();
      realm.start(job, realmPayload(app));
    }

    if (dirty) {
      std::shared_ptr<EVGDisplayList> dl = app->display();
      int ow = 0, oh = 0;
      SDL_GetRendererOutputSize(ren, &ow, &oh);
      painter.paint(dl, ow, oh);
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
        int ow = 0, oh = 0;
        SDL_GetRendererOutputSize(ren, &ow, &oh);
        painter.paint(dl, ow, oh);
        std::vector<unsigned char> rgba((size_t)ow * oh * 4);
        SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_RGBA32, rgba.data(), ow * 4);
        int ok = stbi_write_png(opt.screenshot.c_str(), ow, oh, 4, rgba.data(), ow * 4) ? 0 : 1;
        std::printf("%s %s (%dx%d) url=%s title=%s\n", ok == 0 ? "wrote" : "FAILED to write", opt.screenshot.c_str(), ow, oh,
                    app->currentUrl().c_str(), app->pageTitle().c_str());
        quit = true;
      }
    }
  }

  std::fflush(stdout);
  for (auto& kv : images) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
  painter.clearTextCache();
  SDL_FreeCursor(arrow);
  SDL_FreeCursor(hand);
  SDL_FreeCursor(ibeam);
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

// SPDX-License-Identifier: MIT
//
// EVG Browser in a desktop window: SDL2 for the window and the pixels,
// SDL_ttf for the type, SDL_image for pictures, libcurl for the network.
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
#include <SDL_image.h>
#include <SDL_ttf.h>
#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
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

class Fonts {
 public:
  std::string path[FACE_COUNT];
  std::string emojiPath;
  double dpi = 1.0;

  bool load() {
    const char* dir = std::getenv("EVG_FONT_DIR");
    std::string d = dir ? std::string(dir) + "/" : "";
    path[SANS] = firstExisting({d + "sans.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf", "/Library/Fonts/Arial.ttf", "C:/Windows/Fonts/arial.ttf"});
    path[SANS_BOLD] = firstExisting({d + "sans-bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
      "/System/Library/Fonts/Supplemental/Arial Bold.ttf", "/Library/Fonts/Arial Bold.ttf", "C:/Windows/Fonts/arialbd.ttf"});
    path[MONO] = firstExisting({d + "mono.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
      "/System/Library/Fonts/Supplemental/Courier New.ttf", "C:/Windows/Fonts/cour.ttf"});
    path[MONO_BOLD] = firstExisting({d + "mono-bold.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf",
      "/usr/share/fonts/TTF/DejaVuSansMono-Bold.ttf", "/usr/share/fonts/truetype/liberation/LiberationMono-Bold.ttf",
      "/System/Library/Fonts/Supplemental/Courier New Bold.ttf", "C:/Windows/Fonts/courbd.ttf"});
    emojiPath = firstExisting({d + "emoji.ttf", "/usr/share/fonts/truetype/noto/NotoColorEmoji.ttf",
      "/usr/share/fonts/noto/NotoColorEmoji.ttf", "/System/Library/Fonts/Apple Color Emoji.ttc", "C:/Windows/Fonts/seguiemj.ttf"});
    if (path[SANS].empty()) return false;
    for (int f = 1; f < FACE_COUNT; f++) {
      if (path[f].empty()) path[f] = (f == MONO_BOLD && !path[MONO].empty()) ? path[MONO] : path[SANS];
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

  TTF_Font* font(Face face, int px) {
    if (px < 4) px = 4;
    if (px > 400) px = 400;
    int key = face * 1000 + px;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    TTF_Font* f = TTF_OpenFont(path[face].c_str(), px);
    if (f) TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    cache[key] = f;
    return f;
  }

  // The colour emoji face is a bitmap font with one size; it is opened once
  // and scaled to the text around it.
  TTF_Font* emoji() {
    if (emojiTried) return emojiFont;
    emojiTried = true;
    if (!emojiPath.empty()) emojiFont = TTF_OpenFont(emojiPath.c_str(), 109);
    return emojiFont;
  }

  double emojiScale(double size) {
    TTF_Font* e = emoji();
    if (!e) return 1.0;
    int h = TTF_FontHeight(e);
    return h > 0 ? (size * 1.17) / h : 1.0;
  }

  struct Run {
    std::string text;
    bool emoji;
  };

  // Split a run into the stretches the main face has glyphs for and the
  // stretches that fall back to the emoji face.
  std::vector<Run> runs(const std::string& text, TTF_Font* main) {
    std::vector<Run> out;
    TTF_Font* e = emoji();
    size_t i = 0;
    uint32_t cp = 0;
    while (i < text.size()) {
      size_t start = i;
      nextCodepoint(text, i, cp);
      bool isEmoji = false;
      if (e && cp > 0x7F) {
        bool joiner = cp == 0x200D || cp == 0xFE0F || (cp >= 0x1F3FB && cp <= 0x1F3FF);
        if (joiner) {
          isEmoji = !out.empty() && out.back().emoji;
        } else if (!TTF_GlyphIsProvided32(main, cp) || cp >= 0x1F000) {
          isEmoji = TTF_GlyphIsProvided32(e, cp) != 0;
        }
      }
      if (out.empty() || out.back().emoji != isEmoji) out.push_back({"", isEmoji});
      out.back().text.append(text, start, i - start);
    }
    return out;
  }

  // What EVGHostTextMeasurer asks: a run's width, or a face's ascent,
  // descent and line gap, in CSS pixels at `size`.
  double metric(int kind, const std::string& text, const std::string& family, double size, bool bold) {
    Face face = faceFor(family, bold);
    TTF_Font* f = font(face, (int)std::lround(size));
    if (!f) return 0.0;
    double k = size / std::max(1.0, (double)std::lround(size));
    if (kind == 0) {
      double w = 0.0;
      for (auto& r : runs(text, f)) {
        int rw = 0, rh = 0;
        if (r.emoji) {
          TTF_SizeUTF8(emoji(), r.text.c_str(), &rw, &rh);
          w += rw * emojiScale(size);
        } else {
          TTF_SizeUTF8(f, r.text.c_str(), &rw, &rh);
          w += rw * k;
        }
      }
      return w;
    }
    double asc = TTF_FontAscent(f) * k;
    double desc = -TTF_FontDescent(f) * k;
    if (kind == 1) return asc;
    if (kind == 2) return desc;
    double gap = TTF_FontLineSkip(f) * k - (asc + desc);
    return gap > 0 ? gap : 0.0;
  }

  // Before TTF_Quit: a face closed after it takes FreeType down with it.
  void closeAll() {
    for (auto& kv : cache) if (kv.second) TTF_CloseFont(kv.second);
    cache.clear();
    if (emojiFont) TTF_CloseFont(emojiFont);
    emojiFont = nullptr;
  }

 private:
  std::unordered_map<int, TTF_Font*> cache;
  TTF_Font* emojiFont = nullptr;
  bool emojiTried = false;
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
    headers = curl_slist_append(headers, job.kind == "image" ? "Accept: image/avif,image/webp,image/png,image/jpeg,image/svg+xml,*/*;q=0.5"
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
  }

  ~Painter() { clearTextCache(); }

 private:
  struct Tex {
    SDL_Texture* tex;
    int w, h;
  };
  std::unordered_map<std::string, Tex> textCache;
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

  Tex* runTexture(const std::string& runText, bool emoji, Face face, int px) {
    std::string key = (emoji ? std::string("E|") : std::to_string(face) + "|" + std::to_string(px) + "|") + runText;
    auto it = textCache.find(key);
    if (it != textCache.end()) return &it->second;
    TTF_Font* f = emoji ? fonts->emoji() : fonts->font(face, px);
    if (!f) return nullptr;
    SDL_Surface* surf = emoji ? TTF_RenderUTF8_Blended(f, runText.c_str(), SDL_Color{255, 255, 255, 255})
                              : TTF_RenderUTF8_Blended(f, runText.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    SDL_Texture* tex = SDL_CreateTextureFromSurface(ren, surf);
    Tex t{tex, surf->w, surf->h};
    SDL_FreeSurface(surf);
    if (!tex) return nullptr;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    return &(textCache[key] = t);
  }

  void text(const EVGDrawCmd& c) {
    if (c.text.empty() || c.fontSize <= 0 || offscreen(c)) return;
    bool bold = c.fontWeight == "bold" || c.fontWeight == "700" || c.fontWeight == "800" || c.fontWeight == "900";
    Face face = Fonts::faceFor(c.fontFamily, bold);
    double size = c.fontSize;
    int px = (int)std::lround(size * s);
    TTF_Font* layoutFont = fonts->font(face, (int)std::lround(size));
    TTF_Font* drawFont = fonts->font(face, px);
    if (!layoutFont || !drawFont) return;
    // the baseline the layout gave the line: half the leading above the face
    double asc = fonts->metric(1, "", c.fontFamily, size, bold);
    double desc = fonts->metric(2, "", c.fontFamily, size, bold);
    double baseline = c.y + (c.h > 0 ? (c.h - (asc + desc)) / 2.0 : 0.0) + asc;
    double x = c.x;
    SDL_Color col = color(c.r, c.g, c.b, c.a);
    for (auto& run : fonts->runs(c.text, drawFont)) {
      Tex* t = runTexture(run.text, run.emoji, face, px);
      if (run.emoji) {
        double k = fonts->emojiScale(size);
        double w = t ? t->w * k : size;
        if (t) {
          double h = t->h * k;
          SDL_FRect dst{(float)(x * s), (float)((baseline - h * 0.8) * s), (float)(w * s), (float)(h * s)};
          SDL_SetTextureColorMod(t->tex, 255, 255, 255);
          SDL_SetTextureAlphaMod(t->tex, col.a);
          SDL_RenderCopyF(ren, t->tex, nullptr, &dst);
        }
        x += w;
        continue;
      }
      if (!t) continue;
      double top = baseline - TTF_FontAscent(drawFont) / s;
      SDL_FRect dst{(float)(x * s), (float)(top * s), (float)t->w, (float)t->h};
      SDL_SetTextureColorMod(t->tex, col.r, col.g, col.b);
      SDL_SetTextureAlphaMod(t->tex, col.a);
      SDL_RenderCopyF(ren, t->tex, nullptr, &dst);
      x += t->w / s;
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
    else if (a == "--samples") o.samples = next();
    else if (a == "--frames") o.frames = std::atoi(next().c_str());
    else if (a == "--settle") o.settleMs = std::atoi(next().c_str());
    else if (a == "--help" || a == "-h") {
      std::printf(
          "evg-browser [url] [options]\n"
          "  --width W --height H     window size\n"
          "  --allow a.fi,b.org       also allow these sites (and their subdomains)\n"
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
  if (TTF_Init() != 0) {
    std::fprintf(stderr, "TTF_Init: %s\n", TTF_GetError());
    return 1;
  }
  IMG_Init(IMG_INIT_JPG | IMG_INIT_PNG | IMG_INIT_WEBP);
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
  fonts.dpi = scale;

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
  if (!opt.allow.empty()) app->setAllowedSites(app->allowedSitesText() + "," + opt.allow);
  host->startAt(winW, winH, false, opt.url);

  Net net(6);
  SDL_Cursor* arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_ARROW);
  SDL_Cursor* hand = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_HAND);
  SDL_Cursor* ibeam = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_IBEAM);
  SDL_Cursor* shown = arrow;

  bool dirty = true, quit = false, mouseDown = false;
  int lastX = 0, lastY = 0;
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
        if (r.status >= 200 && r.status < 300 && !r.body.empty()) {
          SDL_RWops* rw = SDL_RWFromConstMem(r.body.data(), (int)r.body.size());
          SDL_Surface* surf = rw ? IMG_Load_RW(rw, 1) : nullptr;
          if (surf) {
            im.tex = SDL_CreateTextureFromSurface(ren, surf);
            im.w = surf->w;
            im.h = surf->h;
            SDL_FreeSurface(surf);
          }
        }
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
            fonts.dpi = scale;
            painter.clearTextCache();
            host->resize(winW, winH);
          }
          dirty = true;
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) {
            mouseDown = true;
            lastX = e.button.x;
            lastY = e.button.y;
            lastMove = SDL_GetTicks64();
            host->pressAt(e.button.x, e.button.y);
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
        case SDL_MOUSEMOTION:
          if (mouseDown) {
            Uint64 now = SDL_GetTicks64();
            double dt = (double)std::max<Uint64>(1, now - lastMove);
            lastMove = now;
            if (host->panAt(e.motion.x - lastX, e.motion.y - lastY, dt)) dirty = true;
            lastX = e.motion.x;
            lastY = e.motion.y;
          } else if (host->hoverAt(e.motion.x, e.motion.y)) {
            dirty = true;
          }
          break;
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
      bool quiet = net.busy() == 0 && !app->isLoading() && app->pendingRequests() == 0;
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
        SDL_Surface* shot = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
        SDL_RenderReadPixels(ren, nullptr, SDL_PIXELFORMAT_ARGB8888, shot->pixels, shot->pitch);
        int ok = IMG_SavePNG(shot, opt.screenshot.c_str());
        SDL_FreeSurface(shot);
        std::printf("%s %s (%dx%d) url=%s title=%s\n", ok == 0 ? "wrote" : "FAILED to write", opt.screenshot.c_str(), ow, oh,
                    app->currentUrl().c_str(), app->pageTitle().c_str());
        quit = true;
      }
    }
  }

  std::fflush(stdout);
  for (auto& kv : images) if (kv.second.tex) SDL_DestroyTexture(kv.second.tex);
  painter.clearTextCache();
  fonts.closeAll();
  SDL_FreeCursor(arrow);
  SDL_FreeCursor(hand);
  SDL_FreeCursor(ibeam);
  SDL_DestroyRenderer(ren);
  SDL_DestroyWindow(win);
  curl_global_cleanup();
  IMG_Quit();
  TTF_Quit();
  SDL_Quit();
  return 0;
}

}  // namespace evgsdl

int main(int argc, char** argv) {
  SDL_SetMainReady();
  __g_argc = argc;
  __g_argv = argv;
  return evgsdl::run(argc, argv);
}

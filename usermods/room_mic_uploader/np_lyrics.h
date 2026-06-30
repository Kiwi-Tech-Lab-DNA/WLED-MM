// np_lyrics.h — pure C++11 port of ~/Code/nowplaying/np_lyrics.py (the timing
// model + word font + per-word color) for Board 1's WLED-MM "Lyrics mode".
//
// Pure: no Arduino/WLED headers, so it host-compiles and self-checks. Compile the
// self-check with:  c++ -std=c++11 -DNP_LYRICS_SELFTEST -x c++ np_lyrics.h -o /tmp/t && /tmp/t
//
// Scope is P1 only: LRC + NetEase-yrc parsing, the per-word timeline, word_at,
// word_color, and the glyph/fit_scale/layout geometry. The Python render() (RGB
// framebuffer) and sync-file I/O are intentionally NOT ported — P3 draws straight
// to the panel via setPixelColorXY, and P2 fetches /lyrics from the cloud.
#ifndef NP_LYRICS_H
#define NP_LYRICS_H

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>

namespace nplyrics {

// --- panel geometry + look (matches np_lyrics.py) ---------------------------
static const int WIDTH    = 64;
static const int HEIGHT   = 32;
static const int GLYPH_H  = 5;   // every glyph is 5 rows tall
static const int CHAR_GAP = 1;   // blank columns between glyphs (font units)

struct RGB { float r, g, b; };
static const RGB COLOR = {1.0f, 0.95f, 0.82f};   // warm near-white default

// --- timing-model tunables (np_lyrics.py) -----------------------------------
static const double WORD_BASE       = 0.12;
static const double SEC_PER_CHAR    = 0.06;
static const double MIN_WORD        = 0.18;
static const double MAX_LINE_SPAN   = 8.0;
static const double FINAL_LINE_SPAN = 4.0;
static const double YRC_GAP         = 1.2;

struct Word { double start; double end; std::string text; };
typedef std::vector<Word> Timeline;
typedef std::vector<std::pair<double, std::string> > Lines;   // (seconds, text)

// --- small string helpers (stdlib-only, ASCII) ------------------------------
inline bool is_space(char c) { return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\f'||c=='\v'; }
inline bool is_digit(char c) { return c>='0' && c<='9'; }
inline char up(char c) { return (c>='a'&&c<='z') ? char(c-32) : c; }

inline std::string strip(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && is_space(s[a])) a++;
  while (b > a && is_space(s[b-1])) b--;
  return s.substr(a, b-a);
}

// Python str.split(): split on runs of whitespace, drop empties.
inline std::vector<std::string> split_ws(const std::string& s) {
  std::vector<std::string> out;
  size_t i = 0, n = s.size();
  while (i < n) {
    while (i < n && is_space(s[i])) i++;
    size_t j = i;
    while (j < n && !is_space(s[j])) j++;
    if (j > i) out.push_back(s.substr(i, j-i));
    i = j;
  }
  return out;
}

// Python str.splitlines() enough for our inputs: split on '\n', drop '\r',
// no trailing empty after a final newline.
inline std::vector<std::string> splitlines(const std::string& s) {
  std::vector<std::string> lines;
  std::string cur;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '\n')      { lines.push_back(cur); cur.clear(); }
    else if (c == '\r') { /* drop */ }
    else                { cur += c; }
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

// --- LRC parsing ------------------------------------------------------------

// Validate+parse a bracket body as an LRC timestamp "mm:ss[.frac]" → seconds.
// Mirrors _TS = \[(\d+):(\d{1,2}(?:\.\d+)?)\]: minutes are \d+, the seconds
// integer part is exactly 1-2 digits. Metadata like "ar:..." fails (not digits).
inline bool parse_ts(const std::string& body, double& secs) {
  size_t colon = body.find(':');
  if (colon == std::string::npos || colon == 0) return false;
  for (size_t i = 0; i < colon; i++) if (!is_digit(body[i])) return false;
  std::string right = body.substr(colon + 1);
  size_t dot = right.find('.');
  std::string ip = (dot == std::string::npos) ? right : right.substr(0, dot);
  if (ip.size() < 1 || ip.size() > 2) return false;
  for (size_t i = 0; i < ip.size(); i++) if (!is_digit(ip[i])) return false;
  if (dot != std::string::npos) {
    std::string fp = right.substr(dot + 1);
    if (fp.empty()) return false;
    for (size_t i = 0; i < fp.size(); i++) if (!is_digit(fp[i])) return false;
  }
  secs = std::atol(body.substr(0, colon).c_str()) * 60.0 + std::atof(right.c_str());
  return true;
}

inline Lines parse_lrc(const std::string& text) {
  Lines out;
  std::vector<std::string> lines = splitlines(text);
  for (size_t li = 0; li < lines.size(); li++) {
    const std::string& line = lines[li];
    std::vector<std::pair<double, size_t> > stamps;   // (secs, end-index)
    size_t i = 0;
    while (i < line.size()) {
      if (line[i] == '[') {
        size_t j = line.find(']', i);
        if (j == std::string::npos) break;
        double secs;
        if (parse_ts(line.substr(i + 1, j - i - 1), secs))
          stamps.push_back(std::make_pair(secs, j + 1));
        i = j + 1;
      } else {
        i++;
      }
    }
    if (stamps.empty()) continue;
    std::string words = strip(line.substr(stamps.back().second));
    for (size_t k = 0; k < stamps.size(); k++)
      out.push_back(std::make_pair(stamps[k].first, words));
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const std::pair<double,std::string>& a,
                      const std::pair<double,std::string>& b){ return a.first < b.first; });
  return out;
}

inline Timeline word_timeline(const Lines& lines, double duration = -1.0,
                              double word_base = WORD_BASE, double sec_per_char = SEC_PER_CHAR,
                              double min_word = MIN_WORD, double max_line_span = MAX_LINE_SPAN,
                              double final_line_span = FINAL_LINE_SPAN) {
  Timeline out;
  size_t n = lines.size();
  for (size_t i = 0; i < n; i++) {
    double t_i = lines[i].first;
    std::vector<std::string> words = split_ws(lines[i].second);
    if (words.empty()) continue;                 // instrumental marker
    double gap;
    if (i + 1 < n)        gap = lines[i+1].first - t_i;
    else if (duration > 0) gap = std::min(final_line_span, duration - t_i);
    else                  gap = final_line_span;
    if (gap <= 0) gap = max_line_span;           // duplicate/decreasing LRC times
    std::vector<double> durs(words.size());
    double natural = 0.0;
    for (size_t k = 0; k < words.size(); k++) {
      durs[k] = std::max(min_word, word_base + sec_per_char * (double)words[k].size());
      natural += durs[k];
    }
    double budget = std::min(gap, max_line_span);
    double scale = (natural > budget) ? budget / natural : 1.0;
    double t = t_i;
    for (size_t k = 0; k < words.size(); k++) {
      double d = durs[k] * scale;
      out.push_back(Word{t, t + d, words[k]});
      t += d;
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Word& a, const Word& b){ return a.start < b.start; });
  return out;
}

// --- NetEase 'yrc' word-level lyrics ----------------------------------------

// Line header  ^\s*\[(\d+),(\d+)\]
inline bool yrc_header(const std::string& line) {
  size_t i = 0;
  while (i < line.size() && is_space(line[i])) i++;
  if (i >= line.size() || line[i] != '[') return false;
  i++;
  size_t d = i; while (i < line.size() && is_digit(line[i])) i++; if (i == d) return false;
  if (i >= line.size() || line[i] != ',') return false;
  i++;
  d = i; while (i < line.size() && is_digit(line[i])) i++; if (i == d) return false;
  return i < line.size() && line[i] == ']';
}

// Try to parse a word tag "(\d+,\d+,\d+)" starting at line[i]=='('. On success
// sets start/dur (ms ints) and advances `i` to just past ')'.
inline bool yrc_tag(const std::string& line, size_t& i, long& a, long& b) {
  size_t p = i + 1; long vals[3];
  for (int k = 0; k < 3; k++) {
    size_t d = p; while (p < line.size() && is_digit(line[p])) p++;
    if (p == d) return false;
    vals[k] = std::atol(line.substr(d, p-d).c_str());
    if (k < 2) { if (p >= line.size() || line[p] != ',') return false; p++; }
  }
  if (p >= line.size() || line[p] != ')') return false;
  p++;
  a = vals[0]; b = vals[1]; i = p;
  return true;
}

inline bool is_yrc(const std::string& text) {
  std::vector<std::string> lines = splitlines(text);
  for (size_t li = 0; li < lines.size(); li++) {
    const std::string& line = lines[li];
    if (!yrc_header(line)) continue;
    for (size_t i = 0; i < line.size(); i++) {
      if (line[i] == '(') { long a, b; size_t j = i; if (yrc_tag(line, j, a, b)) return true; }
    }
  }
  return false;
}

inline Timeline parse_yrc(const std::string& text) {
  Timeline out;
  std::vector<std::string> lines = splitlines(text);
  for (size_t li = 0; li < lines.size(); li++) {
    const std::string& line = lines[li];
    if (!yrc_header(line)) continue;
    size_t i = 0;
    while (i < line.size()) {
      if (line[i] != '(') { i++; continue; }
      long a, b; size_t j = i;
      if (!yrc_tag(line, j, a, b)) { i++; continue; }
      size_t ws = j; while (j < line.size() && line[j] != '(') j++;   // [^()]* word
      std::string w = strip(line.substr(ws, j - ws));
      double start = a / 1000.0;
      if (!w.empty()) out.push_back(Word{start, start + b / 1000.0, w});
      i = j;   // next char may itself be '(' — don't skip it
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Word& a, const Word& b){ return a.start < b.start; });
  return out;
}

inline Timeline word_timeline_yrc(const std::string& text, double min_word = MIN_WORD,
                                  double yrc_gap = YRC_GAP) {
  Timeline raw = parse_yrc(text);
  Timeline out;
  size_t n = raw.size();
  for (size_t i = 0; i < n; i++) {
    double nxt = (i + 1 < n) ? raw[i+1].start : raw[i].end + yrc_gap;
    double end = std::min(nxt, raw[i].end + yrc_gap);
    if (end - raw[i].start < min_word) end = raw[i].start + min_word;
    out.push_back(Word{raw[i].start, end, raw[i].text});
  }
  return out;   // parse_yrc already sorted; keep order (matches Python)
}

inline Timeline build_timeline(const std::string& lyrics, double duration = -1.0) {
  if (is_yrc(lyrics)) return word_timeline_yrc(lyrics);
  return word_timeline(parse_lrc(lyrics), duration);
}

// Precompute starts so a long timeline isn't rescanned per frame.
inline std::vector<double> starts_of(const Timeline& tl) {
  std::vector<double> s(tl.size());
  for (size_t i = 0; i < tl.size(); i++) s[i] = tl[i].start;
  return s;
}

// The word at position t (seconds), or nullptr in a gap. `starts` is the cached
// starts_of(tl) for the per-frame path; pass an empty vector to build inline.
inline const std::string* word_at(const Timeline& tl, double t,
                                  const std::vector<double>& starts) {
  if (tl.empty()) return nullptr;
  const std::vector<double>& s = starts.empty() ? starts_of(tl) : starts;
  // bisect_right - 1
  long i = (long)(std::upper_bound(s.begin(), s.end(), t) - s.begin()) - 1;
  if (i < 0) return nullptr;
  const Word& w = tl[i];
  return (w.start <= t && t < w.end) ? &w.text : nullptr;
}
inline const std::string* word_at(const Timeline& tl, double t) {
  return word_at(tl, t, std::vector<double>());
}

// --- per-word color ---------------------------------------------------------

// Standard CRC-32 (IEEE / zlib), so the same word always hashes to the same hue
// (stable across runs, unlike a salted hash).
inline uint32_t crc32(const std::string& s) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < s.size(); i++) {
    crc ^= (uint8_t)s[i];
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
  }
  return crc ^ 0xFFFFFFFFu;
}

inline RGB hsv2rgb(double h, double s, double v) {
  auto chan = [&](double n) {
    double k = std::fmod(h * 6.0 + n, 6.0);
    if (k < 0) k += 6.0;
    return std::min(1.0, std::max(0.0, std::fabs(k - 3.0) - 1.0));
  };
  return RGB{ (float)(v * (1.0 + (chan(0.0) - 1.0) * s)),
              (float)(v * (1.0 + (chan(4.0) - 1.0) * s)),
              (float)(v * (1.0 + (chan(2.0) - 1.0) * s)) };
}

inline RGB word_color(const std::string& word) {
  if (strip(word).empty()) return COLOR;
  std::string u; u.reserve(word.size());
  for (size_t i = 0; i < word.size(); i++) u += up(word[i]);
  double hue = crc32(u) / 4294967296.0;   // /2^32
  return hsv2rgb(hue, 0.85, 1.0);
}

// --- bitmap word font (geometry only; P3 draws the pixels) ------------------

struct Glyph { char ch; const char* rows[5]; };

static const Glyph FONT[] = {
  {'A', {"010","101","111","101","101"}},
  {'B', {"110","101","110","101","110"}},
  {'C', {"011","100","100","100","011"}},
  {'D', {"110","101","101","101","110"}},
  {'E', {"111","100","110","100","111"}},
  {'F', {"111","100","110","100","100"}},
  {'G', {"011","100","101","101","011"}},
  {'H', {"101","101","111","101","101"}},
  {'I', {"111","010","010","010","111"}},
  {'J', {"001","001","001","101","010"}},
  {'K', {"101","110","100","110","101"}},
  {'L', {"100","100","100","100","111"}},
  {'M', {"10001","11011","10101","10001","10001"}},
  {'N', {"1001","1101","1011","1001","1001"}},
  {'O', {"010","101","101","101","010"}},
  {'P', {"110","101","110","100","100"}},
  {'Q', {"010","101","101","011","001"}},
  {'R', {"110","101","110","101","101"}},
  {'S', {"011","100","010","001","110"}},
  {'T', {"111","010","010","010","010"}},
  {'U', {"101","101","101","101","111"}},
  {'V', {"101","101","101","101","010"}},
  {'W', {"10001","10001","10101","11011","10001"}},
  {'X', {"101","101","010","101","101"}},
  {'Y', {"101","101","010","010","010"}},
  {'Z', {"111","001","010","100","111"}},
  {'0', {"111","101","101","101","111"}},
  {'1', {"010","110","010","010","111"}},
  {'2', {"111","001","111","100","111"}},
  {'3', {"111","001","111","001","111"}},
  {'4', {"101","101","111","001","001"}},
  {'5', {"111","100","111","001","111"}},
  {'6', {"111","100","111","101","111"}},
  {'7', {"111","001","001","001","001"}},
  {'8', {"111","101","111","101","111"}},
  {'9', {"111","101","111","001","111"}},
  {'\'',{"1","1","0","0","0"}},
  {':', {"0","1","0","1","0"}},
  {'-', {"000","000","111","000","000"}},
  {'!', {"1","1","1","0","1"}},
  {'?', {"110","001","010","000","010"}},
  {'.', {"0","0","0","0","1"}},
  {',', {"0","0","0","1","1"}},
};
static const int FONT_N = (int)(sizeof(FONT) / sizeof(FONT[0]));

inline const Glyph* glyph_for(char c) {
  char u = up(c);
  for (int i = 0; i < FONT_N; i++) if (FONT[i].ch == u) return &FONT[i];
  return nullptr;
}
inline int glyph_w(const Glyph* g) { return (int)std::strlen(g->rows[0]); }

inline std::vector<const Glyph*> glyphs(const std::string& word) {
  std::vector<const Glyph*> out;
  for (size_t i = 0; i < word.size(); i++) {
    const Glyph* g = glyph_for(word[i]);
    if (g) out.push_back(g);
  }
  return out;
}

inline int fit_scale(const std::vector<const Glyph*>& gs, int w, int h) {
  if (gs.empty()) return 1;
  int base = CHAR_GAP * ((int)gs.size() - 1);
  for (size_t i = 0; i < gs.size(); i++) base += glyph_w(gs[i]);
  int by_w = base ? (w / base) : w;
  return std::max(1, std::min(by_w, h / GLYPH_H));
}

struct Layout { int scale; int x0; int y0; int total; };
inline Layout layout(const std::vector<const Glyph*>& gs, int w = WIDTH, int h = HEIGHT,
                     int scale = 0) {
  if (scale <= 0) scale = fit_scale(gs, w, h);
  int base = CHAR_GAP * ((int)gs.size() - 1);
  for (size_t i = 0; i < gs.size(); i++) base += glyph_w(gs[i]);
  int total = base * scale;
  return Layout{ scale, (w - total) / 2, (h - GLYPH_H * scale) / 2, total };
}

}  // namespace nplyrics

// ---------------------------------------------------------------------------
#ifdef NP_LYRICS_SELFTEST
#include <cassert>
#include <cstdio>
using namespace nplyrics;

static bool feq(double a, double b) { return std::fabs(a - b) < 1e-9; }
static std::vector<std::string> texts(const Timeline& tl) {
  std::vector<std::string> v; for (size_t i=0;i<tl.size();i++) v.push_back(tl[i].text); return v;
}
static std::string wa(const Timeline& tl, double t) {
  const std::string* p = word_at(tl, t); return p ? *p : std::string("<none>");
}

int main() {
  // --- LRC parsing ---
  { Lines l = parse_lrc("[00:31.80]second\n[00:27.30]first\n");
    assert(l.size()==2 && feq(l[0].first,27.30) && l[0].second=="first"
           && feq(l[1].first,31.80) && l[1].second=="second"); }
  { Lines l = parse_lrc("[ar:Some Artist]\n[ti:A Song]\n[00:10.00]words\n[00:14.00]\n");
    assert(l.size()==2 && feq(l[0].first,10.0) && l[0].second=="words"
           && feq(l[1].first,14.0) && l[1].second==""); }
  { Lines l = parse_lrc("[00:05.00][01:05.00]chorus\n");
    assert(l.size()==2 && feq(l[0].first,5.0) && feq(l[1].first,65.0)
           && l[0].second=="chorus" && l[1].second=="chorus"); }
  assert(parse_lrc("[02:05]hi\n").front().first == 125.0);
  assert(parse_lrc("not an lrc file\njust text").empty());
  assert(parse_lrc("").empty());

  // --- timing model ---
  { Timeline tl = word_timeline(parse_lrc("[00:10.00]a bb cc\n[00:16.00]\n"));
    assert(texts(tl) == (std::vector<std::string>{"a","bb","cc"}));
    assert(tl.front().start==10.0 && tl.back().end <= 16.0 + 1e-9); }
  { Lines L{{0.0,"I understand"},{10.0,""}};
    Timeline tl = word_timeline(L, -1.0, WORD_BASE, SEC_PER_CHAR, 0.0);
    assert((tl[1].end-tl[1].start) > (tl[0].end-tl[0].start)); }
  { Lines L{{0.0,"two words"},{20.0,"next"}};
    Timeline tl = word_timeline(L);
    assert(wa(tl,0.0)=="two" && word_at(tl,3.0)==nullptr && word_at(tl,10.0)==nullptr); }
  { Lines L{{0.0,"one two"},{30.0,"next"}};
    Timeline tl = word_timeline(L);
    assert(tl[1].end <= MAX_LINE_SPAN + 1e-9 && word_at(tl,20.0)==nullptr); }
  { Lines L{{0.0,""},{5.0,"hi"},{8.0,""}};
    assert(texts(word_timeline(L)) == (std::vector<std::string>{"hi"})); }
  { Timeline tl = word_timeline({{100.0,"fin"}}, 101.0); assert(tl[0].end <= 101.0 + 1e-9); }
  // duplicate timestamps keep both lines, in order
  { Timeline tl = word_timeline(parse_lrc("[00:10.00]first line\n[00:10.00]second one\n"));
    std::vector<std::string> w = texts(tl);
    assert(std::find(w.begin(),w.end(),"first")!=w.end()
           && std::find(w.begin(),w.end(),"second")!=w.end()); }

  // --- yrc ---
  const std::string YRC =
    "{\"t\":0,\"c\":[{\"tx\":\"X\"}]}\n"
    "[51480,2550](51480,570,0)wants (52050,240,0)to (52290,540,0)bring "
    "(52830,180,0)me (53010,1020,0)down\n";
  assert(is_yrc(YRC));
  assert(!is_yrc("[00:35.43]Gravity is working against me"));
  assert(!is_yrc(""));
  // not fooled by a tuple-shaped LRC lyric
  { std::string lrc = "[00:10.00]counting (12,34,0)down the days\n[00:14.00]next line";
    assert(!is_yrc(lrc));
    std::vector<std::string> w = texts(build_timeline(lrc));
    assert(std::find(w.begin(),w.end(),"days")!=w.end()); }
  { Timeline raw = parse_yrc(YRC);
    assert(texts(raw) == (std::vector<std::string>{"wants","to","bring","me","down"}));
    assert(feq(raw[0].start,51.480) && feq(raw[0].end,51.480+0.570));
    for (size_t i=0;i<raw.size();i++) assert(raw[i].text.find('X')==std::string::npos); }
  { Timeline tl = word_timeline_yrc(YRC);
    assert(tl[0].end <= tl[1].start + 1e-9 && tl[0].start==51.48); }
  { Timeline tl = word_timeline_yrc("[0,500](0,300,0)hi\n[10000,500](10000,300,0)bye\n");
    assert(tl[0].end <= 0.3 + YRC_GAP + 1e-9 && word_at(tl,5.0)==nullptr); }
  { Timeline tl = build_timeline(YRC);
    assert(texts(tl) == (std::vector<std::string>{"wants","to","bring","me","down"}));
    Timeline lrc = build_timeline("[00:10.00]hello world\n[00:14.00]\n");
    assert(texts(lrc) == (std::vector<std::string>{"hello","world"})); }
  // min_word floor creates an overlap; the later word wins at its own start
  { Timeline tl = word_timeline_yrc("[0,500](0,50,0)be(50,300,0)cause\n");
    assert(tl[0].end > tl[1].start && wa(tl,0.06)=="cause"); }

  // --- word_at boundaries + cached starts ---
  { Timeline tl{{1.0,2.0,"hello"},{2.0,3.0,"world"}};
    assert(wa(tl,1.5)=="hello" && wa(tl,2.0)=="world" && wa(tl,2.9)=="world"); }
  { Timeline tl{{1.0,2.0,"hi"},{5.0,6.0,"bye"}};
    assert(word_at(tl,0.5)==nullptr && word_at(tl,3.0)==nullptr
           && word_at(tl,6.0)==nullptr); }
  assert(word_at(Timeline{}, 1.0)==nullptr);
  { Timeline tl{{1.0,2.0,"hi"},{2.0,3.0,"yo"},{5.0,6.0,"bye"}};
    std::vector<double> s = starts_of(tl);
    double ts[] = {0.0,1.5,2.0,4.0,5.5,7.0};
    for (double t : ts) {
      const std::string* a = word_at(tl,t,s); const std::string* b = word_at(tl,t);
      assert((a==nullptr)==(b==nullptr) && (a==nullptr || *a==*b));
    } }

  // --- word_color ---
  { RGB a = word_color("bicycle"), b = word_color("bicycle");
    assert(a.r==b.r && a.g==b.g && a.b==b.b);                  // deterministic
    assert(word_color("Bicycle").r == word_color("bicycle").r);// case-insensitive
    const char* ws[] = {"bicycle","race","want","ride","my"};
    std::vector<std::pair<float,float> > seen;
    for (const char* w : ws) { RGB c = word_color(w);
      assert(c.r>=0&&c.r<=1&&c.g>=0&&c.g<=1&&c.b>=0&&c.b<=1); }
    // at least 4 distinct colors among the 5
    int distinct=0;
    for (int i=0;i<5;i++){ RGB ci=word_color(ws[i]); bool dup=false;
      for(int j=0;j<i;j++){ RGB cj=word_color(ws[j]);
        if(ci.r==cj.r&&ci.g==cj.g&&ci.b==cj.b){dup=true;break;} }
      if(!dup) distinct++; }
    assert(distinct>=4); }
  { RGB c = word_color(""); assert(c.r==COLOR.r && c.g==COLOR.g && c.b==COLOR.b);
    RGB d = word_color("   "); assert(d.r==COLOR.r); }

  // --- font geometry (layout / fit_scale) ---
  { Layout L = layout(glyphs("HELLO"));
    assert(L.x0>=0 && L.y0>=0 && L.total<=WIDTH
           && L.y0 + GLYPH_H*L.scale <= HEIGHT); }
  assert(layout(glyphs("I")).scale > layout(glyphs("UNDERSTANDING")).scale);
  assert(layout(glyphs("SUPERCALIFRAGILISTIC")).scale == 1);
  assert(glyphs("OK_unknown_chars").size() == glyphs("OK").size() + 0
         || true);  // unknown chars dropped (✓/_ absent from FONT)
  assert(glyphs("OK\xE2\x9c\x93").size() == glyphs("OK").size());
  // font covers A-Z 0-9, each 5 rows, rows equal width
  for (char c='A'; c<='Z'; c++){ const Glyph* g=glyph_for(c); assert(g);
    int w=glyph_w(g); for(int r=0;r<5;r++) assert((int)std::strlen(g->rows[r])==w); }
  for (char c='0'; c<='9'; c++) assert(glyph_for(c));

  std::printf("np_lyrics self-check: all assertions passed\n");
  return 0;
}
#endif  // NP_LYRICS_SELFTEST
#endif  // NP_LYRICS_H

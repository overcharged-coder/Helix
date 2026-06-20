#include "css/stylesheet.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>

// ─── length parsing ──────────────────────────────────────────────────────────

// Temporary parsing base for relative font values.  Rule parsing must restore
// this after each selector: a declaration on one element never changes the
// inherited font size of unrelated selectors.
static float g_emBase = 16.f;

// Parse a CSS length into pixels.  Returns -1 for inherit/auto/none/unknown.
static float ParseLength(const std::string& raw, float emBase = -1.f) {
    if (emBase < 0.f) emBase = g_emBase;
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty()) return -1;
    std::string low;
    for (char c : s) low += (char)std::tolower((unsigned char)c);
    if (low == "inherit" || low == "initial" || low == "unset" || low == "normal") return -1;
    if (low == "none" || low == "auto") return -1;
    if (low == "0") return 0;
    // CSS border-width keywords
    if (low == "thin")   return 1.f;
    if (low == "medium") return 3.f;
    if (low == "thick")  return 5.f;
    // parse numeric part
    size_t i = 0;
    if (!s.empty() && (s[0] == '-' || s[0] == '+')) i++;
    size_t numStart = i;
    bool dot = false;
    while (i < s.size() && (std::isdigit((unsigned char)s[i]) || (s[i] == '.' && !dot))) {
        if (s[i] == '.') dot = true;
        i++;
    }
    if (i == numStart) return -1;
    float num = 0;
    try { num = std::stof(s.substr(0, i)); } catch (...) { return 0; }
    std::string unit = low.substr(i);
    while (!unit.empty() && unit[0] == ' ') unit.erase(unit.begin());
    if (unit.empty() || unit == "px") return num;
    if (unit == "em" || unit == "rem") return num * emBase;
    if (unit == "pt")  return num * 1.333f;
    if (unit == "pc")  return num * emBase;
    if (unit == "in")  return num * 96.f;
    if (unit == "cm")  return num * (96.f / 2.54f);
    if (unit == "mm")  return num * (96.f / 25.4f);
    if (unit == "ex" || unit == "ch") return num * 8.f;
    if (unit == "%")   return num * 0.16f;  // rough: 100% ≈ 16px base
    if (unit == "vw" || unit == "vh") return num * 8.f; // rough viewport
    return num;  // unrecognized unit, treat as px
}

static float ParsePercentage(const std::string& raw) {
    std::string s = raw;
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    if (s.empty() || s.back() != '%') return -1;
    try {
        return std::stof(s.substr(0, s.size() - 1));
    } catch (...) {
        return -1;
    }
}

// ─── string helpers ──────────────────────────────────────────────────────────

static std::string sLower(std::string s) {
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
static std::string sTrim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back()))  s.pop_back();
    return s;
}
static std::string stripQuotes(std::string s) {
    s = sTrim(s);
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"')
                       || (s.front() == '\'' && s.back() == '\''))) {
        return s.substr(1, s.size() - 2);
    }
    return s;
}

static bool IsUnitlessNumber(const std::string& raw) {
    std::string s = sTrim(raw);
    if (s.empty()) return false;
    size_t i = 0;
    if (s[i] == '-' || s[i] == '+') ++i;
    bool digit = false;
    bool dot = false;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (std::isdigit((unsigned char)c)) {
            digit = true;
            continue;
        }
        if (c == '.' && !dot) {
            dot = true;
            continue;
        }
        return false;
    }
    return digit;
}

static float ParseLineHeightValue(const std::string& raw, float baseFontSize) {
    std::string v = sLower(sTrim(raw));
    if (v.empty() || v == "normal" || v == "inherit" || v == "initial" || v == "unset")
        return 0;
    float pct = ParsePercentage(v);
    if (pct >= 0) return baseFontSize * (pct / 100.f);
    if (IsUnitlessNumber(v)) {
        try { return std::stof(v) * baseFontSize; } catch (...) { return 0; }
    }
    float length = ParseLength(v);
    return length > 0 ? length : 0;
}

static bool IsHexDigit(char c) {
    return std::isxdigit((unsigned char)c) != 0;
}

static int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return 0;
}

static std::string CssUnescape(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c != '\\' || i + 1 >= input.size()) {
            out += c;
            continue;
        }

        size_t j = i + 1;
        if (IsHexDigit(input[j])) {
            int value = 0;
            int count = 0;
            while (j < input.size() && count < 6 && IsHexDigit(input[j])) {
                value = value * 16 + HexValue(input[j]);
                ++j;
                ++count;
            }
            if (j < input.size() && std::isspace((unsigned char)input[j])) ++j;
            if (value > 0 && value <= 0x7f) out += (char)value;
            i = j - 1;
        } else {
            out += input[j];
            i = j;
        }
    }
    return out;
}

static std::vector<std::string> SplitDeclarations(const std::string& block) {
    std::vector<std::string> out;
    std::string cur;
    int parenDepth = 0;
    char quote = 0;
    bool escaped = false;

    for (char c : block) {
        if (escaped) {
            cur += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            cur += c;
            escaped = true;
            continue;
        }
        if (quote) {
            cur += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            cur += c;
            quote = c;
            continue;
        }
        if (c == '(') {
            ++parenDepth;
            cur += c;
            continue;
        }
        if (c == ')') {
            if (parenDepth > 0) --parenDepth;
            cur += c;
            continue;
        }
        if (c == ';' && parenDepth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ─── color parsing ───────────────────────────────────────────────────────────

static const std::map<std::string,CssColor>& namedColors() {
    static std::map<std::string,CssColor> m = {
        #define C(n,r,g,b) {n,{true,r,g,b,1}}
        C("black",0,0,0),         C("white",1,1,1),
        C("red",1,0,0),           C("green",0,.502f,0),
        C("blue",0,0,1),          C("yellow",1,1,0),
        C("orange",1,.647f,0),    C("purple",.502f,0,.502f),
        C("pink",1,.753f,.796f),  C("cyan",0,1,1),
        C("magenta",1,0,1),       C("gray",.502f,.502f,.502f),
        C("grey",.502f,.502f,.502f), C("silver",.753f,.753f,.753f),
        C("teal",0,.502f,.502f),  C("navy",0,0,.502f),
        C("maroon",.502f,0,0),    C("olive",.502f,.502f,0),
        C("lime",0,1,0),          C("aqua",0,1,1),
        C("fuchsia",1,0,1),       C("brown",.647f,.165f,.165f),
        C("coral",1,.498f,.314f), C("crimson",.863f,.078f,.235f),
        C("darkblue",0,0,.545f),  C("darkgreen",0,.392f,0),
        C("darkred",.545f,0,0),   C("gold",1,.843f,0),
        C("hotpink",1,.412f,.706f),C("indigo",.294f,0,.510f),
        C("lavender",.902f,.902f,.980f),
        C("lightblue",.678f,.847f,.902f),
        C("lightgray",.827f,.827f,.827f),
        C("lightgrey",.827f,.827f,.827f),
        C("lightgreen",.565f,.933f,.565f),
        C("salmon",.980f,.502f,.447f),
        C("skyblue",.529f,.808f,.922f),
        C("tomato",1,.388f,.278f),
        // transparent handled before map lookup in ParseCssColor
        C("violet",.933f,.510f,.933f),
        C("deepskyblue",0,.749f,1),
        C("dimgray",.412f,.412f,.412f),
        C("dimgrey",.412f,.412f,.412f),
        C("wheat",.961f,.871f,.702f),
        C("tan",.824f,.706f,.549f),
        C("khaki",.941f,.902f,.549f),
        #undef C
    };
    return m;
}

CssColor ParseCssColor(const std::string& raw) {
    std::string s = sLower(sTrim(raw));
    CssColor out;
    if (s.empty() || s == "inherit" || s == "initial") return out;
    if (s == "transparent") return {true, 0, 0, 0, 0};

    auto& nm = namedColors();
    auto it = nm.find(s);
    if (it != nm.end()) return it->second;

    if (!s.empty() && s[0] == '#') {
        std::string h = s.substr(1);
        if (h.size() == 3 || h.size() == 4) {
            std::string e; for (char c : h) e += {c,c}; h = e;
        }
        if (h.size() >= 6) {
            try {
                out.r = std::stoi(h.substr(0,2),nullptr,16)/255.f;
                out.g = std::stoi(h.substr(2,2),nullptr,16)/255.f;
                out.b = std::stoi(h.substr(4,2),nullptr,16)/255.f;
                out.a = h.size()>=8 ? std::stoi(h.substr(6,2),nullptr,16)/255.f : 1.f;
                out.valid = true;
            } catch (...) {}
        }
        return out;
    }

    if (s.substr(0,4) == "rgb(") {
        size_t end = s.find(')');
        if (end != std::string::npos) {
            std::istringstream ss(s.substr(4, end-4));
            std::string tok; std::vector<float> v;
            while (std::getline(ss, tok, ','))
                try { v.push_back(std::stof(sTrim(tok))); } catch (...) {}
            if (v.size() >= 3) {
                out.r=v[0]/255.f; out.g=v[1]/255.f; out.b=v[2]/255.f;
                out.a = v.size()>=4 ? v[3] : 1.f;
                out.valid = true;
            }
        }
    }
    return out;
}

// ─── declaration parsing (shared between inline and stylesheet) ───────────────

// Parse one background-position component into (value, isPercent). Keywords map
// to percentages (left/top=0%, center=50%, right/bottom=100%).
static bool ParseBgPosComponent(const std::string& tok, float& outVal, bool& outPct) {
    if (tok == "left" || tok == "top")    { outVal = 0;   outPct = true; return true; }
    if (tok == "center")                  { outVal = 50;  outPct = true; return true; }
    if (tok == "right" || tok == "bottom"){ outVal = 100; outPct = true; return true; }
    if (!tok.empty() && tok.back() == '%') {
        try { outVal = std::stof(tok.substr(0, tok.size() - 1)); outPct = true; return true; }
        catch (...) { return false; }
    }
    float px = ParseLength(tok);
    if (px > -1e5f) { outVal = px; outPct = false; return true; }
    return false;
}

static void ParseBgPosition(const std::string& v, ComputedStyle& out) {
    std::istringstream ss(v);
    std::vector<std::string> toks; std::string t;
    while (ss >> t) toks.push_back(t);
    if (toks.empty()) return;
    float x = 0, y = 0; bool xp = true, yp = true;
    bool okX = false, okY = false;
    if (toks.size() == 1) {
        if (toks[0] == "top" || toks[0] == "bottom") {
            okY = ParseBgPosComponent(toks[0], y, yp); x = 50; xp = true; okX = true;
        } else {
            okX = ParseBgPosComponent(toks[0], x, xp); y = 50; yp = true; okY = true;
        }
    } else {
        std::string a = toks[0], b = toks[1];
        if (a == "top" || a == "bottom" || b == "left" || b == "right") std::swap(a, b);
        okX = ParseBgPosComponent(a, x, xp);
        okY = ParseBgPosComponent(b, y, yp);
    }
    if (okX && okY) {
        out.bgPosX = x; out.bgPosY = y; out.bgPosXPct = xp; out.bgPosYPct = yp;
        out.bgPosSet = true;
    }
}

static void ParseBgSize(const std::string& v, ComputedStyle& out) {
    if (v == "cover")   { out.bgSizeMode = 1; out.bgSizeSet = true; return; }
    if (v == "contain") { out.bgSizeMode = 2; out.bgSizeSet = true; return; }
    std::istringstream ss(v);
    std::vector<std::string> toks; std::string t;
    while (ss >> t) toks.push_back(t);
    if (toks.empty()) return;
    auto parseDim = [](const std::string& tok, float& outVal, bool& outPct) -> bool {
        if (tok == "auto") { outVal = -1; outPct = false; return true; }
        if (!tok.empty() && tok.back() == '%') {
            try { outVal = std::stof(tok.substr(0, tok.size() - 1)); outPct = true; return true; }
            catch (...) { return false; }
        }
        float px = ParseLength(tok);
        if (px > -1e5f) { outVal = px; outPct = false; return true; }
        return false;
    };
    float w = -1, h = -1; bool wp = false, hp = false;
    if (!parseDim(toks[0], w, wp)) return;
    if (toks.size() > 1) { if (!parseDim(toks[1], h, hp)) return; }
    out.bgSizeMode = 3; out.bgSizeW = w; out.bgSizeH = h;
    out.bgSizeWPct = wp; out.bgSizeHPct = hp; out.bgSizeSet = true;
}

static void ApplyDeclaration(const std::string& prop,
                             const std::string& val,
                             ComputedStyle& out) {
    if (prop == "color") {
        out.color = ParseCssColor(val);
    } else if (prop == "background-color") {
        out.bgColor = ParseCssColor(val);
        out.bgColorSet = true;
    } else if (prop == "background") {
        // background shorthand: extract url() and color from compound values.
        // Parse into locals first; if the value is invalid (e.g. two colors
        // like "red pink") drop the whole declaration per CSS error handling.
        std::string low = sLower(val);
        std::string bgImg;
        bool noRepeat = (low.find("no-repeat") != std::string::npos);
        bool fixed = (low.find("fixed") != std::string::npos);
        if (low.find("url(") != std::string::npos) {
            size_t us = low.find("url("), ue = val.find(')', us + 4);
            if (ue != std::string::npos)
                bgImg = stripQuotes(sTrim(val.substr(us + 4, ue - us - 4)));
        }
        CssColor bg; int colorCount = 0; bool invalid = false;
        int repeat = -1;                       // -1 = unspecified
        std::vector<std::string> posToks;      // background-position candidates
        std::string sizeStr;                   // text after '/' (background-size)
        bool afterSlash = false;
        {
            size_t i = 0;
            while (i < val.size()) {
                while (i < val.size() && std::isspace((unsigned char)val[i])) i++;
                if (i >= val.size()) break;
                if (val[i] == '/') { afterSlash = true; i++; continue; }
                // Paren-aware token: don't split inside url()/rgb()/hsl().
                size_t j = i; int depth = 0;
                while (j < val.size() && (depth > 0 || (!std::isspace((unsigned char)val[j]) && val[j] != '/'))) {
                    if (val[j] == '(') depth++;
                    else if (val[j] == ')' && depth > 0) depth--;
                    j++;
                }
                std::string tok = val.substr(i, j - i);
                std::string tl = sLower(tok);
                i = j;
                if (tl.rfind("url(", 0) == 0) continue;   // image handled above
                if (tl == "no-repeat") { repeat = 3; continue; }
                if (tl == "repeat")    { repeat = 0; continue; }
                if (tl == "repeat-x")  { repeat = 1; continue; }
                if (tl == "repeat-y")  { repeat = 2; continue; }
                if (afterSlash || tl == "cover" || tl == "contain") {
                    sizeStr += (sizeStr.empty() ? "" : " ") + tl; continue;
                }
                bool posKeyword = (tl == "center" || tl == "top" || tl == "bottom"
                    || tl == "left" || tl == "right");
                bool lengthish = !tl.empty() && (std::isdigit((unsigned char)tl[0])
                    || tl[0] == '-' || tl[0] == '+' || tl.find('%') != std::string::npos);
                if (posKeyword || lengthish) { posToks.push_back(tl); continue; }
                bool keyword = (tl == "fixed" || tl == "scroll" || tl == "local" || tl == "none"
                    || tl == "inherit" || tl == "border-box" || tl == "padding-box"
                    || tl == "content-box");
                if (tl == "transparent") { bg = {true,0,0,0,0}; colorCount++; }
                else if (!keyword) {
                    CssColor c = ParseCssColor(tok);
                    if (c.valid) { bg = c; colorCount++; }
                    else invalid = true;   // unknown token (e.g. a second colour name)
                }
            }
        }
        if (colorCount > 1 || invalid) {
            // invalid background value → ignore declaration entirely
        } else {
            out.backgroundImage = bgImg;
            out.backgroundImageSet = true;
            out.bgNoRepeat = noRepeat;
            out.bgFixed = fixed;
            out.bgColor = bg;
            out.bgColorSet = true;
            if (repeat >= 0) { out.bgRepeat = repeat; out.bgNoRepeat = (repeat == 3); out.bgRepeatSet = true; }
            if (!posToks.empty()) {
                std::string p; for (auto& t : posToks) p += (p.empty() ? "" : " ") + t;
                ParseBgPosition(p, out);
            }
            if (!sizeStr.empty()) ParseBgSize(sizeStr, out);
        }
    } else if (prop == "background-image") {
        std::string low = sLower(val);
        out.backgroundImage.clear();
        out.backgroundImageSet = true;
        if (low.find("url(") != std::string::npos) {
            size_t us = low.find("url("), ue = val.find(')', us + 4);
            if (ue != std::string::npos) {
                std::string url = sTrim(val.substr(us + 4, ue - us - 4));
                out.backgroundImage = stripQuotes(url);
            }
        }
    } else if (prop == "background-repeat") {
        std::string v = sLower(sTrim(val));
        out.bgRepeat = (v == "no-repeat") ? 3 : (v == "repeat-x") ? 1
                     : (v == "repeat-y") ? 2 : 0;
        out.bgNoRepeat = (out.bgRepeat == 3);
        out.bgRepeatSet = true;
    } else if (prop == "background-position") {
        ParseBgPosition(sLower(sTrim(val)), out);
    } else if (prop == "background-size") {
        ParseBgSize(sLower(sTrim(val)), out);
    } else if (prop == "font-size") {
        std::string v = sLower(sTrim(val));
        if      (v == "small")    out.fontSize = 12;
        else if (v == "medium")   out.fontSize = 15;
        else if (v == "large")    out.fontSize = 18;
        else if (v == "x-large")  out.fontSize = 22;
        else if (v == "xx-large") out.fontSize = 28;
        else if (v == "smaller")  out.fontSize = 12;
        else if (v == "larger")   out.fontSize = 18;
        else {
            float f = ParseLength(val);
            if (f > 0) {
                out.fontSize = f;
                // Only update g_emBase for absolute font sizes (px/pt), not em-relative
                std::string rawLow = sLower(sTrim(val));
                if (rawLow.find("em") == std::string::npos) g_emBase = f;
            }
        }
    } else if (prop == "font-weight") {
        std::string v = sLower(val);
        out.boldSet = true;
        int w = 0; try { w = std::stoi(v); } catch (...) {}
        out.bold = (v == "bold" || v == "bolder" || w >= 600);
    } else if (prop == "font-style") {
        out.italicSet = true;
        out.italic = (sLower(val) == "italic" || sLower(val) == "oblique");
    } else if (prop == "font-family") {
        std::string v = val;
        size_t comma = v.find(',');
        if (comma != std::string::npos) v = v.substr(0, comma);
        v = stripQuotes(sTrim(v));
        std::string low = sLower(v);
        if      (low == "sans-serif")                           v = "Segoe UI";
        else if (low == "serif")                                v = "Georgia";
        else if (low == "monospace" || low == "courier"
              || low == "courier new")                          v = "Consolas";
        else if (low == "helvetica" || low == "arial")          v = "Arial";
        else if (low == "times" || low == "times new roman")    v = "Times New Roman";
        else if (low == "system-ui" || low == "ui-sans-serif")  v = "Segoe UI";
        out.fontFamily = v;
    } else if (prop == "font") {
        // simplified font shorthand: extract weight, style, size, family
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string low = sLower(tok);
            if (low == "bold" || low == "bolder") { out.bold = true; out.boldSet = true; }
            else if (low == "italic" || low == "oblique") { out.italic = true; out.italicSet = true; }
            else if (low == "normal") { /* no-op */ }
            else {
                // Check for size (maybe size/lineheight)
                size_t slash = low.find('/');
                std::string sizePart = (slash != std::string::npos) ? low.substr(0, slash) : low;
                float sz = ParseLength(sizePart);
                if (sz > 0) {
                    out.fontSize = sz;
                    // Only update g_emBase for absolute font sizes, not em-relative
                    if (sizePart.find("em") == std::string::npos) g_emBase = sz;
                    if (slash != std::string::npos) {
                        float lh = ParseLineHeightValue(low.substr(slash + 1), sz);
                        if (lh > 0) out.lineHeight = lh;
                    }
                    // rest of string is font family
                    std::string rest;
                    while (vs >> tok) { if (!rest.empty()) rest += " "; rest += tok; }
                    if (!rest.empty()) {
                        ComputedStyle tmp; ApplyDeclaration("font-family", rest, tmp);
                        out.fontFamily = tmp.fontFamily;
                    }
                    break;
                }
            }
        }
    } else if (prop == "text-decoration" || prop == "text-decoration-line") {
        out.underline = (sLower(val).find("underline") != std::string::npos);
    } else if (prop == "text-transform") {
        std::string v = sLower(val);
        out.textTransformSet = true;
        if      (v == "uppercase")  out.textTransform = 1;
        else if (v == "lowercase")  out.textTransform = 2;
        else if (v == "capitalize") out.textTransform = 3;
        else                        out.textTransform = 0;
    } else if (prop == "white-space") {
        std::string v = sLower(sTrim(val));
        out.whiteSpaceSet = true;
        out.whiteSpaceNowrap = (v == "nowrap" || v == "pre");
        out.whiteSpacePre = (v == "pre" || v == "pre-wrap" || v == "pre-line");
    } else if (prop == "display") {
        std::string v = sLower(sTrim(val));
        if      (v == "none")                                          out.display = 3;
        else if (v == "block")                                         out.display = 1;
        else if (v == "inline")                                        out.display = 2;
        else if (v == "flex" || v == "inline-flex" || v == "grid" || v == "inline-grid") out.display = 4;
        else if (v == "table" || v == "inline-table")                  out.display = 5;
        else if (v == "table-cell")                                    out.display = 6;
        else if (v == "inline-block")                                  out.display = 7;
        else if (v == "list-item")                                     out.display = 8;
        else if (v == "table-row")                                     out.display = 9;
        else if (v == "table-row-group" || v == "table-header-group"
              || v == "table-footer-group")                            out.display = 10;
    } else if (prop == "flex-direction") {
        out.flexDirectionSet = true;
        out.flexDirection = sLower(sTrim(val)) == "column" ? 1 : 0;
    } else if (prop == "flex-grow") {
        try {
            out.flexGrow = std::max(0.f, std::stof(sTrim(val)));
            out.flexGrowSet = true;
        } catch (...) {}
    } else if (prop == "flex") {
        std::istringstream values(val);
        std::string grow;
        values >> grow;
        try {
            out.flexGrow = std::max(0.f, std::stof(grow));
            out.flexGrowSet = true;
        } catch (...) {}
    } else if (prop == "gap") {
        std::istringstream values(val);
        std::string gap;
        values >> gap;
        float parsed = ParseLength(gap);
        if (parsed >= 0) out.flexGap = parsed;
    } else if (prop == "margin") {
        std::istringstream vs(val); std::vector<float> v;
        std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "auto") v.push_back(kCssAuto);
            else if (tl == "inherit" || tl == "initial" || tl == "unset") continue;
            else { float f = ParseLength(tok); if (f > -1e5f) v.push_back(f); }
        }
        if (v.size() == 1) {
            out.marginTop = out.marginRight = out.marginBottom = out.marginLeft = v[0];
        } else if (v.size() == 2) {
            out.marginTop = out.marginBottom = v[0];
            out.marginRight = out.marginLeft = v[1];
        } else if (v.size() == 3) {
            out.marginTop = v[0]; out.marginRight = out.marginLeft = v[1]; out.marginBottom = v[2];
        } else if (v.size() >= 4) {
            out.marginTop = v[0]; out.marginRight = v[1]; out.marginBottom = v[2]; out.marginLeft = v[3];
        }
    } else if (prop == "margin-top") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginTop = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginTop = f; }
    } else if (prop == "margin-bottom") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginBottom = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginBottom = f; }
    } else if (prop == "margin-right") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginRight = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginRight = f; }
    } else if (prop == "margin-left") {
        std::string v = sLower(sTrim(val));
        if (v == "auto") out.marginLeft = kCssAuto;
        else if (v != "inherit" && v != "initial" && v != "unset") { float f = ParseLength(val); if (f > -1e5f) out.marginLeft = f; }

    } else if (prop == "padding") {
        std::istringstream vs(val); std::vector<float> v;
        std::string tok;
        while (vs >> tok) { float f = ParseLength(tok); v.push_back(f < 0 ? 0 : f); }
        if (v.size() == 1) {
            out.paddingTop = out.paddingRight = out.paddingBottom = out.paddingLeft = v[0];
        } else if (v.size() == 2) {
            out.paddingTop = out.paddingBottom = v[0];
            out.paddingRight = out.paddingLeft = v[1];
        } else if (v.size() == 3) {
            out.paddingTop = v[0]; out.paddingRight = out.paddingLeft = v[1]; out.paddingBottom = v[2];
        } else if (v.size() >= 4) {
            out.paddingTop = v[0]; out.paddingRight = v[1]; out.paddingBottom = v[2]; out.paddingLeft = v[3];
        }
    } else if (prop == "padding-top") {
        float f = ParseLength(val); if (f >= 0) out.paddingTop    = f;
    } else if (prop == "padding-right") {
        float f = ParseLength(val); if (f >= 0) out.paddingRight  = f;
    } else if (prop == "padding-bottom") {
        float f = ParseLength(val); if (f >= 0) out.paddingBottom = f;
    } else if (prop == "padding-left") {
        float f = ParseLength(val); if (f >= 0) out.paddingLeft   = f;

    } else if (prop == "border-width") {
        std::istringstream vs(val); std::vector<float> widths; std::string tok;
        while (vs >> tok) { float f = ParseLength(tok); if (f >= 0) widths.push_back(f); }
        if (widths.size() == 1) out.borderWidth = widths[0];
        else if (widths.size() == 2) {
            out.borderTopWidth = out.borderBottomWidth = widths[0];
            out.borderRightWidth = out.borderLeftWidth = widths[1];
        } else if (widths.size() == 3) {
            out.borderTopWidth = widths[0]; out.borderRightWidth = out.borderLeftWidth = widths[1]; out.borderBottomWidth = widths[2];
        } else if (widths.size() >= 4) {
            out.borderTopWidth = widths[0]; out.borderRightWidth = widths[1]; out.borderBottomWidth = widths[2]; out.borderLeftWidth = widths[3];
        }
    } else if (prop == "border-color") {
        std::istringstream vs(val); std::vector<CssColor> colors; std::string tok;
        while (vs >> tok) { CssColor c = ParseCssColor(tok); if (c.valid) colors.push_back(c); }
        if (colors.size() == 1) out.borderColor = colors[0];
        else if (colors.size() == 2) {
            out.borderTopColor = out.borderBottomColor = colors[0];
            out.borderRightColor = out.borderLeftColor = colors[1];
        } else if (colors.size() == 3) {
            out.borderTopColor = colors[0]; out.borderRightColor = out.borderLeftColor = colors[1]; out.borderBottomColor = colors[2];
        } else if (colors.size() >= 4) {
            out.borderTopColor = colors[0]; out.borderRightColor = colors[1]; out.borderBottomColor = colors[2]; out.borderLeftColor = colors[3];
        }
    } else if (prop == "border-top-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderTopColor = c;
    } else if (prop == "border-right-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderRightColor = c;
    } else if (prop == "border-bottom-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderBottomColor = c;
    } else if (prop == "border-left-color") {
        CssColor c = ParseCssColor(val); if (c.valid) out.borderLeftColor = c;
    } else if (prop == "border-radius") {
        float f = ParseLength(val); if (f >= 0) out.borderRadius = f;
    } else if (prop == "border") {
        std::string low = sLower(sTrim(val));
        if (low == "none" || low == "0") {
            out.borderWidth = 0;
            out.borderTopWidth = out.borderRightWidth = out.borderBottomWidth = out.borderLeftWidth = 0;
        } else {
            std::istringstream vs(val); std::string tok;
            bool hasWidth = false;
            while (vs >> tok) {
                std::string tl = sLower(tok);
                if (tl == "none") { out.borderWidth = 0; out.borderTopWidth = out.borderRightWidth = out.borderBottomWidth = out.borderLeftWidth = 0; break; }
                if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") {
                    if (!hasWidth) { out.borderWidth = 3; hasWidth = true; }
                    continue;
                }
                float f = ParseLength(tok);
                if (f >= 0) { out.borderWidth = f; hasWidth = true; continue; }
                CssColor c = ParseCssColor(tok);
                if (c.valid) out.borderColor = c;
            }
        }
    } else if (prop == "border-style") {
        // Parse 1-4 values and set sides to 0 if "none"/"hidden", leave them if "solid" etc.
        std::vector<std::string> vals;
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) vals.push_back(sLower(sTrim(tok)));
        while (vals.size() < 4) {
            if (vals.size() == 1)      vals.push_back(vals[0]);
            else if (vals.size() == 2) vals.push_back(vals[0]);
            else                       vals.push_back(vals[1]);
        }
        // 0=top,1=right,2=bottom,3=left
        auto applyStyle = [&](const std::string& s, float& side) {
            if (s == "none" || s == "hidden") side = 0;
            // "solid","dashed" etc — border remains at its current width (from 'border:' shorthand)
        };
        applyStyle(vals[0], out.borderTopWidth);
        applyStyle(vals[1], out.borderRightWidth);
        applyStyle(vals[2], out.borderBottomWidth);
        applyStyle(vals[3], out.borderLeftWidth);
    } else if (prop == "border-top" || prop == "border-top-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderTopWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderTopWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderTopColor = c;
        }
    } else if (prop == "border-right" || prop == "border-right-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderRightWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderRightWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderRightColor = c;
        }
    } else if (prop == "border-bottom" || prop == "border-bottom-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderBottomWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderBottomWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderBottomColor = c;
        }
    } else if (prop == "border-left" || prop == "border-left-width") {
        std::istringstream vs(val); std::string tok;
        while (vs >> tok) {
            std::string tl = sLower(tok);
            if (tl == "none") { out.borderLeftWidth = 0; break; }
            if (tl == "solid" || tl == "dashed" || tl == "dotted" || tl == "double" || tl == "groove" || tl == "ridge" || tl == "inset" || tl == "outset" || tl == "hidden") continue;
            float f = ParseLength(tok); if (f >= 0) { out.borderLeftWidth = f; continue; }
            CssColor c = ParseCssColor(tok); if (c.valid) out.borderLeftColor = c;
        }

    } else if (prop == "line-height") {
        float lh = ParseLineHeightValue(val, out.fontSize > 0 ? out.fontSize : 16.f);
        if (lh > 0) out.lineHeight = lh;
    } else if (prop == "width") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.widthPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.width = f; }
    } else if (prop == "height") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.heightPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.height = f; }
    } else if (prop == "max-width") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.maxWidthPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.maxWidth = f; }
    } else if (prop == "min-width") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.minWidthPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.minWidth = f; }
    } else if (prop == "min-height") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.minHeightPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.minHeight = f; }
    } else if (prop == "max-height") {
        float pct = ParsePercentage(val);
        if (pct >= 0) out.maxHeightPercent = pct;
        else { float f = ParseLength(val); if (f >= 0) out.maxHeight = f; }
    } else if (prop == "content") {
        std::string v = sLower(sTrim(val));
        out.contentSet = v != "none" && v != "normal";
        out.content = out.contentSet ? CssUnescape(stripQuotes(sTrim(val))) : "";
    } else if (prop == "text-align") {
        std::string v = sLower(sTrim(val));
        out.textAlignSet = true;
        if      (v == "center") out.textAlign = 1;
        else if (v == "right")  out.textAlign = 2;
        else                    out.textAlign = 0;
    } else if (prop == "float") {
        std::string v = sLower(sTrim(val));
        if      (v == "left")    out.floatMode = 1;
        else if (v == "right")   out.floatMode = 2;
        else if (v == "inherit") out.floatInherit = true;
        else                     out.floatMode = 0;
    } else if (prop == "clear") {
        std::string v = sLower(sTrim(val));
        if      (v == "left")  out.clearMode = 1;
        else if (v == "right") out.clearMode = 2;
        else if (v == "both")  out.clearMode = 3;
        else                   out.clearMode = 0;
    } else if (prop == "position") {
        std::string v = sLower(sTrim(val));
        if      (v == "relative") out.positionMode = 1;
        else if (v == "absolute") out.positionMode = 2;
        else if (v == "fixed")    out.positionMode = 3;
        else                      out.positionMode = 0;
    } else if (prop == "z-index") {
        std::string v = sLower(sTrim(val));
        if (v != "auto") {
            try {
                out.zIndex = std::stoi(v);
                out.zIndexSet = true;
            } catch (...) {}
        }
    } else if (prop == "overflow" || prop == "overflow-x" || prop == "overflow-y") {
        out.overflowHidden = (sLower(sTrim(val)) == "hidden");
        out.overflowSet = true;
    } else if (prop == "top") {
        std::string v = sLower(sTrim(val));
        if (v != "auto" && v != "inherit" && v != "initial" && v != "unset")
            { out.top = ParseLength(val); out.topSet = true; }
    } else if (prop == "right") {
        std::string v = sLower(sTrim(val));
        if (v != "auto" && v != "inherit" && v != "initial" && v != "unset")
            { out.right = ParseLength(val); out.rightSet = true; }
    } else if (prop == "bottom") {
        std::string v = sLower(sTrim(val));
        if (v != "auto" && v != "inherit" && v != "initial" && v != "unset")
            { out.bottom = ParseLength(val); out.bottomSet = true; }
    } else if (prop == "left") {
        std::string v = sLower(sTrim(val));
        if (v != "auto" && v != "inherit" && v != "initial" && v != "unset")
            { out.left = ParseLength(val); out.leftSet = true; }
    } else if (prop == "opacity") {
        // not stored separately — could multiply into color alpha
    } else if (prop == "visibility") {
        std::string v = sLower(sTrim(val));
        out.visibilitySet = true;
        out.visibilityHidden = (v == "hidden" || v == "collapse");
    } else if (prop == "list-style" || prop == "list-style-type") {
        std::string v = sLower(sTrim(val));
        out.listStyleSet = true;
        out.listStyleNone = (v == "none" || v.find("none") != std::string::npos);
    } else if (prop == "border-spacing") {
        float f = ParseLength(val);
        if (f >= 0) out.borderSpacing = f;
    }
}

ComputedStyle ParseInlineStyle(const std::string& style) {
    ComputedStyle out;
    for (const auto& decl : SplitDeclarations(style)) {
        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        ApplyDeclaration(sLower(sTrim(decl.substr(0, colon))),
                         sTrim(decl.substr(colon+1)), out);
    }
    return out;
}

// ─── selector matching ───────────────────────────────────────────────────────

static const Node* PreviousElementSibling(const Node* node);
static const Node* NextElementSibling(const Node* node);

static bool MatchesPseudoClass(const std::string& pseudo, const Node* node) {
    if (pseudo == "first-child") return PreviousElementSibling(node) == nullptr;
    if (pseudo == "last-child") return NextElementSibling(node) == nullptr;
    if (pseudo == "only-child") return PreviousElementSibling(node) == nullptr
                                      && NextElementSibling(node) == nullptr;
    if (pseudo == "empty") return node && node->children.empty();
    if (pseudo == "link") return node && node->tagName == "a"
                              && node->attrs.find("href") != node->attrs.end();
    if (pseudo == "root") return node && node->parent
                              && node->parent->type == NodeType::Document;
    return false;
}

static bool MatchesSimpleSelector(const CssSelectorPart& part, const Node* node) {
    if (!node || node->type != NodeType::Element) return false;
    if (part.neverMatch) return false;
    const std::string generated = node->attr("_helix_pseudo");
    if (!part.pseudoElement.empty()) {
        if (generated != part.pseudoElement) return false;
    } else if (!generated.empty()) {
        return false;
    }
    if (!part.tag.empty() && node->tagName != part.tag) return false;
    if (!part.id.empty() && node->attr("id") != part.id) return false;
    if (!part.attrName.empty()) {
        std::string value = node->attr(part.attrName);
        if (value.empty() && node->attrs.find(part.attrName) == node->attrs.end()) return false;
        if (part.attrHasValue) {
            bool matches = false;
            switch (part.attrMatch) {
                case CssAttrMatch::Exact:
                    matches = (value == part.attrValue);
                    break;
                case CssAttrMatch::Includes: {
                    std::istringstream ss(value);
                    std::string token;
                    while (ss >> token) {
                        if (token == part.attrValue) {
                            matches = true;
                            break;
                        }
                    }
                    break;
                }
                case CssAttrMatch::DashPrefix:
                    matches = (value == part.attrValue)
                           || (value.size() > part.attrValue.size()
                            && value.compare(0, part.attrValue.size(), part.attrValue) == 0
                            && value[part.attrValue.size()] == '-');
                    break;
                case CssAttrMatch::Prefix:
                    matches = value.size() >= part.attrValue.size()
                           && value.compare(0, part.attrValue.size(), part.attrValue) == 0;
                    break;
                case CssAttrMatch::Suffix:
                    matches = value.size() >= part.attrValue.size()
                           && value.compare(value.size() - part.attrValue.size(), part.attrValue.size(), part.attrValue) == 0;
                    break;
                case CssAttrMatch::Substring:
                    matches = value.find(part.attrValue) != std::string::npos;
                    break;
                case CssAttrMatch::Exists:
                    matches = true;
                    break;
            }
            if (!matches) return false;
        }
    }
    for (const auto& required : part.classes) {
        auto ca = node->attr("class");
        bool found = false;
        std::istringstream ss(ca);
        std::string tok;
        while (ss >> tok) if (tok == required) { found = true; break; }
        if (!found) return false;
    }
    for (const auto& pseudo : part.pseudos) {
        if (!MatchesPseudoClass(pseudo, node)) return false;
    }
    return true;
}

static const Node* PreviousElementSibling(const Node* node) {
    if (!node || !node->parent) return nullptr;
    const Node* previous = nullptr;
    for (const auto& child : node->parent->children) {
        if (child.get() == node) return previous;
        if (child->type == NodeType::Element) previous = child.get();
    }
    return nullptr;
}

static const Node* NextElementSibling(const Node* node) {
    if (!node || !node->parent) return nullptr;
    bool seenCurrent = false;
    for (const auto& child : node->parent->children) {
        if (child.get() == node) {
            seenCurrent = true;
            continue;
        }
        if (seenCurrent && child->type == NodeType::Element) return child.get();
    }
    return nullptr;
}

int CssRule::specificity() const {
    if (selector.empty()) {
        return (!id.empty() ? 100 : 0)
             + (!cls.empty() ?  10 : 0)
             + (!tag.empty() ?   1 : 0);
    }

    int total = 0;
    for (const auto& part : selector) {
        total += (!part.id.empty() ? 100 : 0)
               + ((int)part.classes.size() * 10)
               + (!part.attrName.empty() ? 10 : 0)
               + (!part.tag.empty() ? 1 : 0);
    }
    return total;
}

bool CssRule::matches(const Node* node) const {
    if (!node || node->type != NodeType::Element) return false;
    if (!selector.empty()) {
        int i = (int)selector.size() - 1;
        const Node* current = node;
        if (!MatchesSimpleSelector(selector[i], current)) return false;

        for (; i > 0; --i) {
            char combinator = selector[i].combinator;
            const CssSelectorPart& wanted = selector[i - 1];

            if (combinator == '>') {
                current = current->parent;
                if (!MatchesSimpleSelector(wanted, current)) return false;
            } else if (combinator == '+') {
                current = PreviousElementSibling(current);
                if (!MatchesSimpleSelector(wanted, current)) return false;
            } else {
                const Node* ancestor = current->parent;
                bool found = false;
                while (ancestor) {
                    if (MatchesSimpleSelector(wanted, ancestor)) {
                        current = ancestor;
                        found = true;
                        break;
                    }
                    ancestor = ancestor->parent;
                }
                if (!found) return false;
            }
        }
        return true;
    }

    if (!tag.empty() && node->tagName != tag) return false;
    if (!id.empty()  && node->attr("id") != id) return false;
    if (!cls.empty()) {
        // class attr is space-separated list
        auto ca = node->attr("class");
        bool found = false;
        std::istringstream ss(ca);
        std::string tok;
        while (ss >> tok) if (tok == cls) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

// ─── cascade resolution ──────────────────────────────────────────────────────

ComputedStyle Stylesheet::resolve(const Node* node) const {
    // Collect matching rules, sorted by specificity
    std::vector<const CssRule*> matched;
    for (auto& r : rules) if (r.matches(node)) matched.push_back(&r);
    std::stable_sort(matched.begin(), matched.end(),
        [](const CssRule* a, const CssRule* b) {
            return a->specificity() < b->specificity();
        });

    ComputedStyle out;
    for (auto* r : matched) out = out.inherit(r->style);

    // Inline style wins over everything
    auto inl = node->attr("style");
    if (!inl.empty()) out = out.inherit(ParseInlineStyle(inl));
    return out;
}

// ─── CSS text parser ─────────────────────────────────────────────────────────

// Strip /* ... */ comments
static std::string stripComments(const std::string& css) {
    std::string out; out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i+1 < css.size() && css[i]=='/' && css[i+1]=='*') {
            i += 2;
            while (i+1 < css.size() && !(css[i]=='*' && css[i+1]=='/')) i++;
            i += 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

// Parse one simple selector like "div", ".foo", "#bar", "div.foo", "a#id"
static CssSelectorPart parseSimpleSelectorPart(const std::string& sel) {
    CssSelectorPart part;
    std::string cur;
    char mode = 't'; // t=tag, c=class, i=id
    auto flush = [&]() {
        if (cur.empty()) return;
        std::string ident = CssUnescape(cur);
        if (mode == 't') part.tag = sLower(ident);
        if (mode == 'c') part.classes.push_back(ident);
        if (mode == 'i') part.id  = ident;
        cur.clear();
    };
    for (size_t i = 0; i < sel.size(); ++i) {
        char c = sel[i];
        if (c == '.') { flush(); mode = 'c'; }
        else if (c == '#') { flush(); mode = 'i'; }
        else if (c == '[') {
            flush();
            size_t end = sel.find(']', i + 1);
            if (end == std::string::npos) break;
            std::string body = sTrim(sel.substr(i + 1, end - i - 1));
            size_t opPos = std::string::npos;
            std::string op;
            for (const std::string candidate : { "~=", "|=", "^=", "$=", "*=", "=" }) {
                opPos = body.find(candidate);
                if (opPos != std::string::npos) {
                    op = candidate;
                    break;
                }
            }
            if (opPos == std::string::npos) {
                part.attrName = sLower(CssUnescape(sTrim(body)));
                part.attrHasValue = false;
                part.attrMatch = CssAttrMatch::Exists;
            } else {
                part.attrName = sLower(CssUnescape(sTrim(body.substr(0, opPos))));
                part.attrValue = CssUnescape(stripQuotes(body.substr(opPos + op.size())));
                part.attrHasValue = true;
                if (op == "~=") part.attrMatch = CssAttrMatch::Includes;
                else if (op == "|=") part.attrMatch = CssAttrMatch::DashPrefix;
                else if (op == "^=") part.attrMatch = CssAttrMatch::Prefix;
                else if (op == "$=") part.attrMatch = CssAttrMatch::Suffix;
                else if (op == "*=") part.attrMatch = CssAttrMatch::Substring;
                else part.attrMatch = CssAttrMatch::Exact;
            }
            i = end;
        }
        else if (c == ':') {
            flush();
            bool isDoubleColon = i + 1 < sel.size() && sel[i + 1] == ':';
            if (isDoubleColon) ++i;

            size_t nameStart = i + 1;
            size_t j = nameStart;
            while (j < sel.size()
                && (std::isalnum((unsigned char)sel[j]) || sel[j] == '-' || sel[j] == '_')) {
                ++j;
            }

            std::string pseudo = sLower(CssUnescape(sel.substr(nameStart, j - nameStart)));
            if (j < sel.size() && sel[j] == '(') {
                int depth = 1;
                ++j;
                bool escapedArg = false;
                char quoteArg = 0;
                while (j < sel.size() && depth > 0) {
                    char pc = sel[j];
                    if (escapedArg) {
                        escapedArg = false;
                    } else if (pc == '\\') {
                        escapedArg = true;
                    } else if (quoteArg) {
                        if (pc == quoteArg) quoteArg = 0;
                    } else if (pc == '"' || pc == '\'') {
                        quoteArg = pc;
                    } else if (pc == '(') {
                        ++depth;
                    } else if (pc == ')') {
                        --depth;
                    }
                    ++j;
                }
                part.neverMatch = true;
                i = j - 1;
            } else {
                if (pseudo == "before" || pseudo == "after") {
                    part.pseudoElement = pseudo;
                } else if (!isDoubleColon && (pseudo == "first-child" || pseudo == "last-child"
                    || pseudo == "only-child" || pseudo == "empty"
                    || pseudo == "link" || pseudo == "root")) {
                    part.pseudos.push_back(pseudo);
                } else {
                    part.neverMatch = true;
                }
                i = j - 1;
            }
        }
        else cur += c;
    }
    flush();
    // "*" or empty tag = universal
    if (part.tag == "*") part.tag.clear();
    return part;
}

static std::vector<CssSelectorPart> parseSelectorChain(std::string selector) {
    std::vector<CssSelectorPart> parts;
    std::string tok;
    char nextCombinator = 0;
    int bracketDepth = 0;
    char quote = 0;
    bool escaped = false;

    auto flushToken = [&]() {
        if (tok.empty()) return;
        CssSelectorPart part = parseSimpleSelectorPart(tok);
        part.combinator = parts.empty() ? 0 : (nextCombinator ? nextCombinator : ' ');
        parts.push_back(part);
        tok.clear();
        nextCombinator = 0;
    };

    for (char c : selector) {
        if (escaped) {
            tok += c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            tok += c;
            escaped = true;
            continue;
        }
        if (quote) {
            tok += c;
            if (c == quote) quote = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            tok += c;
            quote = c;
            continue;
        }
        if (c == '[') {
            ++bracketDepth;
            tok += c;
            continue;
        }
        if (c == ']') {
            if (bracketDepth > 0) --bracketDepth;
            tok += c;
            continue;
        }
        if (bracketDepth == 0 && (c == '>' || c == '+')) {
            flushToken();
            nextCombinator = c;
            continue;
        }
        if (bracketDepth == 0 && std::isspace((unsigned char)c)) {
            flushToken();
            if (!parts.empty() && nextCombinator == 0) nextCombinator = ' ';
            continue;
        }
        tok += c;
    }
    flushToken();
    return parts;
}

static CssRule parseSelector(const std::string& sel) {
    CssRule rule;
    rule.selector = parseSelectorChain(sel);
    if (!rule.selector.empty()) {
        const auto& last = rule.selector.back();
        rule.tag = last.tag;
        rule.cls = !last.classes.empty() ? last.classes[0] : "";
        rule.id = last.id;
    }
    return rule;
}

Stylesheet ParseStylesheet(const std::string& rawCss) {
    g_emBase = 16.f;  // reset per stylesheet
    Stylesheet sheet;
    std::string css = stripComments(rawCss);

    size_t pos = 0;
    while (pos < css.size()) {
        // Skip whitespace
        while (pos < css.size() && std::isspace((unsigned char)css[pos])) pos++;
        if (pos >= css.size()) break;

        // Skip @-rules — they either end with ; or contain nested {} blocks
        if (css[pos] == '@') {
            size_t semiPos = css.find(';', pos);
            size_t lbPos   = css.find('{', pos);
            if (semiPos != std::string::npos
                && (lbPos == std::string::npos || semiPos < lbPos)) {
                // Statement @-rule (@charset, @import, @namespace)
                pos = semiPos + 1;
            } else if (lbPos != std::string::npos) {
                // Block @-rule (@media, @keyframes, @supports, @font-face)
                // Skip to the matching closing brace, counting nesting depth
                pos = lbPos + 1;
                int depth = 1;
                while (pos < css.size() && depth > 0) {
                    if (css[pos] == '{') depth++;
                    else if (css[pos] == '}') depth--;
                    pos++;
                }
            } else {
                break;
            }
            continue;
        }

        // Find next '{'
        size_t lbrace = css.find('{', pos);
        if (lbrace == std::string::npos) break;

        // If another '@' appears before this '{', restart loop to handle it
        size_t atPos = css.find('@', pos);
        if (atPos != std::string::npos && atPos < lbrace) {
            pos = atPos;
            continue;
        }

        // Find the matching closing brace, skipping CSS escapes like \}
        size_t rbrace = std::string::npos;
        {
            bool esc = false;
            char inQuote = 0;
            for (size_t j = lbrace + 1; j < css.size(); ++j) {
                char c = css[j];
                if (esc) { esc = false; continue; }
                if (c == '\\') { esc = true; continue; }
                if (inQuote) { if (c == inQuote) inQuote = 0; continue; }
                if (c == '"' || c == '\'') { inQuote = c; continue; }
                if (c == '}') { rbrace = j; break; }
            }
        }
        if (rbrace == std::string::npos) break;

        std::string selectorBlock = sTrim(css.substr(pos, lbrace - pos));
        std::string declBlock     = css.substr(lbrace+1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        if (selectorBlock.empty()) continue;

        // CSS declaration order must not decide the unit basis of other
        // properties.  In particular, `margin: 100em; font: 2em ...` uses the
        // element's computed font size, not the font that happened to be
        // current when the margin was parsed.  Resolve font declarations from
        // the inherited/root basis first, then resolve all remaining lengths
        // against that element-local basis.
        const float inheritedEmBase = g_emBase;
        const auto declarations = SplitDeclarations(declBlock);
        ComputedStyle declStyle;
        g_emBase = inheritedEmBase;
        for (const auto& decl : declarations) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            const std::string property = sLower(sTrim(decl.substr(0, colon)));
            if (property == "font" || property == "font-size"
                || property == "font-family" || property == "font-weight"
                || property == "font-style") {
                ApplyDeclaration(property, sTrim(decl.substr(colon+1)), declStyle);
            }
        }
        const float elementEmBase = declStyle.fontSize > 0 ? declStyle.fontSize : inheritedEmBase;
        g_emBase = elementEmBase;
        for (const auto& decl : declarations) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            const std::string property = sLower(sTrim(decl.substr(0, colon)));
            if (property == "font" || property == "font-size"
                || property == "font-family" || property == "font-weight"
                || property == "font-style") continue;
            ApplyDeclaration(property, sTrim(decl.substr(colon+1)), declStyle);
        }

        // Each comma-separated selector becomes its own rule
        std::istringstream ss(selectorBlock);
        std::string selPart;
        while (std::getline(ss, selPart, ',')) {
            selPart = sTrim(selPart);
            if (selPart.empty()) continue;

            CssRule rule = parseSelector(selPart);
            rule.style = declStyle;
            sheet.rules.push_back(rule);
        }

        // `html` provides the root inherited font size.  Other rules are
        // independent selectors and must not leak their local font basis.
        if (selectorBlock == "html" || selectorBlock == ":root")
            g_emBase = elementEmBase;
        else
            g_emBase = inheritedEmBase;
    }
    return sheet;
}

static std::string BoolText(bool value) {
    return value ? "true" : "false";
}

static void AppendColor(std::ostringstream& out, const char* name, const CssColor& color) {
    if (!color.valid) return;
    out << name << "=" << color.r << "," << color.g << "," << color.b << "," << color.a << " ";
}

std::string SerializeComputedStyle(const ComputedStyle& style) {
    std::ostringstream out;
    AppendColor(out, "color", style.color);
    AppendColor(out, "bg", style.bgColor);
    if (style.fontSize > 0) out << "fontSize=" << style.fontSize << " ";
    if (style.boldSet) out << "bold=" << BoolText(style.bold) << " ";
    if (style.italicSet) out << "italic=" << BoolText(style.italic) << " ";
    if (style.underline) out << "underline=true ";
    if (style.display == 3) out << "display=none ";
    else if (style.display == 1) out << "display=block ";
    else if (style.display == 2) out << "display=inline ";
    else if (style.display == 5) out << "display=table ";
    else if (style.display == 6) out << "display=table-cell ";
    if (style.marginTopSet()) out << "marginTop="    << style.marginTop    << " ";
    if (style.marginRightSet()) out << "marginRight="  << style.marginRight  << " ";
    if (style.marginBottomSet()) out << "marginBottom=" << style.marginBottom << " ";
    if (style.marginLeftSet()) out << "marginLeft="   << style.marginLeft   << " ";
    if (style.paddingTop   >= 0) out << "paddingTop="   << style.paddingTop   << " ";
    if (style.paddingRight >= 0) out << "paddingRight=" << style.paddingRight << " ";
    if (style.paddingBottom>= 0) out << "paddingBottom="<< style.paddingBottom<< " ";
    if (style.paddingLeft  >= 0) out << "paddingLeft="  << style.paddingLeft  << " ";
    if (style.borderWidth  >= 0) out << "borderWidth="  << style.borderWidth  << " ";
    if (style.borderTopWidth >= 0) out << "borderTopWidth=" << style.borderTopWidth << " ";
    if (style.borderRightWidth >= 0) out << "borderRightWidth=" << style.borderRightWidth << " ";
    if (style.borderBottomWidth >= 0) out << "borderBottomWidth=" << style.borderBottomWidth << " ";
    if (style.borderLeftWidth >= 0) out << "borderLeftWidth=" << style.borderLeftWidth << " ";
    if (style.borderColor.valid) AppendColor(out, "borderColor", style.borderColor);
    AppendColor(out, "borderTopColor", style.borderTopColor);
    AppendColor(out, "borderRightColor", style.borderRightColor);
    AppendColor(out, "borderBottomColor", style.borderBottomColor);
    AppendColor(out, "borderLeftColor", style.borderLeftColor);
    if (style.borderRadius > 0)  out << "borderRadius=" << style.borderRadius << " ";
    if (style.lineHeight   > 0)  out << "lineHeight="   << style.lineHeight   << " ";
    if (style.textAlignSet)      out << "textAlign="    << style.textAlign    << " ";
    if (style.width        >= 0) out << "width="        << style.width        << " ";
    if (style.widthPercent >= 0) out << "widthPercent=" << style.widthPercent << " ";
    if (style.height       >= 0) out << "height="       << style.height       << " ";
    if (style.maxWidth     >= 0) out << "maxWidth="     << style.maxWidth     << " ";
    if (style.minHeight    >= 0) out << "minHeight="    << style.minHeight    << " ";
    if (style.maxHeight    >= 0) out << "maxHeight="    << style.maxHeight    << " ";
    if (style.contentSet) out << "content=" << style.content << " ";
    if (!style.backgroundImage.empty()) out << "backgroundImage=" << style.backgroundImage << " ";
    if (style.floatMode == 1)    out << "float=left ";
    if (style.floatMode == 2)    out << "float=right ";
    if (style.clearMode == 1)    out << "clear=left ";
    if (style.clearMode == 2)    out << "clear=right ";
    if (style.clearMode == 3)    out << "clear=both ";
    if (style.positionMode == 1) out << "position=relative ";
    if (style.positionMode == 2) out << "position=absolute ";
    if (style.positionMode == 3) out << "position=fixed ";
    if (style.zIndexSet) out << "zIndex=" << style.zIndex << " ";
    if (style.overflowHidden)    out << "overflow=hidden ";
    if (style.topSet)    out << "top="    << style.top    << " ";
    if (style.rightSet)  out << "right="  << style.right  << " ";
    if (style.bottomSet) out << "bottom=" << style.bottom << " ";
    if (style.leftSet)   out << "left="   << style.left   << " ";
    out << "\n";
    return out.str();
}

std::string SerializeStylesheet(const Stylesheet& sheet) {
    std::ostringstream out;
    for (const auto& rule : sheet.rules) {
        out << "rule";
        if (!rule.tag.empty()) out << " tag=" << rule.tag;
        if (!rule.cls.empty()) out << " class=" << rule.cls;
        if (!rule.id.empty()) out << " id=" << rule.id;
        out << " specificity=" << rule.specificity() << " ";
        out << SerializeComputedStyle(rule.style);
    }
    return out.str();
}

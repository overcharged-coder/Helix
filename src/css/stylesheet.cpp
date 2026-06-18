#include "css/stylesheet.h"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <map>

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
        C("transparent",0,0,0),
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

static void ApplyDeclaration(const std::string& prop,
                             const std::string& val,
                             ComputedStyle& out) {
    if (prop == "color") {
        out.color = ParseCssColor(val);
    } else if (prop == "background-color" || prop == "background") {
        out.bgColor = ParseCssColor(val);
    } else if (prop == "font-size") {
        std::string v = sLower(val);
        if      (v == "small")    out.fontSize = 12;
        else if (v == "medium")   out.fontSize = 15;
        else if (v == "large")    out.fontSize = 18;
        else if (v == "x-large")  out.fontSize = 22;
        else if (v == "xx-large") out.fontSize = 28;
        else try { out.fontSize = std::stof(v); } catch (...) {}
    } else if (prop == "font-weight") {
        std::string v = sLower(val);
        out.boldSet = true;
        out.bold = (v == "bold" || v == "700" || v == "800" || v == "900");
    } else if (prop == "font-style") {
        out.italicSet = true;
        out.italic = (sLower(val) == "italic" || sLower(val) == "oblique");
    } else if (prop == "text-decoration" || prop == "text-decoration-line") {
        out.underline = (sLower(val).find("underline") != std::string::npos);
    } else if (prop == "display") {
        out.displayNone = (sLower(val) == "none");
    } else if (prop == "margin" || prop == "margin-top") {
        try { out.marginTop = std::stof(val); } catch (...) {}
        if (prop == "margin") try { out.marginBottom = std::stof(val); } catch (...) {}
    } else if (prop == "margin-bottom") {
        try { out.marginBottom = std::stof(val); } catch (...) {}
    }
}

ComputedStyle ParseInlineStyle(const std::string& style) {
    ComputedStyle out;
    std::istringstream ss(style);
    std::string decl;
    while (std::getline(ss, decl, ';')) {
        size_t colon = decl.find(':');
        if (colon == std::string::npos) continue;
        ApplyDeclaration(sLower(sTrim(decl.substr(0, colon))),
                         sTrim(decl.substr(colon+1)), out);
    }
    return out;
}

// ─── selector matching ───────────────────────────────────────────────────────

static bool MatchesSimpleSelector(const CssSelectorPart& part, const Node* node) {
    if (!node || node->type != NodeType::Element) return false;
    if (!part.tag.empty() && node->tagName != part.tag) return false;
    if (!part.id.empty() && node->attr("id") != part.id) return false;
    if (!part.cls.empty()) {
        auto ca = node->attr("class");
        bool found = false;
        std::istringstream ss(ca);
        std::string tok;
        while (ss >> tok) if (tok == part.cls) { found = true; break; }
        if (!found) return false;
    }
    return true;
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
               + (!part.cls.empty() ? 10 : 0)
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
        if (mode == 't') part.tag = sLower(cur);
        if (mode == 'c') part.cls = cur;
        if (mode == 'i') part.id  = cur;
        cur.clear();
    };
    for (char c : sel) {
        if (c == '.') { flush(); mode = 'c'; }
        else if (c == '#') { flush(); mode = 'i'; }
        else if (c == ':' || c == '[') break; // skip pseudo-classes/attrs
        else cur += c;
    }
    flush();
    // "*" or empty tag = universal
    if (part.tag == "*") part.tag.clear();
    return part;
}

static std::vector<CssSelectorPart> parseSelectorChain(std::string selector) {
    std::string spaced;
    spaced.reserve(selector.size() + 4);
    for (char c : selector) {
        if (c == '>') spaced += " > ";
        else spaced += c;
    }

    std::vector<CssSelectorPart> parts;
    std::istringstream ss(spaced);
    std::string tok;
    char nextCombinator = 0;
    while (ss >> tok) {
        if (tok == ">") {
            nextCombinator = '>';
            continue;
        }
        CssSelectorPart part = parseSimpleSelectorPart(tok);
        part.combinator = parts.empty() ? 0 : (nextCombinator ? nextCombinator : ' ');
        parts.push_back(part);
        nextCombinator = ' ';
    }
    return parts;
}

static CssRule parseSelector(const std::string& sel) {
    CssRule rule;
    rule.selector = parseSelectorChain(sel);
    if (!rule.selector.empty()) {
        const auto& last = rule.selector.back();
        rule.tag = last.tag;
        rule.cls = last.cls;
        rule.id = last.id;
    }
    return rule;
}

Stylesheet ParseStylesheet(const std::string& rawCss) {
    Stylesheet sheet;
    std::string css = stripComments(rawCss);

    size_t pos = 0;
    while (pos < css.size()) {
        // Find next '{'
        size_t lbrace = css.find('{', pos);
        if (lbrace == std::string::npos) break;
        size_t rbrace = css.find('}', lbrace);
        if (rbrace == std::string::npos) break;

        std::string selectorBlock = sTrim(css.substr(pos, lbrace - pos));
        std::string declBlock     = css.substr(lbrace+1, rbrace - lbrace - 1);
        pos = rbrace + 1;

        if (selectorBlock.empty()) continue;

        // Parse declarations once
        ComputedStyle declStyle;
        std::istringstream ds(declBlock);
        std::string decl;
        while (std::getline(ds, decl, ';')) {
            size_t colon = decl.find(':');
            if (colon == std::string::npos) continue;
            ApplyDeclaration(sLower(sTrim(decl.substr(0, colon))),
                             sTrim(decl.substr(colon+1)), declStyle);
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
    if (style.displayNone) out << "display=none ";
    if (style.marginTop >= 0) out << "marginTop=" << style.marginTop << " ";
    if (style.marginBottom >= 0) out << "marginBottom=" << style.marginBottom << " ";
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

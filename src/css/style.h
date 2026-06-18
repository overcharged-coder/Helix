#pragma once
#include <string>

// A resolved CSS color value
struct CssColor {
    bool  valid = false;
    float r = 0, g = 0, b = 0, a = 1;
};
CssColor ParseCssColor(const std::string& s);

// Fully computed style for one element (post-cascade)
struct ComputedStyle {
    CssColor color;
    CssColor bgColor;
    float    fontSize    = 0;     // 0 = inherit
    bool     bold        = false;
    bool     boldSet     = false;
    bool     italic      = false;
    bool     italicSet   = false;
    bool     underline   = false;
    bool     displayNone = false;
    float    marginTop   = -1;    // -1 = not set
    float    marginBottom = -1;

    // Merge child on top of this (parent) — returns new computed style
    ComputedStyle inherit(const ComputedStyle& child) const {
        ComputedStyle out = *this;
        if (child.color.valid)   out.color = child.color;
        if (child.bgColor.valid) out.bgColor = child.bgColor;
        if (child.fontSize > 0)  out.fontSize = child.fontSize;
        if (child.boldSet)   { out.bold = child.bold; out.boldSet = true; }
        if (child.italicSet) { out.italic = child.italic; out.italicSet = true; }
        if (child.underline) out.underline = true;
        if (child.displayNone) out.displayNone = true;
        if (child.marginTop    >= 0) out.marginTop    = child.marginTop;
        if (child.marginBottom >= 0) out.marginBottom = child.marginBottom;
        return out;
    }
};

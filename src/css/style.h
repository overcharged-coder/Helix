#pragma once
#include <string>
#include <cmath>

static constexpr float kCssNotSet = -1e6f;
static constexpr float kCssAuto   = -2.f;

struct CssColor {
    bool  valid = false;
    float r = 0, g = 0, b = 0, a = 1;
};
CssColor ParseCssColor(const std::string& s);

struct ComputedStyle {
    CssColor    color;
    CssColor    bgColor;
    std::string fontFamily;          // "" = default
    std::string backgroundImage;     // URL from background-image: url(...)
    bool     bgColorSet   = false;
    bool     backgroundImageSet = false;
    bool     bgNoRepeat   = false;
    bool     bgFixed      = false;
    // Background placement (CSS sprites). repeat: 0=repeat,1=repeat-x,2=repeat-y,3=no-repeat.
    int      bgRepeat     = 0;
    bool     bgRepeatSet  = false;
    float    bgPosX       = 0;     // position offset; px unless bgPosXPct
    float    bgPosY       = 0;
    bool     bgPosXPct    = false; // bgPosX is a percentage (0..100)
    bool     bgPosYPct    = false;
    bool     bgPosSet     = false;
    int      bgSizeMode   = 0;     // 0=auto, 1=cover, 2=contain, 3=length
    float    bgSizeW      = -1;    // when mode=length (px; -1=auto dimension)
    float    bgSizeH      = -1;
    bool     bgSizeWPct   = false;
    bool     bgSizeHPct   = false;
    bool     bgSizeSet    = false;
    float    fontSize     = 0;
    bool     bold         = false;
    bool     boldSet      = false;
    bool     italic       = false;
    bool     italicSet    = false;
    bool     underline    = false;
    // Display: 0=unset, 1=block, 2=inline, 3=none, 4=flex, 5=table, 6=table-cell
    int      display      = 0;
    // Box model (kCssNotSet = not set, kCssAuto = auto for margins)
    float    marginTop    = kCssNotSet;
    float    marginRight  = kCssNotSet;
    float    marginBottom = kCssNotSet;
    float    marginLeft   = kCssNotSet;
    float    paddingTop   = -1;
    float    paddingRight = -1;
    float    paddingBottom= -1;
    float    paddingLeft  = -1;
    float    borderWidth  = -1;
    float    borderTopWidth    = -1;
    float    borderRightWidth  = -1;
    float    borderBottomWidth = -1;
    float    borderLeftWidth   = -1;
    CssColor borderColor;
    CssColor borderTopColor;
    CssColor borderRightColor;
    CssColor borderBottomColor;
    CssColor borderLeftColor;
    float    borderRadius = 0;
    // Text
    float    lineHeight      = 0;
    int      textAlign       = 0;      // 0=left, 1=center, 2=right
    bool     textAlignSet    = false;
    int      textTransform   = 0;      // 0=none, 1=uppercase, 2=lowercase, 3=capitalize
    bool     textTransformSet= false;
    bool     whiteSpaceNowrap= false;
    bool     whiteSpaceSet   = false;
    bool     whiteSpacePre   = false;
    // Sizing
    float    width        = -1;
    float    widthPercent = -1;
    float    height       = -1;
    float    heightPercent = -1;
    float    maxWidth     = -1;
    float    maxWidthPercent = -1;
    float    minWidth     = -1;
    float    minWidthPercent = -1;
    float    minHeight    = -1;
    float    minHeightPercent = -1;
    float    maxHeight    = -1;
    float    maxHeightPercent = -1;
    // Generated content. This is only meaningful for :before/:after boxes.
    bool     contentSet   = false;
    std::string content;
    // Layout
    int      floatMode    = 0;       // 0=none, 1=left, 2=right
    bool     floatInherit = false;
    int      clearMode    = 0;       // 0=none, 1=left, 2=right, 3=both
    int      positionMode = 0;       // 0=static, 1=relative, 2=absolute, 3=fixed
    int      zIndex       = 0;
    bool     zIndexSet    = false;
    bool     overflowHidden = false;
    bool     overflowSet    = false;
    float    top          = 0;
    float    right        = 0;
    float    bottom       = 0;
    float    left         = 0;
    bool     topSet       = false;
    bool     rightSet     = false;
    bool     bottomSet    = false;
    bool     leftSet      = false;
    // Visibility
    bool     visibilityHidden = false;
    bool     visibilitySet    = false;
    // List style
    bool     listStyleNone    = false;
    bool     listStyleSet     = false;
    // Border-spacing
    float    borderSpacing    = -1;
    // Baseline flex formatting values.
    int      flexDirection   = 0; // 0=row, 1=column
    bool     flexDirectionSet = false;
    float    flexGrow        = 0;
    bool     flexGrowSet     = false;
    float    flexGap         = -1;

    bool isDisplayNone()        const { return display == 3; }
    bool isDisplayBlock()       const { return display == 1; }
    bool isDisplayInline()      const { return display == 2; }
    bool isDisplayFlex()        const { return display == 4; }
    bool isDisplayTable()       const { return display == 5; }
    bool isDisplayTableCell()   const { return display == 6; }
    bool isDisplayInlineBlock() const { return display == 7; }
    bool isDisplayListItem()    const { return display == 8; }
    bool isDisplayTableRow()    const { return display == 9; }
    bool isDisplayTableRowGroup() const { return display == 10; }
    bool marginTopSet()       const { return marginTop    > kCssNotSet + 1.f; }
    bool marginRightSet()     const { return marginRight  > kCssNotSet + 1.f; }
    bool marginBottomSet()    const { return marginBottom > kCssNotSet + 1.f; }
    bool marginLeftSet()      const { return marginLeft   > kCssNotSet + 1.f; }
    bool isMarginAuto(float v) const { return v > kCssAuto - 0.5f && v < kCssAuto + 0.5f; }

    ComputedStyle inherit(const ComputedStyle& child) const {
        ComputedStyle out = *this;
        if (child.color.valid)       out.color   = child.color;
        if (child.bgColorSet)       { out.bgColor = child.bgColor; out.bgColorSet = true; }
        if (!child.fontFamily.empty())      out.fontFamily      = child.fontFamily;
        if (child.backgroundImageSet) {
            out.backgroundImage = child.backgroundImage;
            out.backgroundImageSet = true;
            out.bgNoRepeat = child.bgNoRepeat;
            out.bgFixed = child.bgFixed;
        }
        if (child.bgRepeatSet) {
            out.bgRepeat = child.bgRepeat; out.bgNoRepeat = (child.bgRepeat == 3);
            out.bgRepeatSet = true;
        }
        if (child.bgPosSet) {
            out.bgPosX = child.bgPosX; out.bgPosY = child.bgPosY;
            out.bgPosXPct = child.bgPosXPct; out.bgPosYPct = child.bgPosYPct;
            out.bgPosSet = true;
        }
        if (child.bgSizeSet) {
            out.bgSizeMode = child.bgSizeMode;
            out.bgSizeW = child.bgSizeW; out.bgSizeH = child.bgSizeH;
            out.bgSizeWPct = child.bgSizeWPct; out.bgSizeHPct = child.bgSizeHPct;
            out.bgSizeSet = true;
        }
        if (child.fontSize > 0)      out.fontSize  = child.fontSize;
        if (child.boldSet)   { out.bold = child.bold; out.boldSet = true; }
        if (child.italicSet) { out.italic = child.italic; out.italicSet = true; }
        if (child.underline)    out.underline   = true;
        if (child.display != 0)     out.display      = child.display;
        if (child.marginTopSet())    out.marginTop    = child.marginTop;
        if (child.marginRightSet())  out.marginRight  = child.marginRight;
        if (child.marginBottomSet()) out.marginBottom = child.marginBottom;
        if (child.marginLeftSet())   out.marginLeft   = child.marginLeft;
        if (child.paddingTop   >= 0) out.paddingTop    = child.paddingTop;
        if (child.paddingRight >= 0) out.paddingRight  = child.paddingRight;
        if (child.paddingBottom>= 0) out.paddingBottom = child.paddingBottom;
        if (child.paddingLeft  >= 0) out.paddingLeft   = child.paddingLeft;
        if (child.borderWidth       >= 0) out.borderWidth       = child.borderWidth;
        if (child.borderTopWidth    >= 0) out.borderTopWidth    = child.borderTopWidth;
        if (child.borderRightWidth  >= 0) out.borderRightWidth  = child.borderRightWidth;
        if (child.borderBottomWidth >= 0) out.borderBottomWidth = child.borderBottomWidth;
        if (child.borderLeftWidth   >= 0) out.borderLeftWidth   = child.borderLeftWidth;
        if (child.borderColor.valid) out.borderColor   = child.borderColor;
        if (child.borderTopColor.valid) out.borderTopColor = child.borderTopColor;
        if (child.borderRightColor.valid) out.borderRightColor = child.borderRightColor;
        if (child.borderBottomColor.valid) out.borderBottomColor = child.borderBottomColor;
        if (child.borderLeftColor.valid) out.borderLeftColor = child.borderLeftColor;
        if (child.borderRadius > 0)  out.borderRadius  = child.borderRadius;
        if (child.lineHeight   > 0)  out.lineHeight    = child.lineHeight;
        if (child.textAlignSet) { out.textAlign = child.textAlign; out.textAlignSet = true; }
        if (child.textTransformSet) { out.textTransform = child.textTransform; out.textTransformSet = true; }
        if (child.whiteSpaceSet) { out.whiteSpaceNowrap = child.whiteSpaceNowrap; out.whiteSpacePre = child.whiteSpacePre; out.whiteSpaceSet = true; }
        if (child.width        >= 0) out.width     = child.width;
        if (child.widthPercent >= 0) out.widthPercent = child.widthPercent;
        if (child.height       >= 0) out.height    = child.height;
        if (child.heightPercent >= 0) out.heightPercent = child.heightPercent;
        if (child.maxWidth     >= 0) out.maxWidth  = child.maxWidth;
        if (child.maxWidthPercent >= 0) out.maxWidthPercent = child.maxWidthPercent;
        if (child.minWidth     >= 0) out.minWidth  = child.minWidth;
        if (child.minWidthPercent >= 0) out.minWidthPercent = child.minWidthPercent;
        if (child.minHeight    >= 0) out.minHeight = child.minHeight;
        if (child.minHeightPercent >= 0) out.minHeightPercent = child.minHeightPercent;
        if (child.maxHeight    >= 0) out.maxHeight = child.maxHeight;
        if (child.maxHeightPercent >= 0) out.maxHeightPercent = child.maxHeightPercent;
        if (child.contentSet) { out.contentSet = true; out.content = child.content; }
        if (child.floatMode    != 0) out.floatMode = child.floatMode;
        if (child.floatInherit)      out.floatInherit = true;
        if (child.clearMode    != 0) out.clearMode = child.clearMode;
        if (child.positionMode != 0) out.positionMode = child.positionMode;
        if (child.zIndexSet) { out.zIndex = child.zIndex; out.zIndexSet = true; }
        if (child.overflowSet)  { out.overflowHidden = child.overflowHidden; out.overflowSet = true; }
        if (child.topSet)    { out.top    = child.top;    out.topSet    = true; }
        if (child.rightSet)  { out.right  = child.right;  out.rightSet  = true; }
        if (child.bottomSet) { out.bottom = child.bottom; out.bottomSet = true; }
        if (child.leftSet)   { out.left   = child.left;   out.leftSet   = true; }
        if (child.visibilitySet) { out.visibilityHidden = child.visibilityHidden; out.visibilitySet = true; }
        if (child.listStyleSet) { out.listStyleNone = child.listStyleNone; out.listStyleSet = true; }
        if (child.borderSpacing >= 0) out.borderSpacing = child.borderSpacing;
        if (child.flexDirectionSet) {
            out.flexDirection = child.flexDirection;
            out.flexDirectionSet = true;
        }
        if (child.flexGrowSet) {
            out.flexGrow = child.flexGrow;
            out.flexGrowSet = true;
        }
        if (child.flexGap >= 0) out.flexGap = child.flexGap;
        return out;
    }
};

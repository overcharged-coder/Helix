#pragma once
//
// svg.h — minimal SVG renderer for Helix.
//
// Rasterizes basic SVG elements (rect, circle, ellipse, line, path) into a
// bitmap using a simple scanline fill. The bitmap is then drawn by the platform
// renderer as a replaced element (like an <img>).
//
#include "html/dom.h"
#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <sstream>
#include <algorithm>

struct SvgBitmap {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels; // RGBA
};

namespace svg {

struct Color { uint8_t r=0,g=0,b=0,a=255; };

inline Color parseColor(const std::string& s) {
    if (s.empty() || s == "none" || s == "transparent") return {0,0,0,0};
    if (s[0] == '#') {
        unsigned long v = 0;
        if (s.size() == 4) { // #RGB
            v = std::stoul(s.substr(1), nullptr, 16);
            uint8_t r = ((v>>8)&0xF)*17, g = ((v>>4)&0xF)*17, b = (v&0xF)*17;
            return {r,g,b,255};
        } else if (s.size() >= 7) { // #RRGGBB
            v = std::stoul(s.substr(1,6), nullptr, 16);
            return {(uint8_t)(v>>16),(uint8_t)((v>>8)&0xFF),(uint8_t)(v&0xFF),255};
        }
    }
    // Named colors (most common)
    if (s=="red") return {255,0,0,255}; if (s=="green") return {0,128,0,255};
    if (s=="blue") return {0,0,255,255}; if (s=="white") return {255,255,255,255};
    if (s=="black") return {0,0,0,255}; if (s=="gray"||s=="grey") return {128,128,128,255};
    if (s=="yellow") return {255,255,0,255}; if (s=="orange") return {255,165,0,255};
    if (s=="purple") return {128,0,128,255}; if (s=="pink") return {255,192,203,255};
    if (s=="cyan") return {0,255,255,255}; if (s=="brown") return {165,42,42,255};
    if (s=="navy") return {0,0,128,255}; if (s=="teal") return {0,128,128,255};
    if (s=="silver") return {192,192,192,255}; if (s=="gold") return {255,215,0,255};
    if (s=="darkgray"||s=="darkgrey") return {169,169,169,255};
    if (s=="lightgray"||s=="lightgrey") return {211,211,211,255};
    return {0,0,0,255};
}

inline float parseNum(const std::string& s) {
    try { return std::stof(s); } catch (...) { return 0; }
}

inline void blendPixel(SvgBitmap& bmp, int x, int y, Color c) {
    if (x < 0 || x >= bmp.width || y < 0 || y >= bmp.height || c.a == 0) return;
    int idx = (y * bmp.width + x) * 4;
    if (c.a == 255) {
        bmp.pixels[idx] = c.r; bmp.pixels[idx+1] = c.g;
        bmp.pixels[idx+2] = c.b; bmp.pixels[idx+3] = 255;
    } else {
        float a = c.a / 255.f, ia = 1.f - a;
        bmp.pixels[idx]   = (uint8_t)(c.r * a + bmp.pixels[idx]   * ia);
        bmp.pixels[idx+1] = (uint8_t)(c.g * a + bmp.pixels[idx+1] * ia);
        bmp.pixels[idx+2] = (uint8_t)(c.b * a + bmp.pixels[idx+2] * ia);
        bmp.pixels[idx+3] = std::max(bmp.pixels[idx+3], c.a);
    }
}

inline void fillRect(SvgBitmap& bmp, float x, float y, float w, float h, Color c) {
    int x0 = std::max(0, (int)x), y0 = std::max(0, (int)y);
    int x1 = std::min(bmp.width, (int)(x+w+0.5f)), y1 = std::min(bmp.height, (int)(y+h+0.5f));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
            blendPixel(bmp, px, py, c);
}

inline void fillCircle(SvgBitmap& bmp, float cx, float cy, float r, Color c) {
    int x0 = std::max(0, (int)(cx-r)), y0 = std::max(0, (int)(cy-r));
    int x1 = std::min(bmp.width, (int)(cx+r+1)), y1 = std::min(bmp.height, (int)(cy+r+1));
    float r2 = r*r;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px)
            if ((px-cx)*(px-cx)+(py-cy)*(py-cy) <= r2) blendPixel(bmp, px, py, c);
}

inline void fillEllipse(SvgBitmap& bmp, float cx, float cy, float rx, float ry, Color c) {
    int x0 = std::max(0, (int)(cx-rx)), y0 = std::max(0, (int)(cy-ry));
    int x1 = std::min(bmp.width, (int)(cx+rx+1)), y1 = std::min(bmp.height, (int)(cy+ry+1));
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            float dx = (px-cx)/rx, dy = (py-cy)/ry;
            if (dx*dx+dy*dy <= 1.f) blendPixel(bmp, px, py, c);
        }
}

inline void drawLine(SvgBitmap& bmp, float x1, float y1, float x2, float y2, Color c, float sw) {
    // Bresenham with thickness
    float dx = x2-x1, dy = y2-y1;
    float len = std::sqrt(dx*dx+dy*dy);
    if (len < 0.5f) return;
    float steps = std::max(std::abs(dx), std::abs(dy));
    float ix = dx/steps, iy = dy/steps;
    float half = sw / 2.f;
    for (float s = 0; s <= steps; s += 1.f) {
        float px = x1 + ix*s, py = y1 + iy*s;
        for (int ty = (int)(py-half); ty <= (int)(py+half); ++ty)
            for (int tx = (int)(px-half); tx <= (int)(px+half); ++tx)
                blendPixel(bmp, tx, ty, c);
    }
}

// Minimal SVG path parser: handles M, L, H, V, C, S, Q, Z (absolute + relative).
struct PathPoint { float x, y; };

inline void rasterizePath(SvgBitmap& bmp, const std::string& d, Color fill, Color stroke, float sw) {
    // Collect path segments as line segments for scanline fill.
    std::vector<PathPoint> poly;
    float cx = 0, cy = 0, startX = 0, startY = 0;

    size_t i = 0;
    auto skipWS = [&]() { while (i < d.size() && (d[i]==' '||d[i]==','||d[i]=='\n'||d[i]=='\r'||d[i]=='\t')) ++i; };
    auto readNum = [&]() -> float {
        skipWS();
        if (i >= d.size()) return 0;
        size_t start = i;
        if (d[i]=='-'||d[i]=='+') ++i;
        while (i < d.size() && (std::isdigit((unsigned char)d[i]) || d[i]=='.')) ++i;
        if (d[i]=='e'||d[i]=='E') { ++i; if (d[i]=='-'||d[i]=='+') ++i; while (i<d.size()&&std::isdigit((unsigned char)d[i])) ++i; }
        try { return std::stof(d.substr(start, i-start)); } catch (...) { return 0; }
    };

    while (i < d.size()) {
        skipWS();
        if (i >= d.size()) break;
        char cmd = d[i];
        if (std::isalpha((unsigned char)cmd)) { ++i; }
        else { cmd = 'L'; } // implicit lineto
        bool rel = (cmd >= 'a' && cmd <= 'z');
        char C = rel ? (cmd - 32) : cmd;

        switch (C) {
        case 'M': { float x = readNum(), y = readNum();
            if (rel) { x+=cx; y+=cy; }
            cx = startX = x; cy = startY = y;
            if (!poly.empty()) poly.clear();
            poly.push_back({cx, cy});
            break; }
        case 'L': { float x = readNum(), y = readNum();
            if (rel) { x+=cx; y+=cy; }
            cx = x; cy = y; poly.push_back({cx, cy}); break; }
        case 'H': { float x = readNum();
            if (rel) x += cx; cx = x; poly.push_back({cx, cy}); break; }
        case 'V': { float y = readNum();
            if (rel) y += cy; cy = y; poly.push_back({cx, cy}); break; }
        case 'C': { // cubic bezier: approximate with line segments
            float x1=readNum(),y1=readNum(),x2=readNum(),y2=readNum(),x=readNum(),y=readNum();
            if (rel) { x1+=cx;y1+=cy;x2+=cx;y2+=cy;x+=cx;y+=cy; }
            for (float t = 0.1f; t <= 1.01f; t += 0.1f) {
                float u=1-t;
                float px = u*u*u*cx + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x;
                float py = u*u*u*cy + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y;
                poly.push_back({px, py});
            }
            cx=x; cy=y; break; }
        case 'S': { // smooth cubic
            float x2=readNum(),y2=readNum(),x=readNum(),y=readNum();
            if (rel) { x2+=cx;y2+=cy;x+=cx;y+=cy; }
            for (float t = 0.1f; t <= 1.01f; t += 0.1f) {
                float u=1-t;
                float px = u*u*u*cx + 3*u*t*t*x2 + t*t*t*x;
                float py = u*u*u*cy + 3*u*t*t*y2 + t*t*t*y;
                poly.push_back({px, py});
            }
            cx=x; cy=y; break; }
        case 'Q': { // quadratic bezier
            float x1=readNum(),y1=readNum(),x=readNum(),y=readNum();
            if (rel) { x1+=cx;y1+=cy;x+=cx;y+=cy; }
            for (float t = 0.1f; t <= 1.01f; t += 0.1f) {
                float u=1-t;
                float px = u*u*cx + 2*u*t*x1 + t*t*x;
                float py = u*u*cy + 2*u*t*y1 + t*t*y;
                poly.push_back({px, py});
            }
            cx=x; cy=y; break; }
        case 'A': { // arc: simplified as line to endpoint
            readNum(); readNum(); readNum(); readNum(); readNum();
            float x=readNum(), y=readNum();
            if (rel) { x+=cx; y+=cy; }
            cx=x; cy=y; poly.push_back({cx, cy}); break; }
        case 'Z': {
            cx = startX; cy = startY;
            poly.push_back({cx, cy});
            // Scanline fill the polygon
            if (fill.a > 0 && poly.size() >= 3) {
                float minY=1e9,maxY=-1e9;
                for (auto& p : poly) { minY=std::min(minY,p.y); maxY=std::max(maxY,p.y); }
                for (int y = std::max(0,(int)minY); y <= std::min(bmp.height-1,(int)maxY); ++y) {
                    std::vector<float> xs;
                    for (size_t j = 0; j < poly.size()-1; ++j) {
                        float y0=poly[j].y, y1=poly[j+1].y;
                        if ((y0<=y&&y1>y)||(y1<=y&&y0>y)) {
                            float t = (y-y0)/(y1-y0);
                            xs.push_back(poly[j].x + t*(poly[j+1].x-poly[j].x));
                        }
                    }
                    std::sort(xs.begin(), xs.end());
                    for (size_t j = 0; j+1 < xs.size(); j+=2) {
                        int x0 = std::max(0,(int)xs[j]), x1 = std::min(bmp.width-1,(int)xs[j+1]);
                        for (int x = x0; x <= x1; ++x) blendPixel(bmp, x, y, fill);
                    }
                }
            }
            // Stroke
            if (stroke.a > 0 && sw > 0 && poly.size() >= 2) {
                for (size_t j = 0; j+1 < poly.size(); ++j)
                    drawLine(bmp, poly[j].x, poly[j].y, poly[j+1].x, poly[j+1].y, stroke, sw);
            }
            poly.clear();
            break; }
        default:
            ++i; break;
        }
    }
    // If path didn't close with Z, still fill/stroke what we have
    if (poly.size() >= 3 && fill.a > 0) {
        poly.push_back(poly[0]); // close
        float minY=1e9,maxY=-1e9;
        for (auto& p : poly) { minY=std::min(minY,p.y); maxY=std::max(maxY,p.y); }
        for (int y = std::max(0,(int)minY); y <= std::min(bmp.height-1,(int)maxY); ++y) {
            std::vector<float> xs;
            for (size_t j = 0; j < poly.size()-1; ++j) {
                float y0=poly[j].y, y1=poly[j+1].y;
                if ((y0<=y&&y1>y)||(y1<=y&&y0>y))
                    xs.push_back(poly[j].x + (y-y0)/(y1-y0)*(poly[j+1].x-poly[j].x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t j = 0; j+1 < xs.size(); j+=2) {
                int x0 = std::max(0,(int)xs[j]), x1 = std::min(bmp.width-1,(int)xs[j+1]);
                for (int x = x0; x <= x1; ++x) blendPixel(bmp, x, y, fill);
            }
        }
    }
}

inline void renderElement(SvgBitmap& bmp, const Node* node, float sx, float sy) {
    if (!node || node->type != NodeType::Element) return;

    std::string fill_s = node->attr("fill");
    std::string stroke_s = node->attr("stroke");
    Color fill = fill_s.empty() ? Color{0,0,0,255} : parseColor(fill_s);
    Color stroke = stroke_s.empty() ? Color{0,0,0,0} : parseColor(stroke_s);
    float sw = 1; try { sw = std::stof(node->attr("stroke-width")); } catch (...) {}
    if (fill_s == "none") fill.a = 0;
    if (stroke_s == "none") stroke.a = 0;

    const std::string& tag = node->tagName;
    if (tag == "rect") {
        float x = parseNum(node->attr("x"))+sx, y = parseNum(node->attr("y"))+sy;
        float w = parseNum(node->attr("width")), h = parseNum(node->attr("height"));
        if (fill.a > 0) fillRect(bmp, x, y, w, h, fill);
        if (stroke.a > 0) {
            drawLine(bmp,x,y,x+w,y,stroke,sw); drawLine(bmp,x+w,y,x+w,y+h,stroke,sw);
            drawLine(bmp,x+w,y+h,x,y+h,stroke,sw); drawLine(bmp,x,y+h,x,y,stroke,sw);
        }
    } else if (tag == "circle") {
        float cx = parseNum(node->attr("cx"))+sx, cy = parseNum(node->attr("cy"))+sy;
        float r = parseNum(node->attr("r"));
        if (fill.a > 0) fillCircle(bmp, cx, cy, r, fill);
    } else if (tag == "ellipse") {
        float cx = parseNum(node->attr("cx"))+sx, cy = parseNum(node->attr("cy"))+sy;
        float rx = parseNum(node->attr("rx")), ry = parseNum(node->attr("ry"));
        if (fill.a > 0) fillEllipse(bmp, cx, cy, rx, ry, fill);
    } else if (tag == "line") {
        float x1 = parseNum(node->attr("x1"))+sx, y1 = parseNum(node->attr("y1"))+sy;
        float x2 = parseNum(node->attr("x2"))+sx, y2 = parseNum(node->attr("y2"))+sy;
        Color c = stroke.a > 0 ? stroke : fill;
        drawLine(bmp, x1, y1, x2, y2, c, sw);
    } else if (tag == "path") {
        std::string d = node->attr("d");
        if (!d.empty()) rasterizePath(bmp, d, fill, stroke, sw);
    } else if (tag == "polygon" || tag == "polyline") {
        std::string pts = node->attr("points");
        std::vector<PathPoint> poly;
        std::istringstream ss(pts);
        float x, y; char comma;
        while (ss >> x) { ss >> comma; ss >> y; poly.push_back({x+sx,y+sy}); }
        if (tag == "polygon" && !poly.empty()) poly.push_back(poly[0]);
        if (fill.a > 0 && poly.size() >= 3) {
            float minY=1e9,maxY=-1e9;
            for (auto& p : poly) { minY=std::min(minY,p.y); maxY=std::max(maxY,p.y); }
            for (int iy = std::max(0,(int)minY); iy <= std::min(bmp.height-1,(int)maxY); ++iy) {
                std::vector<float> xs;
                for (size_t j=0;j+1<poly.size();++j) {
                    float y0=poly[j].y,y1=poly[j+1].y;
                    if ((y0<=iy&&y1>iy)||(y1<=iy&&y0>iy))
                        xs.push_back(poly[j].x+(iy-y0)/(y1-y0)*(poly[j+1].x-poly[j].x));
                }
                std::sort(xs.begin(),xs.end());
                for (size_t j=0;j+1<xs.size();j+=2)
                    for (int ix=std::max(0,(int)xs[j]);ix<=std::min(bmp.width-1,(int)xs[j+1]);++ix)
                        blendPixel(bmp,ix,iy,fill);
            }
        }
    }
    // Recurse into children (g, svg, etc.)
    for (auto& child : node->children)
        renderElement(bmp, child.get(), sx, sy);
}

// Render an <svg> element to a bitmap. Returns empty bitmap if no valid viewBox/width/height.
inline SvgBitmap renderSvg(const Node* svgNode, int maxDim = 512) {
    SvgBitmap bmp;
    if (!svgNode) return bmp;

    // Parse dimensions from viewBox or width/height attributes.
    float vx=0,vy=0,vw=0,vh=0;
    std::string vb = svgNode->attr("viewbox");
    if (vb.empty()) vb = svgNode->attr("viewBox");
    if (!vb.empty()) {
        std::istringstream ss(vb);
        char c; ss >> vx >> c >> vy >> c >> vw >> c >> vh;
        if (vw <= 0) { ss.clear(); ss.str(vb); ss >> vx >> vy >> vw >> vh; }
    }
    float w = parseNum(svgNode->attr("width"));
    float h = parseNum(svgNode->attr("height"));
    if (w <= 0 && vw > 0) w = vw;
    if (h <= 0 && vh > 0) h = vh;
    if (w <= 0 || h <= 0) return bmp;

    // Scale to fit maxDim while preserving aspect ratio.
    float scale = 1.f;
    if (w > maxDim || h > maxDim) scale = maxDim / std::max(w, h);
    bmp.width = std::max(1, (int)(w * scale));
    bmp.height = std::max(1, (int)(h * scale));
    bmp.pixels.resize(bmp.width * bmp.height * 4, 0);

    // If viewBox is set, map viewBox coords to pixel coords.
    float sx = -vx * scale, sy = -vy * scale;
    // TODO: apply viewBox-to-viewport transform (scale vw/vh to w/h)
    if (vw > 0 && vh > 0) {
        float vscale = std::min(w / vw, h / vh) * scale;
        // Re-render with viewBox scaling would need coordinate transform.
        // For now, just use the scale factor.
        (void)vscale;
    }

    for (auto& child : svgNode->children)
        renderElement(bmp, child.get(), sx, sy);

    return bmp;
}

} // namespace svg

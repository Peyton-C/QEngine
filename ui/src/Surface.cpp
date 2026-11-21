// Every panel is built from the same dozen pieces, drawn here with ImDrawList.
// Nothing in this file names a device; what a Prime GO or an MPC One looks
// like is entirely in its layout file.

#include "Surface.hpp"

#include "imgui_internal.h"

#include <algorithm>
#include <utility>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <vector>

using nlohmann::json;

namespace {

constexpr float kPi = 3.14159265f;

ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
            a.w + (b.w - a.w) * t};
}

ImVec4 lighten(ImVec4 c, float t) { return mix(c, {1, 1, 1, c.w}, t); }
ImVec4 darken(ImVec4 c, float t) { return mix(c, {0, 0, 0, c.w}, t); }

ImVec4 alpha(ImVec4 c, float a) {
    c.w *= a;
    return c;
}

float hash(const std::string& s) {
    unsigned h = 2166136261u;

    for (char ch : s) {
        h = (h ^ unsigned(ch)) * 16777619u;
    }

    return float(h % 1000) / 1000.0f;
}

// Clockwise from twelve o'clock, as a knob is read, to ImGui's angles, which
// start at three o'clock.
float imAngle(float a) { return a - kPi * 0.5f; }

ImVec2 polar(ImVec2 c, float r, float a) { return {c.x + std::sin(a) * r, c.y - std::cos(a) * r}; }

// Turns millimetres into screen pixels.
struct Painter {
    ImDrawList* dl = nullptr;
    ImVec2 origin;
    float s = 1.0f;
    const Fonts* fonts = nullptr;
    float labelSize = 2.4f;
    float textScale = 1.0f;

    ImVec2 P(ImVec2 mm) const { return origin + mm * s; }
    float L(float mm) const { return mm * s; }

    // Text sizes in a layout are the height of capitals, which is what can
    // be measured off a photo. A font's size is its em, about 1.4 times that.
    float fontPx(float capMm) const { return L(capMm) * textScale / 0.72f; }

    ImU32 C(ImVec4 c) const { return ImGui::ColorConvertFloat4ToU32(c); }

    // Shades whatever was drawn since `start` from c0 at the top to c1 at
    // the bottom, keeping each vertex's alpha so anti-aliased edges survive.
    void shade(int start, float top, float bottom, ImVec4 c0, ImVec4 c1) const {
        ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, start, dl->VtxBuffer.Size, {0, top},
                                                      {0, bottom}, C(c0), C(c1));
    }

    void rect(ImVec2 p0, ImVec2 p1, float r, ImVec4 top, ImVec4 bottom) const {
        const int start = dl->VtxBuffer.Size;
        dl->AddRectFilled(p0, p1, C(top), r);
        shade(start, p0.y, p1.y, top, bottom);
    }

    void circle(ImVec2 c, float r, ImVec4 top, ImVec4 bottom) const {
        const int start = dl->VtxBuffer.Size;
        dl->AddCircleFilled(c, r, C(top), 0);
        shade(start, c.y - r, c.y + r, top, bottom);
    }

    // Soft shadows and glows are stacks of translucent shapes, each a little
    // larger. Cheaper than a blur pass, and fine at these sizes.
    void haloRect(ImVec2 p0, ImVec2 p1, float r, float spread, ImVec2 offset, ImVec4 c,
                  float strength) const {
        constexpr int steps = 7;

        for (int i = steps; i >= 1; --i) {
            const float t = float(i) / steps;
            const float grow = spread * t;
            dl->AddRectFilled(p0 - ImVec2(grow, grow) + offset, p1 + ImVec2(grow, grow) + offset,
                              C(alpha(c, strength * (1.0f - t) * 0.45f + strength * 0.04f)),
                              r + grow);
        }
    }

    void haloCircle(ImVec2 c, float r, float spread, ImVec2 offset, ImVec4 col,
                    float strength) const {
        constexpr int steps = 7;

        for (int i = steps; i >= 1; --i) {
            const float t = float(i) / steps;
            dl->AddCircleFilled(c + offset, r + spread * t,
                                C(alpha(col, strength * (1.0f - t) * 0.45f + strength * 0.04f)),
                                0);
        }
    }

    // A disc of circularly brushed metal: one triangle fan with a colour per
    // rim vertex. The brushing is round, so its highlight is two wedges fixed
    // to the light, not to the part, which is why it never rotates.
    void metal(ImVec2 c, float r, ImVec4 dark, ImVec4 light, float sharpness = 14.0f) const {
        const int n = 120;
        const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
        const float lightAngle = -0.7f;

        auto shine = [&](float a) {
            const float k = std::fabs(std::cos(a - lightAngle));
            return 0.7f * std::pow(k, sharpness) + 0.16f * std::pow(k, 2.0f);
        };

        dl->PrimReserve(n * 3, n + 1);
        const ImDrawIdx base = ImDrawIdx(dl->_VtxCurrentIdx);
        dl->PrimWriteVtx(c, uv, C(mix(dark, light, 0.22f)));

        for (int i = 0; i < n; ++i) {
            const float a = 2.0f * 3.14159265f * float(i) / float(n);
            dl->PrimWriteVtx(c + ImVec2(std::cos(a), std::sin(a)) * r, uv, C(mix(dark, light, shine(a))));
        }

        for (int i = 0; i < n; ++i) {
            dl->PrimWriteIdx(base);
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + i));
            dl->PrimWriteIdx(ImDrawIdx(base + 1 + (i + 1) % n));
        }

        // The fan has no anti-aliased edge, so draw one.
        dl->AddCircle(c, r, C(mix(dark, light, 0.3f)), 0, 1.0f);
    }

    float textWidth(ImFont* font, float px, const char* b, const char* e, float tracking) const {
        float w = 0.0f;
        int n = 0;

        for (const char* c = b; c < e; ++c, ++n) {
            w += font->CalcTextSizeA(px, FLT_MAX, 0.0f, c, c + 1).x;
        }

        return w + tracking * px * float(std::max(0, n - 1));
    }

    // Text placed by an anchor in mm: ax and ay pick which point of the text
    // block sits on it, 0.5 for the middle. Lines split on \n.
    void text(ImFont* font, float sizeMm, ImVec2 anchor, ImVec4 col, const std::string& s,
              float ax = 0.5f, float ay = 0.5f, float tracking = 0.0f) const {
        if (s.empty() || !font) {
            return;
        }

        const float px = fontPx(sizeMm);

        // Below a few pixels it is noise, and a fresh glyph size each frame
        // while zooming is wasted atlas space.
        if (px < 3.0f) {
            return;
        }

        const float lineH = px * 1.02f;
        const int lines = 1 + int(std::count(s.begin(), s.end(), '\n'));
        const ImVec2 a = P(anchor);
        float y = a.y - ay * lineH * float(lines);
        size_t start = 0;

        while (start <= s.size()) {
            size_t end = s.find('\n', start);

            if (end == std::string::npos) {
                end = s.size();
            }

            const char* b = s.c_str() + start;
            const char* e = s.c_str() + end;
            float x = a.x - ax * textWidth(font, px, b, e, tracking);

            if (tracking == 0.0f) {
                dl->AddText(font, px, {x, y}, C(col), b, e);
            } else {
                for (const char* c = b; c < e; ++c) {
                    dl->AddText(font, px, {x, y}, C(col), c, c + 1);
                    x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, c, c + 1).x + tracking * px;
                }
            }

            y += lineH;
            start = end + 1;
        }
    }
};

// ---------------------------------------------------------------- pieces

// Brushed aluminium: hairlines of slightly different brightness. The pattern
// is fixed per line, so it does not crawl as the window resizes.
void brush(const Painter& g, ImVec2 p0, ImVec2 p1) {
    g.dl->PushClipRect(p0, p1, true);
    const float step = std::max(1.0f, g.L(0.3f));
    int i = 0;

    for (float y = p0.y; y < p1.y; y += step, ++i) {
        const float n = std::fmod(std::sin(float(i) * 12.9898f) * 43758.5453f, 1.0f);
        const ImVec4 line = n > 0 ? ImVec4(1, 1, 1, 0.035f * n) : ImVec4(0, 0, 0, -0.08f * n);
        g.dl->AddLine({p0.x, y}, {p1.x, y}, g.C(line), 1.0f);
    }

    g.dl->PopClipRect();
}

void drawBody(const Painter& g, const Face& face) {
    const json& b = face.body;

    // A device that is not one slab, such as a Prime 4 with its screen
    // standing above the body, draws its outline from panels instead.
    if (flag(b, "none")) {
        return;
    }

    const ImVec4 colour = ::colour(b, "color", {0.1f, 0.1f, 0.1f, 1});
    const ImVec4 shade = ::colour(b, "shade", darken(colour, 0.2f));
    const float r = g.L(num(b, "radius", 3.0f));
    const ImVec2 p0 = g.P({face.rect.x, face.rect.y});
    const ImVec2 p1 = g.P({face.rect.x + face.rect.z, face.rect.y + face.rect.w});

    g.haloRect(p0, p1, r, g.L(10), {0, g.L(4)}, {0, 0, 0, 1}, 0.9f);
    g.rect(p0, p1, r, lighten(colour, 0.03f), shade);

    if (flag(b, "brushed")) {
        brush(g, p0, p1);
    }

    // A lit top edge and a dark bottom one read as a slab with thickness.
    g.dl->AddRect(p0, p1, g.C(alpha(::colour(b, "edge", lighten(colour, 0.2f)), 0.55f)), r, 0,
                  std::max(1.0f, g.L(0.35f)));
}

void drawPanel(const Painter& g, const json& p) {
    const ImVec4 r = rect(p, "rect");
    const ImVec2 p0 = g.P({r.x, r.y});
    const ImVec2 p1 = g.P({r.x + r.z, r.y + r.w});
    const float round = g.L(num(p, "radius", 1.0f));
    const ImVec4 c = colour(p, "color", {0.1f, 0.1f, 0.1f, 1});

    g.rect(p0, p1, round, lighten(c, 0.035f), darken(c, 0.06f));

    if (flag(p, "brushed")) {
        brush(g, p0, p1);
    }

    // A speaker grille: a staggered grid of holes.
    if (str(p, "pattern") == "dots") {
        const float pitch = std::max(3.0f, g.L(1.5f));
        const float hole = std::max(0.8f, g.L(0.45f));
        const float inset = round * 0.6f + pitch * 0.5f;
        int row = 0;

        for (float y = p0.y + inset; y < p1.y - inset * 0.6f; y += pitch * 0.87f, ++row) {
            for (float x = p0.x + inset + (row % 2 ? pitch * 0.5f : 0.0f); x < p1.x - inset * 0.6f; x += pitch) {
                g.dl->AddCircleFilled({x, y}, hole, g.C({0, 0, 0, 0.85f}), 8);
            }
        }
    }

    if (hasColour(p, "edge")) {
        g.dl->AddRect(p0, p1, g.C(colour(p, "edge", {})), round, 0, 1.0f);
    }
}

void drawFrame(const Painter& g, const json& p) {
    const ImVec4 r = rect(p, "rect");
    const ImVec4 c = colour(p, "color", {0.8f, 0.8f, 0.8f, 1});
    g.dl->AddRect(g.P({r.x, r.y}), g.P({r.x + r.z, r.y + r.w}), g.C(c), g.L(0.8f), 0,
                  std::max(1.0f, g.L(0.3f)));

    const std::string label = str(p, "label");

    if (!label.empty()) {
        const float size = num(p, "label_size", g.labelSize);
        const float w = g.textWidth(g.fonts->label, g.fontPx(size), label.data(),
                                    label.data() + label.size(), 0) / g.s;
        const ImVec2 at = {r.x + r.z * 0.5f, r.y};
        const ImVec4 bg = colour(p, "bg", {0.078f, 0.082f, 0.094f, 1});
        g.dl->AddRectFilled(g.P(at - ImVec2(w * 0.5f + 1.0f, size * 0.6f)),
                            g.P(at + ImVec2(w * 0.5f + 1.0f, size * 0.6f)), g.C(bg));
        g.text(g.fonts->label, size, at, c, label);
    }
}

void drawLine(const Painter& g, const json& p) {
    g.dl->AddLine(g.P(vec2(p, "from")), g.P(vec2(p, "to")),
                  g.C(colour(p, "color", {0.5f, 0.5f, 0.5f, 1})),
                  std::max(1.0f, g.L(num(p, "width", 0.3f))));
}

void drawText(const Painter& g, const json& p) {
    const std::string font = str(p, "font", "label");
    ImFont* f = font == "title" ? g.fonts->title : g.fonts->label;
    const std::string align = str(p, "align", "center");
    const float ax = align == "left" ? 0.0f : align == "right" ? 1.0f : 0.5f;
    g.text(f, num(p, "size", g.labelSize), vec2(p, "at"), colour(p, "color", {1, 1, 1, 1}),
           str(p, "text"), ax, 0.5f, num(p, "tracking", 0.0f) * 0.1f);
}

void drawLed(const Painter& g, const json& p, bool lit) {
    const ImVec2 c = g.P(vec2(p, "at"));
    const float r = g.L(num(p, "size", 1.2f) * 0.5f);
    const ImVec4 col = colour(p, "color", {1, 0.2f, 0.2f, 1});
    const std::string shape = str(p, "shape");

    // An arrow, as beside a pitch fader to say which way it is from the
    // tempo playing.
    if (shape == "up" || shape == "down") {
        const float d = shape == "up" ? -1.0f : 1.0f;
        const ImVec2 a = c + ImVec2(0, d * r);
        const ImVec2 b = c + ImVec2(-r, -d * r * 0.7f);
        const ImVec2 e = c + ImVec2(r, -d * r * 0.7f);

        if (lit) {
            g.haloCircle(c, r * 0.7f, g.L(1.4f), {0, 0}, col, 0.45f);
        }

        g.dl->AddTriangleFilled(a, b, e, g.C(lit ? col : mix(col, {0.12f, 0.12f, 0.13f, 1}, 0.78f)));
        return;
    }

    if (lit) {
        g.haloCircle(c, r, g.L(1.6f), {0, 0}, col, 0.5f);
        g.dl->AddCircleFilled(c, r, g.C(col));
        g.dl->AddCircleFilled(c, r * 0.45f, g.C(lighten(col, 0.7f)));
    } else {
        g.dl->AddCircleFilled(c, r, g.C(mix(col, {0.12f, 0.12f, 0.13f, 1}, 0.78f)));
    }
}

void drawMeter(const Painter& g, const json& p, float level, float peak = -1.0f) {
    const ImVec4 r = rect(p, "rect");
    const int n = std::max(1, int(num(p, "segments", 10)));
    const bool vertical = r.w > r.z;
    const float gap = num(p, "gap", (vertical ? r.w : r.z) / float(n) * 0.3f);
    const float seg = ((vertical ? r.w : r.z) - gap * float(n - 1)) / float(n);

    std::vector<ImVec4> colours;

    if (p.contains("colors")) {
        for (const json& c : p["colors"]) {
            colours.push_back(parseColour(c.get<std::string>(), {0.3f, 1, 0.4f, 1}));
        }
    }

    if (colours.empty()) {
        colours.push_back({0.3f, 1, 0.4f, 1});
    }

    for (int i = 0; i < n; ++i) {
        const ImVec4 c = colours[size_t(i) * colours.size() / size_t(n)];
        // The peak lights the one segment it holds at.
        const int held = peak > 0 ? std::clamp(int(std::ceil(peak * float(n))) - 1, 0, n - 1) : -1;
        const bool on = (float(i) + 0.5f) / float(n) <= level || i == held;
        const float along = float(i) * (seg + gap);
        ImVec2 p0, p1;

        // Vertical meters fill from the bottom.
        if (vertical) {
            p0 = {r.x, r.y + r.w - along - seg};
            p1 = {r.x + r.z, r.y + r.w - along};
        } else {
            p0 = {r.x + along, r.y};
            p1 = {r.x + along + seg, r.y + r.w};
        }

        const ImU32 col = g.C(on ? c : mix(c, {0.07f, 0.07f, 0.08f, 1}, 0.8f));

        if (flag(p, "round")) {
            const ImVec2 mid = (g.P(p0) + g.P(p1)) * 0.5f;
            const float r = std::min(g.L(p1.x - p0.x), g.L(p1.y - p0.y)) * 0.5f;

            if (on) {
                g.haloCircle(mid, r, r * 1.2f, {0, 0}, c, 0.35f);
            }
            g.dl->AddCircleFilled(mid, r, col);
        } else {
            g.dl->AddRectFilled(g.P(p0), g.P(p1), col, g.L(0.15f));
        }
    }
}

// Printed accent marks: a row or column of short bars in one colour. They
// look like a meter but light nothing.
void drawHatch(const Painter& g, const json& p) {
    const ImVec4 r = rect(p, "rect");
    const int n = std::max(1, int(num(p, "segments", 10)));
    const bool vertical = r.w > r.z;
    const float pitch = (vertical ? r.w : r.z) / float(n);
    const float bar = pitch * num(p, "fill", 0.55f);
    const ImU32 col = g.C(colour(p, "color", {0.36f, 0.37f, 0.39f, 1}));

    for (int i = 0; i < n; ++i) {
        const float along = float(i) * pitch + (pitch - bar) * 0.5f;
        const ImVec2 p0 = vertical ? ImVec2(r.x, r.y + along) : ImVec2(r.x + along, r.y);
        const ImVec2 p1 = vertical ? ImVec2(r.x + r.z, r.y + along + bar) : ImVec2(r.x + along + bar, r.y + r.w);
        g.dl->AddRectFilled(g.P(p0), g.P(p1), col);
    }
}

// The part of an lcd the guest's picture fills: all of it, unless the
// guest's shape differs from the layout's, when it is centred at its own.
void lcdPicture(ImVec2 p0, ImVec2 p1, ImVec2 size, ImVec2& min, ImVec2& max) {
    min = p0;
    max = p1;

    if (size.x <= 0 || size.y <= 0) {
        return;
    }

    const ImVec2 box = p1 - p0;
    const float s = std::min(box.x / size.x, box.y / size.y);
    min = p0 + (box - size * s) * 0.5f;
    max = min + size * s;
}

void drawLcd(const Painter& g, const json& p, const SurfaceOptions::Image& screen) {
    const ImVec4 r = rect(p, "rect");
    const ImVec2 p0 = g.P({r.x, r.y});
    const ImVec2 p1 = g.P({r.x + r.z, r.y + r.w});

    g.rect(p0, p1, 0, {0.05f, 0.07f, 0.09f, 1}, {0.02f, 0.025f, 0.035f, 1});

    if (screen.id != ImTextureID_Invalid) {
        ImVec2 min;
        ImVec2 max;
        lcdPicture(p0, p1, screen.size, min, max);
        g.dl->AddRectFilled(p0, p1, g.C({0, 0, 0, 1}));
        g.dl->AddImage(screen.id, min, max, {0, 0}, {1, 1}, g.C({1, 1, 1, 1}));
    }

    // A faint diagonal reflection is most of what makes a black rectangle
    // look like glass.
    g.dl->AddTriangleFilled(p0, {p0.x + (p1.x - p0.x) * 0.62f, p0.y}, {p0.x, p0.y + (p1.y - p0.y) * 0.9f},
                            g.C({1, 1, 1, 0.025f}));
    g.dl->AddRect(p0, p1, g.C({0, 0, 0, 1}), 0, 0, 1.0f);

    if (screen.id != ImTextureID_Invalid) {
        return;
    }

    const ImVec2 mid = {r.x + r.z * 0.5f, r.y + r.w * 0.5f};
    g.text(g.fonts->title, std::min(r.w * 0.06f, 5.0f), mid - ImVec2(0, r.w * 0.04f),
           {0.35f, 0.42f, 0.5f, 0.7f}, "GUEST DISPLAY", 0.5f, 0.5f, 0.12f);
    g.text(g.fonts->label, std::min(r.w * 0.035f, 3.0f), mid + ImVec2(0, r.w * 0.05f),
           {0.35f, 0.42f, 0.5f, 0.5f}, str(p, "note"));
}

void drawIcon(const Painter& g, const std::string& icon, ImVec2 c, float h, ImVec4 col) {
    const ImU32 u = g.C(col);

    if (icon == "play_pause") {
        const float x = c.x - h * 0.95f;
        g.dl->AddTriangleFilled({x, c.y - h * 0.5f}, {x, c.y + h * 0.5f}, {x + h * 0.8f, c.y}, u);
        g.dl->AddLine({x + h * 1.05f, c.y + h * 0.55f}, {x + h * 1.3f, c.y - h * 0.55f}, u,
                      std::max(1.0f, h * 0.1f));
        g.dl->AddRectFilled({x + h * 1.45f, c.y - h * 0.5f}, {x + h * 1.62f, c.y + h * 0.5f}, u);
        g.dl->AddRectFilled({x + h * 1.78f, c.y - h * 0.5f}, {x + h * 1.95f, c.y + h * 0.5f}, u);
    } else if (icon == "eject") {
        g.dl->AddTriangleFilled({c.x - h * 0.5f, c.y + h * 0.1f}, {c.x + h * 0.5f, c.y + h * 0.1f},
                                {c.x, c.y - h * 0.45f}, u);
        g.dl->AddRectFilled({c.x - h * 0.5f, c.y + h * 0.25f}, {c.x + h * 0.5f, c.y + h * 0.4f}, u);
    } else if (icon == "play") {
        g.dl->AddTriangleFilled({c.x - h * 0.35f, c.y - h * 0.45f}, {c.x - h * 0.35f, c.y + h * 0.45f},
                                {c.x + h * 0.45f, c.y}, u);
    } else if (icon == "stop") {
        g.dl->AddRectFilled(c - ImVec2(h, h) * 0.4f, c + ImVec2(h, h) * 0.4f, u);
    } else if (icon == "triangle_up" || icon == "triangle_down") {
        const float s = icon == "triangle_up" ? 1.0f : -1.0f;
        const float t = std::max(1.0f, h * 0.06f);
        g.dl->AddTriangle({c.x - h * 0.5f, c.y + s * h * 0.4f}, {c.x + h * 0.5f, c.y + s * h * 0.4f},
                          {c.x, c.y - s * h * 0.45f}, u, t);
    }
}

void drawIconElement(const Painter& g, const json& p) {
    drawIcon(g, str(p, "icon"), g.P(vec2(p, "at")), g.L(vec2(p, "size", {6, 6}).x),
             colour(p, "color", {1, 1, 1, 1}));
}

void drawScrew(const Painter& g, const json& p) {
    const ImVec2 c = g.P(vec2(p, "at"));
    const float r = g.L(vec2(p, "size", {3, 3}).x * 0.5f);
    g.circle(c, r, {0.32f, 0.33f, 0.35f, 1}, {0.1f, 0.1f, 0.11f, 1});
    g.dl->AddLine(c + ImVec2(-r * 0.6f, -r * 0.35f), c + ImVec2(r * 0.6f, r * 0.35f), g.C({0, 0, 0, 0.8f}),
                  std::max(1.0f, r * 0.22f));
}

// An audio socket: a chrome nut round a dark hole.
void drawJack(const Painter& g, const json& p) {
    const ImVec2 c = g.P(vec2(p, "at"));
    const float r = g.L(vec2(p, "size", {10, 10}).x * 0.5f);
    g.haloCircle(c, r, g.L(0.8f), {0, g.L(0.4f)}, {0, 0, 0, 1}, 0.6f);
    g.metal(c, r, {0.25f, 0.26f, 0.28f, 1}, {0.92f, 0.93f, 0.95f, 1}, 6.0f);
    g.circle(c, r * 0.55f, {0.02f, 0.02f, 0.02f, 1}, {0.12f, 0.12f, 0.13f, 1});
    g.dl->AddCircle(c, r * 0.55f, g.C({0, 0, 0, 1}), 0, 1.0f);
}

// A slide or toggle switch with two or three positions, the lever moving
// along its longer side. Clicking steps it to the next position.
void drawSwitch(const Painter& g, const json& p, const ControlState& st) {
    const ImVec2 at = vec2(p, "at");
    const ImVec2 size = vec2(p, "size", {4, 8});
    const int positions = std::max(2, int(num(p, "positions", 2)));
    const bool vertical = size.y >= size.x;
    const ImVec2 p0 = g.P(at - size * 0.5f);
    const ImVec2 p1 = g.P(at + size * 0.5f);

    g.rect(p0, p1, g.L(0.4f), {0.02f, 0.02f, 0.02f, 1}, {0.08f, 0.08f, 0.09f, 1});
    g.dl->AddRect(p0, p1, g.C({0.3f, 0.31f, 0.33f, 0.6f}), g.L(0.4f), 0, 1.0f);

    const float t = std::clamp(st.value, 0.0f, float(positions - 1)) / float(positions - 1);
    const float travel = (vertical ? size.y : size.x) * 0.3f;
    const ImVec2 lever = at + (vertical ? ImVec2(0, (t - 0.5f) * 2.0f * travel) : ImVec2((t - 0.5f) * 2.0f * travel, 0));
    const ImVec2 half = vertical ? ImVec2(size.x * 0.22f, size.y * 0.2f) : ImVec2(size.x * 0.2f, size.y * 0.22f);
    const ImVec4 lc = colour(p, "lever", {0.98f, 0.98f, 0.98f, 1});
    g.rect(g.P(lever - half), g.P(lever + half), g.L(0.2f), lc, darken(lc, 0.28f));
}

// A label over a line with its ends turned down, grouping the controls
// beneath it.
void drawBracket(const Painter& g, const json& p) {
    const ImVec2 from = vec2(p, "from");
    const ImVec2 to = vec2(p, "to");
    const ImVec4 col = colour(p, "color", {0.8f, 0.8f, 0.8f, 1});
    const std::string label = str(p, "label");
    const float size = num(p, "label_size", g.labelSize);
    const float thick = std::max(1.0f, g.L(0.25f));
    const float drop = num(p, "drop", 1.2f);
    const float mid = (from.x + to.x) * 0.5f;
    const float half = label.empty() ? 0.0f
                                     : g.textWidth(g.fonts->label, g.fontPx(size), label.data(),
                                                   label.data() + label.size(), 0) / g.s * 0.5f + 1.2f;

    g.dl->AddLine(g.P({from.x, from.y + drop}), g.P(from), g.C(col), thick);
    g.dl->AddLine(g.P(from), g.P({mid - half, from.y}), g.C(col), thick);
    g.dl->AddLine(g.P({mid + half, to.y}), g.P(to), g.C(col), thick);
    g.dl->AddLine(g.P(to), g.P({to.x, to.y + drop}), g.C(col), thick);
    g.text(g.fonts->label, size, {mid, from.y}, col, label);
}

// A guitar pedal's stomp switch: a hex nut round a chrome plunger. It sinks
// a little when pressed.
void drawFootswitch(const Painter& g, const json& p, const ControlState& st) {
    const ImVec2 c = g.P(vec2(p, "at"));
    const float r = g.L(vec2(p, "size", {20, 20}).x * 0.5f);
    const ImVec4 chromeDark = {0.22f, 0.23f, 0.25f, 1};
    const ImVec4 chromeLight = {0.95f, 0.96f, 0.98f, 1};

    g.haloCircle(c, r * 0.95f, g.L(2.5f), {0, g.L(1.5f)}, {0, 0, 0, 1}, 0.8f);

    ImVec2 hex[6];

    for (int i = 0; i < 6; ++i) {
        hex[i] = polar(c, r, float(i) * kPi / 3.0f + kPi / 6.0f);
    }

    const int start = g.dl->VtxBuffer.Size;
    g.dl->AddConvexPolyFilled(hex, 6, g.C(chromeLight));
    g.shade(start, c.y - r, c.y + r, {0.78f, 0.8f, 0.83f, 1}, {0.32f, 0.33f, 0.36f, 1});
    g.dl->AddPolyline(hex, 6, g.C({0.15f, 0.15f, 0.16f, 1}), ImDrawFlags_Closed, 1.0f);

    g.metal(c, r * 0.8f, chromeDark, chromeLight, 6.0f);
    g.dl->AddCircle(c, r * 0.8f, g.C({0.1f, 0.1f, 0.11f, 0.8f}), 0, std::max(1.0f, g.L(0.4f)));

    const float plunger = r * (st.pressed ? 0.5f : 0.56f);
    g.haloCircle(c, plunger, g.L(1.2f), {0, g.L(st.pressed ? 0.3f : 1.0f)}, {0, 0, 0, 1}, 0.7f);
    g.metal(c, plunger, st.pressed ? darken(chromeDark, 0.2f) : chromeDark,
            st.pressed ? darken(chromeLight, 0.15f) : chromeLight, 10.0f);
}

// Buttons light one of three ways: the whole cap glows (translucent rubber,
// as on MPC), only the legend glows (printed caps, as on Prime), or an LED
// ring around the cap does (Prime's CUE and PLAY).
void drawButton(const Painter& g, const json& p, const ControlState& st, bool lampsOn) {
    const ImVec2 size = vec2(p, "size", {8, 6});
    const bool round = str(p, "shape") == "round";
    const ImVec4 cap = colour(p, "cap", {0.17f, 0.18f, 0.19f, 1});
    const ImVec4 textCol = colour(p, "text", {0.9f, 0.9f, 0.9f, 1});
    const ImVec4 lamp = colour(p, "lamp", textCol);
    const std::string labelAt = str(p, "label_at", "on");
    const bool ring = hasColour(p, "ring");
    const std::string lights = str(p, "lights", ring ? "ring" : labelAt == "on" ? "text" : "cap");
    const bool lit = st.pressed || (lampsOn && st.lit);

    const ImVec2 at = vec2(p, "at");
    const float sink = st.pressed ? 0.25f : 0.0f;
    const ImVec2 c = g.P(at + ImVec2(0, sink));
    const ImVec2 half = size * 0.5f * g.s;
    const ImVec2 p0 = c - half;
    const ImVec2 p1 = c + half;
    const float r = round ? half.x : g.L(num(p, "radius", std::min(size.x, size.y) * 0.16f));

    if (round) {
        g.haloCircle(c, half.x, g.L(1.0f), {0, g.L(0.5f - sink)}, {0, 0, 0, 1}, 0.6f);
    } else {
        g.haloRect(p0, p1, r, g.L(1.0f), {0, g.L(0.5f - sink)}, {0, 0, 0, 1}, 0.6f);
    }

    ImVec4 face = cap;

    if (lit && lights == "cap") {
        face = lighten(mix(cap, lamp, 0.65f), 0.25f);

        if (round) {
            g.haloCircle(c, half.x, g.L(2.6f), {0, 0}, lamp, 0.45f);
        } else {
            g.haloRect(p0, p1, r, g.L(2.6f), {0, 0}, lamp, 0.45f);
        }
    }

    if (st.pressed) {
        face = darken(face, 0.12f);
    }

    if (round) {
        g.circle(c, half.x, lighten(face, 0.14f), darken(face, 0.2f));
    } else {
        g.rect(p0, p1, r, lighten(face, 0.14f), darken(face, 0.2f));
        // A lit top edge, so the cap reads as raised.
        g.dl->AddLine({p0.x + r, p0.y + 1}, {p1.x - r, p0.y + 1}, g.C({1, 1, 1, 0.12f}), 1.0f);
    }

    if (ring) {
        const ImVec4 rc = colour(p, "ring", lamp);
        const ImVec4 now = lit ? rc : mix(rc, {0.16f, 0.17f, 0.18f, 1}, 0.6f);
        const float inset = g.L(0.9f);
        const float thick = std::max(1.0f, g.L(0.7f));

        if (round) {
            if (lit) {
                g.haloCircle(c, half.x - inset, g.L(1.4f), {0, 0}, rc, 0.4f);
            }
            g.dl->AddCircle(c, half.x - inset, g.C(now), 0, thick);
        } else {
            const ImVec2 in = {inset, inset};

            if (lit) {
                g.haloRect(p0 + in, p1 - in, r * 0.7f, g.L(1.6f), {0, 0}, rc, 0.35f);
                g.rect(p0 + in * 1.4f, p1 - in * 1.4f, r * 0.6f, lighten(cap, 0.1f), darken(cap, 0.2f));
            }
            g.dl->AddRect(p0 + in, p1 - in, g.C(now), r * 0.7f, 0, thick);
        }
    }

    if (hasColour(p, "outline")) {
        g.dl->AddRect(p0, p1, g.C(colour(p, "outline", {})), r, 0, std::max(1.0f, g.L(0.35f)));
    }

    // A coloured bar across the bottom of the cap, as MPC Sample marks its
    // record and transport buttons.
    if (hasColour(p, "stripe")) {
        const ImVec4 sc = colour(p, "stripe", {});
        const float inset = g.L(1.2f);
        const ImVec2 s0 = {p0.x + inset, p1.y - inset - g.L(1.1f)};
        const ImVec2 s1 = {p1.x - inset, p1.y - inset};

        if (lit) {
            g.haloRect(s0, s1, 0, g.L(1.0f), {0, 0}, sc, 0.5f);
        }
        g.dl->AddRectFilled(s0, s1, g.C(lit ? lighten(sc, 0.2f) : sc), g.L(0.2f));
    }

    // Printed legends are dimmer unlit, so lighting them is visible.
    ImVec4 legend = textCol;

    if (lights == "text" && !lit) {
        legend = mix(textCol, cap, 0.35f);
    }

    const std::string label = str(p, "label");
    const float labelSize = num(p, "label_size", g.labelSize);
    const ImVec2 mm = at + ImVec2(0, sink);

    if (!label.empty()) {
        if (labelAt == "above") {
            g.text(g.fonts->label, labelSize, mm - ImVec2(0, size.y * 0.5f + 0.7f), legend, label,
                   0.5f, 1.0f);
        } else if (labelAt == "below") {
            g.text(g.fonts->label, labelSize, mm + ImVec2(0, size.y * 0.5f + 0.7f), legend, label,
                   0.5f, 0.0f);
        } else {
            g.text(g.fonts->label, labelSize, mm, legend, label);
        }
    }

    const std::string icon = str(p, "icon");

    if (!icon.empty()) {
        drawIcon(g, icon, c, std::min(half.x, half.y) * 0.75f, legend);
    }

    const std::string sub = str(p, "sub");

    if (!sub.empty()) {
        // A sub-label is printed at the panel's size, whatever size the
        // legend on the cap is.
        const float labelSize = num(p, "sub_size", g.labelSize);
        const float top = at.y + size.y * 0.5f + 0.8f;

        if (hasColour(p, "sub_box")) {
            const float h = labelSize * g.textScale * 1.75f;
            g.dl->AddRectFilled(g.P({at.x - size.x * 0.5f, top}), g.P({at.x + size.x * 0.5f, top + h}),
                                g.C(colour(p, "sub_box", {})), g.L(0.3f));
            g.text(g.fonts->label, labelSize * 0.92f, {at.x, top + h * 0.5f},
                   colour(p, "sub_text", textCol), sub);
        } else {
            g.text(g.fonts->label, labelSize * 0.85f, {at.x, top}, colour(p, "sub_text", textCol), sub,
                   0.5f, 0.0f);
        }
    }
}

void drawPad(const Painter& g, const json& p, const ControlState& st, bool lampsOn) {
    const ImVec2 at = vec2(p, "at");
    const ImVec2 size = vec2(p, "size", {15, 15});
    const ImVec2 c = g.P(at);
    const ImVec2 half = size * 0.5f * g.s;
    const ImVec2 p0 = c - half;
    const ImVec2 p1 = c + half;
    const float r = g.L(std::min(size.x, size.y) * 0.07f);
    const ImVec4 cap = colour(p, "cap", {0.13f, 0.13f, 0.14f, 1});
    ImVec4 lamp = colour(p, "lamp", {0.4f, 0.3f, 1, 1});
    float idle = num(p, "glow", 0.0f);

    // Driven by the guest, a pad glows in what it lit, or not at all.
    if (st.guest && lampsOn) {
        ImVec4 sum = {0, 0, 0, 0};

        for (const ImVec4& q : st.quads) {
            if (q.w > 0) sum += ImVec4(q.x, q.y, q.z, 1);
        }

        if (sum.w > 0) {
            lamp = {sum.x / sum.w, sum.y / sum.w, sum.z / sum.w, 1};
        } else if (st.lit) {
            lamp = st.lamp.w > 0 ? st.lamp : lamp;
        } else {
            idle = 0.0f;
        }
    }
    const float hit = std::max(st.flash, st.pressed ? 1.0f : 0.0f);
    const bool lit = lampsOn && st.lit;

    g.haloRect(p0, p1, r, g.L(1.2f), {0, g.L(0.6f)}, {0, 0, 0, 1}, 0.7f);

    // MPC pads are lit from below, so the light shows round the edge.
    if (idle > 0 || hit > 0) {
        g.haloRect(p0, p1, r, g.L(1.4f + 2.0f * hit), {0, 0}, lamp, idle * 0.5f + hit * 0.6f);
    }

    ImVec4 face = st.pressed ? darken(cap, 0.08f) : cap;
    g.rect(p0, p1, r, lighten(face, 0.07f), darken(face, 0.12f));

    // RGB pads light their whole face. A pad with a strip lights only that.
    if (lit && !hasColour(p, "strip")) {
        g.haloRect(p0, p1, r, g.L(2.4f), {0, 0}, lamp, 0.4f);
        g.rect(p0, p1, r, lighten(lamp, 0.35f), mix(lamp, cap, 0.2f));
        g.dl->AddRectFilled(p0 + (p1 - p0) * 0.2f, p1 - (p1 - p0) * 0.2f, g.C({1, 1, 1, 0.12f}),
                            r * 3.0f);
    }

    const bool quarters = flag(p, "quadrants");

    // Engine lights each quarter of a split pad on its own note.
    if (lampsOn && quarters) {
        const ImVec2 mid = (p0 + p1) * 0.5f;

        for (int q = 0; q < 4; ++q) {
            const ImVec4 qc = st.quads[q];

            if (qc.w <= 0) {
                continue;
            }

            const ImVec2 q0 = {q % 2 ? mid.x : p0.x, q / 2 ? mid.y : p0.y};
            const ImVec2 q1 = {q % 2 ? p1.x : mid.x, q / 2 ? p1.y : mid.y};
            const ImDrawFlags corner = q == 0 ? ImDrawFlags_RoundCornersTopLeft
                                       : q == 1 ? ImDrawFlags_RoundCornersTopRight
                                       : q == 2 ? ImDrawFlags_RoundCornersBottomLeft
                                                : ImDrawFlags_RoundCornersBottomRight;
            const int start = g.dl->VtxBuffer.Size;
            g.dl->AddRectFilled(q0, q1, g.C(qc), r, corner);
            // Scaled rather than lightened, so a dim lamp stays dim.
            const ImVec4 top = {std::min(qc.x * 1.2f, 1.0f), std::min(qc.y * 1.2f, 1.0f), std::min(qc.z * 1.2f, 1.0f), 1};
            g.shade(start, q0.y, q1.y, top, mix(qc, cap, 0.3f));
        }
    }

    if (hit > 0) {
        ImVec2 h0 = p0, h1 = p1;

        if (quarters && st.quadrant >= 0) {
            const ImVec2 mid = (p0 + p1) * 0.5f;
            h0 = {st.quadrant % 2 ? mid.x : p0.x, st.quadrant / 2 ? mid.y : p0.y};
            h1 = {st.quadrant % 2 ? p1.x : mid.x, st.quadrant / 2 ? p1.y : mid.y};
        }

        g.dl->AddRectFilled(h0, h1, g.C(alpha(lit ? ImVec4(1, 1, 1, 1) : lamp, 0.55f * hit)), r);
    }

    if (quarters) {
        const ImVec2 mid = (p0 + p1) * 0.5f;
        const ImU32 line = g.C({0, 0, 0, 0.35f});
        g.dl->AddLine({mid.x, p0.y + r}, {mid.x, p1.y - r}, line, 1.0f);
        g.dl->AddLine({p0.x + r, mid.y}, {p1.x - r, mid.y}, line, 1.0f);
    }

    if (idle > 0 || hit > 0) {
        g.dl->AddRect(p0, p1, g.C(alpha(lighten(lamp, 0.2f), std::min(1.0f, idle + hit))), r, 0,
                      std::max(1.0f, g.L(0.35f)));
    }

    // Prime pads show their colour as a strip along the top.
    if (hasColour(p, "strip")) {
        const ImVec4 strip = colour(p, "strip", lamp);
        const ImVec2 s0 = g.P(at + ImVec2(-size.x * 0.5f + 2.2f, -size.y * 0.5f + 1.4f));
        const ImVec2 s1 = g.P(at + ImVec2(size.x * 0.5f - 2.2f, -size.y * 0.5f + 1.9f));
        const float bright = lit || hit > 0 ? 1.0f : 0.55f;

        if (bright == 1.0f) {
            g.haloRect(s0, s1, 0, g.L(1.0f), {0, 0}, strip, 0.4f);
        }
        g.dl->AddRectFilled(s0, s1, g.C(alpha(strip, bright)));
    }

    // A pad's number sits under its left corner and its shift function, if
    // any, under its right.
    const float labelSize = num(p, "label_size", g.labelSize);
    const ImVec4 textCol = colour(p, "text", {1, 1, 1, 1});
    const float below = size.y * 0.5f + 0.6f;
    g.text(g.fonts->label, labelSize, at + ImVec2(-size.x * 0.5f + 0.8f, below), textCol, str(p, "label"),
           0.0f, 0.0f);
    g.text(g.fonts->label, labelSize, at + ImVec2(size.x * 0.5f - 0.8f, below), textCol, str(p, "label_r"),
           1.0f, 0.0f);
}

// Knobs and encoders share a body. A knob points at its value between -135
// and +135 degrees; an encoder turns freely.
// An encoder seen from above: a flared base, tapered sides fluted with ribs,
// and a smaller top dished in, as on inMusic's and most makers' plastic encoders.
// The ribs and the dish are what tell it apart from a pot at a glance. Light
// comes from the top left, so ribs facing it catch it and the dish is
// brightest on its far wall. The ribs turn with the encoder. Returns the
// top's radius.
float drawFluted(const Painter& g, const json& p, ImVec2 c, float r, ImVec4 cap, ImVec4 skirt, float angle) {
    constexpr float light = -kPi / 4.0f;
    const float d = vec2(p, "size", {10, 10}).x;
    const int ribs = std::clamp(int(num(p, "ribs", std::round(d * 1.6f))), 16, 40);
    const float topR = r * num(p, "top", 0.7f);
    const ImVec2 white = g.dl->_Data->TexUvWhitePixel;

    const auto vtx = [&](ImVec2 pos, ImVec4 col) { g.dl->PrimWriteVtx(pos, white, g.C(col)); };

    // Shadow and the flange the body stands on.
    g.haloCircle(c, r, g.L(1.4f), {0, g.L(0.8f)}, {0, 0, 0, 1}, 0.75f);
    g.circle(c, r, darken(skirt, 0.15f), darken(skirt, 0.45f));
    g.dl->AddCircle(c, r - 0.5f, g.C(alpha(lighten(skirt, 0.25f), 0.35f)), 0, std::max(1.0f, g.L(0.12f)));

    // The sides, a ring of quads from the top's edge out to a scalloped rim.
    // Each rib's two faces lean either way, so its shading follows the slope
    // of the scallop as well as which way that side of the knob faces.
    const int steps = ribs * 8;
    const auto outer = [&](float a) {
        const float t = std::fabs(std::cos(float(ribs) * (a - angle) * 0.5f));
        return r * (0.885f + 0.035f * t);
    };
    const auto shade = [&](float a) {
        const float lean = 0.45f * std::sin(float(ribs) * (a - angle));
        const float lit = 0.5f + 0.5f * std::cos(a + lean - light);
        return lit;
    };

    for (int i = 0; i < steps; ++i) {
        const float a0 = 2.0f * kPi * float(i) / float(steps);
        const float a1 = 2.0f * kPi * float(i + 1) / float(steps);
        const float l0 = shade(a0);
        const float l1 = shade(a1);
        const ImVec4 dark = darken(skirt, 0.35f);
        const ImVec4 bright = lighten(skirt, 0.26f);

        g.dl->PrimReserve(6, 4);
        const ImDrawIdx base = ImDrawIdx(g.dl->_VtxCurrentIdx);
        vtx(polar(c, topR, a0), mix(dark, bright, l0));
        vtx(polar(c, topR, a1), mix(dark, bright, l1));
        vtx(polar(c, outer(a1), a1), mix(dark, bright, l1 * 0.75f));
        vtx(polar(c, outer(a0), a0), mix(dark, bright, l0 * 0.75f));
        g.dl->PrimWriteIdx(base);
        g.dl->PrimWriteIdx(ImDrawIdx(base + 1));
        g.dl->PrimWriteIdx(ImDrawIdx(base + 2));
        g.dl->PrimWriteIdx(base);
        g.dl->PrimWriteIdx(ImDrawIdx(base + 2));
        g.dl->PrimWriteIdx(ImDrawIdx(base + 3));
    }

    // The top: a flat lip round a shallow dish. The lip catches the light on
    // the side facing it. Inside the dish that is reversed: the wall nearest
    // the light is in its own shadow and the far wall faces the light, so
    // the fan runs from a mid tone just off centre, away from the light, out
    // to a rim that is dark near the light and bright across from it.
    const int segs = 64;
    const float dishR = topR * 0.84f;

    g.circle(c, topR, lighten(cap, 0.12f), darken(cap, 0.2f));

    for (int i = 0; i < segs; ++i) {
        const float a0 = 2.0f * kPi * float(i) / float(segs);
        const float a1 = 2.0f * kPi * float(i + 1) / float(segs);
        const auto lip = [&](float a) { return 0.5f + 0.5f * std::cos(a - light); };
        g.dl->PathArcTo(c, topR - g.L(0.12f), imAngle(a0), imAngle(a1), 2);
        g.dl->PathStroke(g.C(alpha(lighten(cap, 0.45f), 0.55f * lip(0.5f * (a0 + a1)))), 0,
                         std::max(1.0f, g.L(0.18f)));
    }

    const ImVec2 mid = polar(c, dishR * 0.18f, light + kPi);
    const ImVec4 centre = darken(cap, 0.06f);

    for (int i = 0; i < segs; ++i) {
        const float a0 = 2.0f * kPi * float(i) / float(segs);
        const float a1 = 2.0f * kPi * float(i + 1) / float(segs);
        const auto rim = [&](float a) {
            return mix(darken(cap, 0.42f), lighten(cap, 0.2f), 0.5f - 0.5f * std::cos(a - light));
        };

        g.dl->PrimReserve(3, 3);
        const ImDrawIdx base = ImDrawIdx(g.dl->_VtxCurrentIdx);
        vtx(mid, centre);
        vtx(polar(c, dishR, a0), rim(a0));
        vtx(polar(c, dishR, a1), rim(a1));
        g.dl->PrimWriteIdx(base);
        g.dl->PrimWriteIdx(ImDrawIdx(base + 1));
        g.dl->PrimWriteIdx(ImDrawIdx(base + 2));
    }

    // The dish's edge, and the crease where the top meets the ribs.
    g.dl->AddCircle(c, dishR, g.C({0, 0, 0, 0.25f}), 64, std::max(1.0f, g.L(0.1f)));
    g.dl->AddCircle(c, topR, g.C({0, 0, 0, 0.45f}), 64, std::max(1.0f, g.L(0.14f)));
    return topR;
}

void drawKnob(const Painter& g, const json& p, const ControlState& st, bool endless) {
    const ImVec2 at = vec2(p, "at");
    const float d = vec2(p, "size", {10, 10}).x;
    const ImVec2 c = g.P(at);
    const float r = g.L(d * 0.5f);
    const ImVec4 cap = colour(p, "cap", {0.22f, 0.23f, 0.25f, 1});
    const ImVec4 skirt = colour(p, "skirt", darken(cap, 0.4f));
    const float angle = endless ? st.angle : (-135.0f + 270.0f * st.value) * kPi / 180.0f;
    const float minA = -135.0f * kPi / 180.0f;
    const float maxA = 135.0f * kPi / 180.0f;
    const bool bipolar = flag(p, "bipolar");

    if (hasColour(p, "well")) {
        const ImVec4 well = colour(p, "well", {});
        const float wr = g.L(num(p, "well_size", d * 1.15f) * 0.5f);
        g.circle(c, wr, darken(well, 0.3f), lighten(well, 0.05f));
        g.dl->AddCircle(c, wr, g.C({0, 0, 0, 0.35f}), 0, std::max(1.0f, g.L(0.4f)));
    }

    // A printed range arc with a dot at each end, as on MPC Sample.
    if (hasColour(p, "arc")) {
        const ImVec4 arc = colour(p, "arc", {});
        const float ar = r * 1.22f;
        g.dl->PathArcTo(c, ar, imAngle(minA), imAngle(maxA), 48);
        g.dl->PathStroke(g.C(arc), 0, std::max(1.0f, g.L(d * 0.035f)));

        for (float a : {minA, maxA}) {
            g.dl->AddCircleFilled(polar(c, ar, a), g.L(d * 0.05f), g.C(arc));
        }
    }

    if (flag(p, "scale")) {
        for (int i = 0; i <= 10; ++i) {
            const float a = minA + (maxA - minA) * float(i) / 10.0f;
            const bool big = i == 0 || i == 10 || (bipolar && i == 5);
            g.dl->AddCircleFilled(polar(c, r * 1.3f, a), g.L(big ? 0.38f : 0.24f),
                                  g.C(colour(p, "scale_color", {0.85f, 0.86f, 0.88f, 0.9f})));
        }
    }

    if (hasColour(p, "halo")) {
        const ImVec4 halo = colour(p, "halo", {});
        const float hr = r * 1.17f;
        const float thick = g.L(num(p, "halo_width", 1.1f));

        // An encoder has no ends, so its ring goes all the way round.
        if (endless) {
            g.dl->AddCircle(c, hr, g.C(alpha(halo, 0.25f)), 64, thick * 2.6f);
            g.dl->AddCircle(c, hr, g.C(halo), 64, thick);
        } else {
            g.dl->PathArcTo(c, hr, imAngle(minA), imAngle(maxA), 48);
            g.dl->PathStroke(g.C({0.2f, 0.21f, 0.22f, 1}), 0, thick);
        }

        float from = endless ? minA : bipolar ? 0.0f : minA;
        float to = endless ? maxA : angle;

        if (from > to) {
            std::swap(from, to);
        }

        if (!endless && to - from > 0.01f) {
            g.dl->PathArcTo(c, hr, imAngle(from), imAngle(to), 48);
            g.dl->PathStroke(g.C(alpha(halo, 0.25f)), 0, thick * 2.6f);
            g.dl->PathArcTo(c, hr, imAngle(from), imAngle(to), 48);
            g.dl->PathStroke(g.C(halo), 0, thick);
        }
    }

    // Encoders are fluted with a dished top unless they are metal or ask to
    // look like a pot.
    const bool fluted = endless && !flag(p, "metal") && str(p, "style", "fluted") == "fluted";
    float capR = r * 0.8f;

    if (fluted) {
        capR = drawFluted(g, p, c, r, cap, skirt, angle);
    } else {
        g.haloCircle(c, r, g.L(1.2f), {0, g.L(0.7f)}, {0, 0, 0, 1}, 0.7f);
        g.circle(c, r, lighten(skirt, 0.1f), darken(skirt, 0.3f));

        // Knurling turns with the knob, which is most of what makes a turn
        // visible on a big encoder.
        if (flag(p, "knurl", true)) {
            const int ridges = std::clamp(int(d * 3.0f), 24, 96);

            for (int i = 0; i < ridges; ++i) {
                const float a = angle + 2.0f * kPi * float(i) / float(ridges);
                g.dl->AddLine(polar(c, r * 0.84f, a), polar(c, r * 0.98f, a),
                              g.C(alpha(lighten(skirt, 0.18f), 0.8f)), std::max(1.0f, g.L(0.18f)));
            }
        }

        if (flag(p, "metal")) {
            g.metal(c, capR, darken(cap, 0.55f), lighten(cap, 0.5f), 8.0f);
        } else {
            g.circle(c, capR, lighten(cap, 0.22f), darken(cap, 0.3f));
            g.dl->AddCircleFilled(c - ImVec2(capR * 0.18f, capR * 0.28f), capR * 0.5f, g.C({1, 1, 1, 0.045f}));
            g.dl->AddCircleFilled(c - ImVec2(capR * 0.22f, capR * 0.34f), capR * 0.28f, g.C({1, 1, 1, 0.04f}));
        }
    }

    // Large encoders are turned with a finger in a dimple, small ones show a
    // painted pointer. A fluted encoder's ribs already show it turning, so it
    // has a pointer only when the layout gives one.
    if (fluted && !flag(p, "dimple", d > 20.0f)) {
        if (p.contains("pointer")) {
            g.dl->AddLine(polar(c, capR * 0.45f, angle), polar(c, capR * 0.92f, angle),
                          g.C(colour(p, "pointer", {})), std::max(1.2f, g.L(d * 0.05f)));
        }
    } else if (flag(p, "dimple", endless && d > 20.0f)) {
        const ImVec2 dimple = polar(c, capR * 0.6f, angle);
        g.circle(dimple, capR * 0.17f, darken(cap, 0.35f), lighten(cap, 0.12f));
    } else {
        g.dl->AddLine(polar(c, capR * 0.2f, angle), polar(c, capR * 0.95f, angle),
                      g.C(colour(p, "pointer", {0.96f, 0.96f, 0.96f, 1})), std::max(1.2f, g.L(d * 0.055f)));
    }

    const std::string sub = str(p, "sub");

    if (!sub.empty()) {
        const float extra = flag(p, "scale") ? 0.32f : hasColour(p, "halo") ? 0.22f : 0.08f;
        g.text(g.fonts->label, num(p, "label_size", g.labelSize * 0.9f),
               at + ImVec2(0, d * (0.5f + extra) + 0.4f), colour(p, "text", {0.9f, 0.9f, 0.9f, 1}),
               sub, 0.5f, 0.0f);
    }
}

void drawFader(const Painter& g, const json& p, const ControlState& st) {
    const ImVec2 at = vec2(p, "at");
    const float len = num(p, "length", 40);
    const bool vertical = str(p, "orient", "v") != "h";
    const ImVec2 along = vertical ? ImVec2(0, 1) : ImVec2(1, 0);
    const ImVec2 across = vertical ? ImVec2(1, 0) : ImVec2(0, 1);
    const int ticks = int(num(p, "ticks", 0));
    const bool detent = flag(p, "detent");

    const ImVec2 slotHalf = along * (len * 0.5f + 2.0f) + across * (num(p, "slot_width", 1.1f) * 0.5f);
    g.dl->AddRectFilled(g.P(at - slotHalf), g.P(at + slotHalf), g.C({0.02f, 0.02f, 0.025f, 1}),
                        g.L(0.5f));

    const float tickLength = num(p, "tick_length", 2.4f);

    for (int i = 0; i < ticks && ticks > 1; ++i) {
        const float t = float(i) / float(ticks - 1) - 0.5f;
        const bool centre = detent && i == (ticks - 1) / 2;
        const float tl = centre ? tickLength * 1.4f : tickLength;

        for (float side : {-1.0f, 1.0f}) {
            const ImVec2 base = at + along * (t * len) + across * (side * 2.4f);
            g.dl->AddLine(g.P(base), g.P(base + across * (side * tl)), g.C({0.85f, 0.86f, 0.88f, 0.85f}),
                          std::max(1.0f, g.L(0.42f)));
        }
    }

    const float v = st.value;
    const ImVec2 capC = at + along * (vertical ? (0.5f - v) * len : (v - 0.5f) * len);
    const ImVec2 capSize = vec2(p, "cap_size", vertical ? ImVec2(9.5f, 4.4f) : ImVec2(4.4f, 9.5f));
    const ImVec2 c0 = g.P(capC - capSize * 0.5f);
    const ImVec2 c1 = g.P(capC + capSize * 0.5f);
    const ImVec4 cap = colour(p, "cap", {0.78f, 0.8f, 0.82f, 1});

    g.haloRect(c0, c1, g.L(0.5f), g.L(1.2f), {0, g.L(0.8f)}, {0, 0, 0, 1}, 0.8f);
    g.rect(c0, c1, g.L(0.5f), lighten(cap, 0.25f), darken(cap, 0.45f));

    const ImVec2 lineHalf = across * (capSize.x * 0.5f - 0.6f);
    const ImVec2 l0 = vertical ? capC - lineHalf : capC - ImVec2(0, capSize.y * 0.5f - 0.6f);
    const ImVec2 l1 = vertical ? capC + lineHalf : capC + ImVec2(0, capSize.y * 0.5f - 0.6f);
    g.dl->AddLine(g.P(l0), g.P(l1), g.C({0.08f, 0.08f, 0.09f, 1}), std::max(1.0f, g.L(0.5f)));
}

// A platter in brushed aluminium. The brushing is circular, so its highlight
// is two wedges fixed to the light, not to the platter; the knurled rim and a
// marker are what visibly turn.
// A motorised platter carrying a record, as on SYSTEM ONE: a strobe ring of
// dots, grooved vinyl, a centre label and a stripe that shows it turning.
void drawVinyl(const Painter& g, const ImVec2 c, float r, const ControlState& st) {
    g.haloCircle(c, r, g.L(3.0f), {0, g.L(1.5f)}, {0, 0, 0, 1}, 0.9f);
    g.metal(c, r, {0.2f, 0.21f, 0.22f, 1}, {0.62f, 0.64f, 0.67f, 1}, 10.0f);

    // Strobe dots in staggered rows, turning with the platter.
    const float dot = std::max(1.0f, r * 0.012f);

    for (int row = 0; row < 4; ++row) {
        const float rr = r * (0.84f + 0.04f * float(row));
        const int count = int(2.0f * kPi * rr / (dot * 4.5f));

        for (int i = 0; i < count; ++i) {
            const float a = st.angle + 2.0f * kPi * (float(i) + (row % 2 ? 0.5f : 0.0f)) / float(count);
            g.dl->AddCircleFilled(polar(c, rr, a), dot, g.C({0.95f, 0.95f, 0.97f, 0.9f}), 6);
        }
    }

    const float record = r * 0.8f;
    g.circle(c, record, {0.06f, 0.06f, 0.065f, 1}, {0.02f, 0.02f, 0.025f, 1});

    // Grooves, and a faint sheen across them.
    const float step = std::max(1.5f, g.L(0.6f));
    int ring = 0;

    for (float rr = record * 0.32f; rr < record * 0.97f; rr += step, ++ring) {
        g.dl->AddCircle(c, rr, g.C({1, 1, 1, ring % 3 == 0 ? 0.045f : 0.02f}), 96, 1.0f);
    }

    g.dl->PathArcTo(c, record * 0.7f, -2.6f, -1.6f, 32);
    g.dl->PathStroke(g.C({1, 1, 1, 0.018f}), 0, record * 0.5f);

    const float label = record * 0.27f;
    g.circle(c, label, {0.3f, 0.31f, 0.33f, 1}, {0.16f, 0.17f, 0.18f, 1});
    g.dl->AddLine(polar(c, label * 1.05f, st.angle + kPi * 0.5f), polar(c, record * 0.95f, st.angle + kPi * 0.5f),
                  g.C({0.92f, 0.92f, 0.94f, 0.95f}), std::max(1.5f, g.L(1.2f)));
    g.circle(c, label * 0.18f, {0.85f, 0.86f, 0.88f, 1}, {0.4f, 0.41f, 0.43f, 1});

    if (st.pressed) {
        g.dl->AddCircle(c, record, g.C({1, 1, 1, 0.25f}), 0, std::max(1.0f, g.L(0.6f)));
    }
}

// A Prime 4 style platter: a ridged rim, an LED ring, a black touch surface
// and a small round screen in the middle, which the guest draws on the real
// unit.
void drawDisplayJog(const Painter& g, const json& p, ImVec2 c, float r, const ControlState& st) {
    const ImVec4 ring = colour(p, "ring", {0.15f, 0.45f, 1.0f, 1});

    g.haloCircle(c, r, g.L(3.0f), {0, g.L(1.5f)}, {0, 0, 0, 1}, 0.9f);
    g.circle(c, r, {0.2f, 0.21f, 0.23f, 1}, {0.08f, 0.08f, 0.09f, 1});

    // Grip ridges on the rim, turning with it.
    const int ridges = 48;

    for (int i = 0; i < ridges; ++i) {
        const float a = st.angle + 2.0f * kPi * float(i) / float(ridges);
        g.dl->AddLine(polar(c, r * 0.9f, a), polar(c, r * 0.99f, a + 0.04f), g.C({0.36f, 0.37f, 0.4f, 0.9f}),
                      std::max(1.0f, g.L(1.0f)));
    }

    const float lit = st.pressed ? 1.0f : 0.85f;
    g.dl->AddCircle(c, r * 0.885f, g.C(alpha(ring, 0.3f * lit)), 0, g.L(3.0f));
    g.dl->AddCircle(c, r * 0.885f, g.C(alpha(ring, lit)), 0, std::max(1.5f, g.L(1.2f)));

    const float plate = r * 0.86f;
    g.circle(c, plate, {0.09f, 0.09f, 0.1f, 1}, {0.03f, 0.03f, 0.035f, 1});
    const float step = std::max(1.5f, g.L(0.7f));

    for (float rr = plate * 0.45f; rr < plate * 0.98f; rr += step) {
        g.dl->AddCircle(c, rr, g.C({1, 1, 1, 0.018f}), 96, 1.0f);
    }

    const float screen = r * num(p, "display", 0.36f);
    g.circle(c, screen * 1.08f, {0.25f, 0.26f, 0.28f, 1}, {0.12f, 0.12f, 0.13f, 1});
    g.circle(c, screen, {0.06f, 0.08f, 0.11f, 1}, {0.02f, 0.03f, 0.04f, 1});

    // The screen's position ticks, turning with the platter.
    for (int i = 0; i < 40; ++i) {
        const float a = st.angle + 2.0f * kPi * float(i) / 40.0f;
        g.dl->AddLine(polar(c, screen * 0.78f, a), polar(c, screen * 0.92f, a),
                      g.C({0.85f, 0.88f, 0.92f, i == 0 ? 1.0f : 0.5f}), std::max(1.0f, g.L(0.35f)));
    }
}

void drawJog(const Painter& g, const json& p, const ControlState& st) {
    const ImVec2 c = g.P(vec2(p, "at"));
    const float r = g.L(vec2(p, "size", {80, 80}).x * 0.5f);
    const float plate = r * 0.87f;

    if (str(p, "style") == "vinyl") {
        drawVinyl(g, c, r, st);
        return;
    }

    if (str(p, "style") == "display") {
        drawDisplayJog(g, p, c, r, st);
        return;
    }

    g.haloCircle(c, r, g.L(3.0f), {0, g.L(1.5f)}, {0, 0, 0, 1}, 0.9f);
    g.circle(c, r, {0.12f, 0.12f, 0.13f, 1}, {0.05f, 0.05f, 0.06f, 1});

    const int ridges = 150;

    for (int i = 0; i < ridges; ++i) {
        const float a = st.angle + 2.0f * kPi * float(i) / float(ridges);
        g.dl->AddLine(polar(c, r * 0.905f, a), polar(c, r * 0.985f, a), g.C({0.3f, 0.31f, 0.33f, 0.9f}),
                      std::max(1.0f, g.L(0.25f)));
    }

    g.dl->AddCircle(c, r * 0.995f, g.C({0.32f, 0.33f, 0.35f, 1}), 0, 1.0f);
    g.dl->AddCircle(c, r * 0.885f, g.C({0, 0, 0, 1}), 0, std::max(1.5f, g.L(0.6f)));

    g.metal(c, plate, {0.14f, 0.15f, 0.16f, 1}, {0.72f, 0.74f, 0.77f, 1});

    // Fine concentric rings for the brushing itself.
    const float step = std::max(1.5f, g.L(0.45f));
    int ring = 0;

    for (float rr = plate * 0.1f; rr < plate; rr += step, ++ring) {
        const float k = std::fmod(std::sin(float(ring) * 7.13f) * 9137.1f, 1.0f);
        g.dl->AddCircle(c, rr, g.C(k > 0 ? ImVec4(1, 1, 1, 0.03f * k) : ImVec4(0, 0, 0, -0.06f * k)), 96,
                        1.0f);
    }

    g.dl->AddCircle(c, plate, g.C({0.45f, 0.46f, 0.48f, 0.8f}), 0, 1.0f);

    if (st.pressed) {
        g.dl->AddCircle(c, plate - g.L(0.4f), g.C({1, 1, 1, 0.3f}), 0, std::max(1.0f, g.L(0.6f)));
    }

    g.dl->AddCircleFilled(polar(c, plate * 0.9f, st.angle), g.L(0.9f), g.C({0.9f, 0.92f, 0.95f, 0.55f}));
    g.circle(c, plate * 0.07f, {0.25f, 0.26f, 0.28f, 1}, {0.1f, 0.1f, 0.11f, 1});
}

std::string hexColour(ImVec4 c) {
    char hex[8];
    std::snprintf(hex, sizeof hex, "#%02x%02x%02x", int(c.x * 255.0f), int(c.y * 255.0f), int(c.z * 255.0f));
    return hex;
}

// An LED, an encoder's halo or a jog's ring as the guest has it: in the
// colour it lit it, or dark. Unchanged while the guest has not touched it.
json guestRing(const json& p, const ControlState& st) {
    if (!st.guest) {
        return p;
    }

    json q = p;

    for (const char* key : {"color", "halo", "ring"}) {
        if (!q.contains(key)) {
            continue;
        }

        const ImVec4 own = colour(p, key, {1, 1, 1, 1});
        const ImVec4 lit = st.lamp.w > 0 ? st.lamp : own;
        q[key] = hexColour(st.lit ? lit : mix(own, {0.1f, 0.1f, 0.11f, 1}, 0.85f));
    }

    return q;
}

// The element's own fields, with its lamp in the colour the guest lit it.
json guestLamp(const json& p, ImVec4 c) {
    const std::string hex = hexColour(c);
    json q = p;
    q["lamp"] = hex;

    for (const char* key : {"ring", "strip"}) {
        if (q.contains(key)) {
            q[key] = hex;
        }
    }

    return q;
}

bool interactive(const std::string& kind) {
    return kind == "button" || kind == "pad" || kind == "knob" || kind == "encoder" ||
           kind == "fader" || kind == "jog" || kind == "footswitch" || kind == "switch";
}

}  // namespace

void Surface::setLayout(const Layout* layout) {
    layout_ = layout;
    hovered_ = -1;
    contacts_.clear();

    if (!layout_) {
        return;
    }

    std::unordered_map<std::string, ControlState> kept;

    for (const Element& e : layout_->elements) {
        if (e.id.empty()) {
            continue;
        }

        auto it = state_.find(e.id);

        if (it != state_.end()) {
            kept[e.id] = it->second;
            continue;
        }

        ControlState st;
        st.value = num(e.p, "value", 0.0f);
        st.lit = flag(e.p, "lit");
        kept[e.id] = st;
    }

    state_ = std::move(kept);
}

const Element* Surface::hovered() const {
    if (!layout_ || hovered_ < 0) {
        return nullptr;
    }

    return &layout_->elements[size_t(hovered_)];
}

bool Surface::hit(const Element& e, ImVec2 m) const {
    const json& p = e.p;
    const ImVec2 at = vec2(p, "at");

    if (e.kind == "button" || e.kind == "pad" || e.kind == "switch") {
        const ImVec2 half = vec2(p, "size", {8, 6}) * 0.5f + ImVec2(0.8f, 0.8f) * float(e.kind == "switch");
        return std::fabs(m.x - at.x) <= half.x && std::fabs(m.y - at.y) <= half.y;
    }

    if (e.kind == "knob" || e.kind == "encoder" || e.kind == "jog" || e.kind == "footswitch") {
        const float r = vec2(p, "size", {10, 10}).x * 0.5f * (e.kind == "knob" || e.kind == "encoder" ? 1.15f : 1.0f);
        const ImVec2 d = m - at;
        return d.x * d.x + d.y * d.y <= r * r;
    }

    if (e.kind == "fader") {
        const float len = num(p, "length", 40) * 0.5f + 3.0f;
        const bool vertical = str(p, "orient", "v") != "h";
        const ImVec2 half = vertical ? ImVec2(5.5f, len) : ImVec2(len, 5.5f);
        return std::fabs(m.x - at.x) <= half.x && std::fabs(m.y - at.y) <= half.y;
    }

    return false;
}

int Surface::elementAt(ImVec2 screen) const {
    const ImVec2 mm = (screen - origin_) / scale_;

    for (int i = int(layout_->elements.size()) - 1; i >= 0; --i) {
        const Element& e = layout_->elements[size_t(i)];

        if (!e.id.empty() && interactive(e.kind) && !flag(e.p, "hidden") && hit(e, mm)) {
            return i;
        }
    }

    return -1;
}

bool Surface::onScreen(ImVec2 at, ImVec2* uv) const {
    if (screenMax_.x <= screenMin_.x || at.x < screenMin_.x || at.y < screenMin_.y || at.x >= screenMax_.x ||
        at.y >= screenMax_.y) {
        return false;
    }

    if (uv) {
        *uv = (at - screenMin_) / (screenMax_ - screenMin_);
    }

    return true;
}

void Surface::begin(uint64_t who, int index, ImVec2 at) {
    const Element& e = layout_->elements[size_t(index)];
    ControlState& st = state_[e.id];

    st.pressed = true;
    st.flash = 1.0f;

    // A pad split into quarters, as MPC's are, remembers which was struck:
    // 0 top left, 1 top right, 2 bottom left, 3 bottom right.
    if (e.kind == "pad" && flag(e.p, "quadrants")) {
        const ImVec2 c = origin_ + vec2(e.p, "at") * scale_;
        st.quadrant = (at.x > c.x ? 1 : 0) + (at.y > c.y ? 2 : 0);
    }

    if (e.kind == "switch") {
        const float positions = std::max(2.0f, num(e.p, "positions", 2));
        st.value = std::fmod(std::round(st.value) + 1.0f, positions);
    }

    Contact& c = contacts_[who];
    c.index = index;
    c.start = at;
    c.value = st.value;
    c.angle = st.angle;

    if (e.kind == "jog") {
        const ImVec2 centre = origin_ + vec2(e.p, "at") * scale_;
        c.lastJogAngle = std::atan2(at.y - centre.y, at.x - centre.x);
        st.spin = 0.0f;
    }

    // A fader cap jumps to where it is grabbed, as a real one would be
    // grabbed and moved.
    drag(c, at);
}

void Surface::drag(Contact& c, ImVec2 at) {
    c.pos = at;
    const Element& e = layout_->elements[size_t(c.index)];
    ControlState& st = state_[e.id];

    // Right and up both turn clockwise, so a drag works in whichever
    // direction the hand goes.
    const float travel = (at.x - c.start.x) - (at.y - c.start.y);

    if (e.kind == "knob") {
        st.value = std::clamp(c.value + travel / 160.0f, 0.0f, 1.0f);
    } else if (e.kind == "encoder") {
        st.angle = c.angle + travel * 0.02f;
    } else if (e.kind == "fader") {
        const ImVec2 cap = origin_ + vec2(e.p, "at") * scale_;
        const float len = num(e.p, "length", 40) * scale_;
        const bool vertical = str(e.p, "orient", "v") != "h";
        const float t = vertical ? 0.5f - (at.y - cap.y) / len : 0.5f + (at.x - cap.x) / len;
        st.value = std::clamp(t, 0.0f, 1.0f);

        // Centre detent, for pitch faders.
        if (flag(e.p, "detent") && std::fabs(st.value - 0.5f) < 0.025f) {
            st.value = 0.5f;
        }
    } else if (e.kind == "jog") {
        const ImVec2 centre = origin_ + vec2(e.p, "at") * scale_;
        const float now = std::atan2(at.y - centre.y, at.x - centre.x);
        float delta = now - c.lastJogAngle;

        if (delta > kPi) delta -= 2 * kPi;
        if (delta < -kPi) delta += 2 * kPi;

        c.lastJogAngle = now;
        st.angle += delta;

        const float dt = std::max(ImGui::GetIO().DeltaTime, 1.0f / 240.0f);
        st.spin = st.spin * 0.6f + (delta / dt) * 0.4f;
    }
}

void Surface::end(uint64_t who) {
    auto it = contacts_.find(who);

    if (it == contacts_.end()) {
        return;
    }

    const int index = it->second.index;
    contacts_.erase(it);

    // Still pressed while another finger holds the same control.
    for (const auto& [_, other] : contacts_) {
        if (other.index == index) {
            return;
        }
    }

    state_[layout_->elements[size_t(index)].id].pressed = false;
}

void Surface::touches() {
    // A finger moves several times a frame. Only its last position counts,
    // so a jog's speed is measured over a whole frame as the mouse's is.
    std::unordered_map<uint64_t, ImVec2> moved;

    const auto flush = [&](uint64_t id) {
        auto m = moved.find(id);
        auto c = contacts_.find(id);

        if (m != moved.end() && c != contacts_.end()) {
            drag(c->second, m->second);
        }

        if (m != moved.end()) {
            moved.erase(m);
        }
    };

    for (const TouchEvent& t : touches_) {
        if (t.id == screenHolder_) {
            // One finger on the screen at a time, since VNC has one pointer.
            onScreen(t.pos, &screenPointer_.uv);

            // Let go a frame later, or a tap that lands and lifts within
            // one frame would never reach the guest pressed.
            if (t.phase == TouchEvent::Up) {
                screenHolder_ = kNobody;
                screenLift_ = true;
            }
            continue;
        }

        if (t.phase == TouchEvent::Down) {
            const int index = elementAt(t.pos);

            if (index >= 0) {
                begin(t.id, index, t.pos);
            } else if (screenHolder_ == kNobody && onScreen(t.pos, &screenPointer_.uv)) {
                screenHolder_ = t.id;
                screenPointer_.inside = true;
                screenPointer_.down = true;
            }
        } else if (t.phase == TouchEvent::Move) {
            moved[t.id] = t.pos;
        } else {
            flush(t.id);
            end(t.id);
        }
    }

    for (const auto& [id, pos] : moved) {
        auto c = contacts_.find(id);

        if (c != contacts_.end()) {
            drag(c->second, pos);
        }
    }

    touches_.clear();
}

void Surface::scrolled(int index, ImVec2 finger) {
    const Element& e = layout_->elements[size_t(index)];
    ControlState& st = state_[e.id];

    // The same mapping as a drag, so a finger turns a knob the same way
    // whether it arrives as a drag or, from a touchscreen, as a scroll.
    const float travel = finger.x - finger.y;

    if (e.kind == "knob") {
        st.value = std::clamp(st.value + travel / 160.0f, 0.0f, 1.0f);
    } else if (e.kind == "encoder") {
        st.angle += travel * 0.02f;
    } else if (e.kind == "fader") {
        const float len = std::max(num(e.p, "length", 40) * scale_, 1.0f);
        const bool vertical = str(e.p, "orient", "v") != "h";
        st.value = std::clamp(st.value + (vertical ? -finger.y : finger.x) / len, 0.0f, 1.0f);
    } else if (e.kind == "jog") {
        // A scroll carries no position, only how far the finger went, and
        // the pointer stays where the touch began. Turning by the finger's
        // motion across the line from the centre to there follows a finger
        // that circles the platter, and the scroll's own momentum lets it
        // coast.
        const ImVec2 r = ImGui::GetIO().MousePos - (origin_ + vec2(e.p, "at") * scale_);
        const float r2 = std::max(r.x * r.x + r.y * r.y, 400.0f);
        st.angle += (r.x * finger.y - r.y * finger.x) / r2;
        st.spin = 0.0f;
    }
}

void Surface::reset(int index) {
    const Element& e = layout_->elements[size_t(index)];
    ControlState& st = state_[e.id];
    st.value = num(e.p, "value", 0.0f);
    st.spin = 0.0f;
}

void Surface::draw(const SurfaceOptions& options, const Fonts& fonts) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();

    if (avail.x < 50 || avail.y < 50) {
        return;
    }

    ImGui::InvisibleButton("##surface", avail);
    const bool canvasHovered = ImGui::IsItemHovered();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    time_ += io.DeltaTime;

    dl->AddRectFilledMultiColor(pos, pos + avail, IM_COL32(34, 36, 41, 255), IM_COL32(34, 36, 41, 255),
                                IM_COL32(14, 15, 17, 255), IM_COL32(14, 15, 17, 255));

    if (!layout_) {
        return;
    }

    const Layout& layout = *layout_;
    const float margin = std::min(avail.x, avail.y) * 0.06f;
    scale_ = std::min((avail.x - 2 * margin) / layout.size.x, (avail.y - 2 * margin) / layout.size.y);
    origin_ = pos + (avail - layout.size * scale_) * 0.5f;

    Painter g;
    g.dl = dl;
    g.origin = origin_;
    g.s = scale_;
    g.fonts = &fonts;
    g.labelSize = layout.labelSize;
    g.textScale = layout.textScale;

    // Input first, so this frame draws what the mouse and fingers just did.
    canvasMin_ = pos;
    canvasMax_ = pos + avail;
    hovered_ = canvasHovered || contacts_.count(kMouse) ? elementAt(io.MousePos) : -1;

    // The first lcd is the guest's screen. A device with two, such as one
    // with a screen in each jog, shows the guest's main display in the first.
    screenMin_ = screenMax_ = {0, 0};

    for (const Element& e : layout.elements) {
        if (e.kind == "lcd" && !flag(e.p, "hidden")) {
            const ImVec4 r = rect(e.p, "rect");
            lcdPicture(g.P({r.x, r.y}), g.P({r.x + r.z, r.y + r.w}), options.screen.size, screenMin_, screenMax_);
            break;
        }
    }

    if (std::exchange(screenLift_, false)) {
        screenPointer_.down = false;
    }

    if (ImGui::IsItemActivated() && hovered_ >= 0) {
        begin(kMouse, hovered_, io.MousePos);
    } else if (ImGui::IsItemActivated() && screenHolder_ == kNobody && onScreen(io.MousePos)) {
        screenHolder_ = kMouse;
    }

    if (screenHolder_ == kMouse) {
        screenPointer_.inside = true;
        screenPointer_.down = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        // Clamped rather than dropped, so a drag off the edge still lets go
        // where the guest can see it.
        screenPointer_.uv = ImClamp((io.MousePos - screenMin_) / ImMax(screenMax_ - screenMin_, ImVec2(1, 1)),
                                    ImVec2(0, 0), ImVec2(1, 1));

        if (!screenPointer_.down) {
            screenHolder_ = kNobody;
        }
    } else if (screenHolder_ == kNobody && !screenPointer_.down) {
        screenPointer_.inside = canvasHovered && hovered_ < 0 && onScreen(io.MousePos, &screenPointer_.uv);
    }

    if (auto it = contacts_.find(kMouse); it != contacts_.end()) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            drag(it->second, io.MousePos);
        } else {
            end(kMouse);
        }
    }

    touches();

    if (canvasHovered && hovered_ >= 0) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            reset(hovered_);
        }

        if (scroll_.x != 0.0f || scroll_.y != 0.0f) {
            scrolled(hovered_, scroll_);
        }
    }

    scroll_ = {0, 0};

    // Jogs coast and pads fade even when nothing is touching them.
    for (const Element& e : layout.elements) {
        if (e.id.empty()) {
            continue;
        }

        ControlState& st = state_[e.id];

        if (e.kind == "jog" && !st.pressed) {
            st.angle += st.spin * io.DeltaTime;
            st.spin *= std::exp(-1.1f * io.DeltaTime);
        }

        if (!st.pressed && st.flash > 0) {
            st.flash = std::max(0.0f, st.flash - io.DeltaTime * 2.5f);
        }

        if (e.kind == "meter" && !st.guest) {
            float level = 0.0f;

            if (options.animateMeters) {
                const float h = hash(e.id) * 10.0f;
                const float t = float(time_);
                level = 0.55f + 0.25f * std::sin(t * 2.1f + h) + 0.15f * std::sin(t * 7.3f + h * 3.0f);
            }

            st.value = level;
        }
    }

    for (const Face& face : layout.faces) {
        drawBody(g, face);
    }

    for (const Element& e : layout.elements) {
        static const ControlState none;
        const ControlState& st = e.id.empty() ? none : state_[e.id];
        const json& p = e.p;

        if (flag(p, "hidden")) {
            continue;
        }

        if (e.kind == "panel") drawPanel(g, p);
        else if (e.kind == "frame") drawFrame(g, p);
        else if (e.kind == "line") drawLine(g, p);
        else if (e.kind == "text") drawText(g, p);
        else if (e.kind == "led") drawLed(g, guestRing(p, st), st.guest ? st.lit : options.layoutLamps && flag(p, "lit"));
        else if (e.kind == "meter") drawMeter(g, p, st.value, st.peak);
        else if (e.kind == "lcd") drawLcd(g, p, options.screen);
        else if (e.kind == "hatch") drawHatch(g, p);
        else if (e.kind == "button") drawButton(g, st.lamp.w > 0 ? guestLamp(p, st.lamp) : p, st, options.layoutLamps);
        else if (e.kind == "pad") drawPad(g, st.lamp.w > 0 ? guestLamp(p, st.lamp) : p, st, options.layoutLamps);
        else if (e.kind == "knob") drawKnob(g, p, st, false);
        else if (e.kind == "encoder") drawKnob(g, guestRing(p, st), st, true);
        else if (e.kind == "fader") drawFader(g, p, st);
        else if (e.kind == "jog") drawJog(g, guestRing(p, st), st);
        else if (e.kind == "footswitch") drawFootswitch(g, p, st);
        else if (e.kind == "icon") drawIconElement(g, p);
        else if (e.kind == "screw") drawScrew(g, p);
        else if (e.kind == "jack") drawJack(g, p);
        else if (e.kind == "switch") drawSwitch(g, p, st);
        else if (e.kind == "bracket") drawBracket(g, p);
    }

    if (options.overlay.image.id != ImTextureID_Invalid) {
        const SurfaceOptions::Overlay& o = options.overlay;

        for (const Face& face : layout.faces) {
            if (face.name != o.face) {
                continue;
            }

            const ImVec2 size = o.image.size;
            const ImVec4 r = o.rect.z > 0 ? o.rect : ImVec4(0, 0, size.x, size.y);
            dl->AddImage(o.image.id, g.P({face.rect.x, face.rect.y}),
                         g.P({face.rect.x + face.rect.z, face.rect.y + face.rect.w}), {r.x / size.x, r.y / size.y},
                         {r.z / size.x, r.w / size.y}, g.C({1, 1, 1, o.alpha}));
        }
    }

    if (options.showIds) {
        for (const Element& e : layout.elements) {
            if (e.id.empty() || !interactive(e.kind) || flag(e.p, "hidden")) {
                continue;
            }

            const ImVec2 at = g.P(vec2(e.p, "at"));
            const float px = 11.0f;
            const ImVec2 ts = fonts.ui->CalcTextSizeA(px, FLT_MAX, 0, e.id.c_str());
            const ImVec2 t0 = at - ts * 0.5f - ImVec2(3, 1);
            dl->AddRectFilled(t0, t0 + ts + ImVec2(6, 2), IM_COL32(10, 12, 16, 210), 3);
            dl->AddText(fonts.ui, px, t0 + ImVec2(3, 1), IM_COL32(140, 210, 255, 255), e.id.c_str());
        }
    }
}

// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <swofty/mixin.hpp>

// Immediate-mode 2D overlay drawn on top of the game. Register a paint callback; the core
// invokes it once per presented frame, on the render thread, with a per-frame Paint handle.
// Issue draw calls through that handle. Coordinates are framebuffer pixels with the origin at
// the top-left; query the drawable size each frame. Colours are 0xRRGGBBAA. Load textures from
// the addon folder once (outside paint) and draw them by id.

namespace swofty::overlay {

enum class Result { ok, late, unavailable, invalid };

using Paint = SwoftyPaint;

namespace detail {

inline const SwoftyOverlay *table()
{
    return api && api->version >= 5 ? api->overlay : nullptr;
}

inline Result result(int r)
{
    return r == SWOFTY_ASSET_OK ? Result::ok
           : r == SWOFTY_ASSET_LATE ? Result::late
           : r == SWOFTY_ASSET_UNAVAILABLE ? Result::unavailable
                                           : Result::invalid;
}

}

// The overlay library is usable (feature on and implemented by this core).
inline bool available()
{
    auto t = detail::table();
    return t && t->available();
}

// Register a per-frame paint callback. The token identifies it for remove; pass nullptr to
// ignore it.
inline Result on_paint(SwoftyPaintFn fn, void *user = nullptr, uint32_t *token = nullptr)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->on_paint(fn, user, token));
}

// Drop a paint callback by its token.
inline Result remove(uint32_t token)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->remove(token));
}

// Current drawable size in pixels.
inline void size(const Paint *paint, float *width, float *height)
{
    if (auto t = detail::table())
        t->size(paint, width, height);
}

// Filled rectangle.
inline void rect(Paint *paint, float x, float y, float w, float h, uint32_t rgba)
{
    if (auto t = detail::table())
        t->rect(paint, x, y, w, h, rgba);
}

// Rectangle outline of the given stroke thickness.
inline void rect_outline(Paint *paint, float x, float y, float w, float h, float thickness, uint32_t rgba)
{
    if (auto t = detail::table())
        t->rect_outline(paint, x, y, w, h, thickness, rgba);
}

// Line segment of the given stroke thickness.
inline void line(Paint *paint, float x0, float y0, float x1, float y1, float thickness, uint32_t rgba)
{
    if (auto t = detail::table())
        t->line(paint, x0, y0, x1, y1, thickness, rgba);
}

// Text in the built-in font at the given pixel size; the top-left of the text is (x, y).
inline void text(Paint *paint, float x, float y, float font_size, uint32_t rgba, std::string_view utf8)
{
    if (auto t = detail::table())
        t->text(paint, x, y, font_size, rgba, std::string(utf8).c_str());
}

// Bounds the text would occupy at the given pixel size.
inline void measure(const Paint *paint, float font_size, std::string_view utf8, float *width, float *height)
{
    if (auto t = detail::table())
        t->measure(paint, font_size, std::string(utf8).c_str(), width, height);
}

// Load a texture from a file in the addon folder and register it under an id. Call outside
// paint; the id is then available to image() for the lifetime of the process.
inline Result load_image(std::string_view id, std::string_view file)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->load_image(std::string(id).c_str(), std::string(file).c_str()));
}

// Draw a loaded texture into the rectangle, multiplied by the tint colour (0xffffffff for none).
inline Result image(Paint *paint, std::string_view id, float x, float y, float w, float h, uint32_t tint = 0xffffffff)
{
    auto t = detail::table();
    if (!t)
        return Result::unavailable;
    return detail::result(t->image(paint, std::string(id).c_str(), x, y, w, h, tint));
}

inline const char *name(Result r)
{
    return r == Result::ok ? "ok" : r == Result::late ? "late" : r == Result::unavailable ? "unavailable" : "invalid";
}

}

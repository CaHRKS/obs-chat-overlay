#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "chat.hpp"

// A partial visual configuration produced by importing a third-party design
// (StreamElements overlay export, raw CSS from StreamElements/Streamlabs, or our
// own theme JSON). Only the fields present in the source are set.
struct Theme {
	std::string name;

	std::optional<std::string> font_face;
	std::optional<float> font_size;
	std::optional<bool> bold;
	std::optional<bool> italic;

	std::optional<uint32_t> text_color;
	std::optional<uint32_t> nick_color;
	std::optional<uint32_t> bg_color;
	std::optional<uint32_t> outline_color;

	std::optional<int> outline_size;
	std::optional<int> bg_radius;
	std::optional<int> bg_padding;
	std::optional<int> line_spacing;
	std::optional<float> emote_scale;
	std::optional<int> text_align; // 0 left, 1 center, 2 right
};

// Parses our own theme schema:
// {
//   "name": "...",
//   "font":   { "face": "Roboto", "size": 30, "bold": true, "italic": false },
//   "colors": { "text": "#fff", "nick": "#8ab4f8",
//               "background": "rgba(0,0,0,0.55)", "outline": "#000" },
//   "outline": 2, "radius": 8, "padding": 10, "line_spacing": 4,
//   "emote_scale": 1.0, "align": "left"
// }
bool theme_from_json(const std::string &text, Theme &out, std::string *error);

// Parses a StreamElements overlay export: { "widgets": [ { "css": {...} } ] }.
// Only the style of the chat-like widget is taken.
bool theme_from_stream_elements(const std::string &text, Theme &out, std::string *error);

// Parses raw CSS declarations, e.g. copied from the StreamElements "Custom CSS"
// field or from a Streamlabs chat box theme.
bool theme_from_css(const std::string &css, Theme &out, std::string *error);

// Applies the fields that are set in `theme` to `cfg`.
void theme_apply(const Theme &theme, Config &cfg);

// Serialises the visual part of `cfg` as a theme JSON document.
std::string theme_to_json(const Config &cfg, const std::string &name);

// Built-in presets: id -> theme.
std::vector<std::pair<std::string, Theme>> theme_presets();

// "#rrggbb", "#rgb", "#rrggbbaa", "rgb()", "rgba()", a few named colours.
// The result uses the OBS byte order 0xAABBGGRR.
bool parse_css_color(const std::string &value, uint32_t &out);

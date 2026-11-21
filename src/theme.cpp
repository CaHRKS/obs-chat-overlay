#include "theme.hpp"

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <vector>

namespace {

std::string trim_copy(std::string_view value)
{
	size_t begin = 0;
	size_t end = value.size();
	while (begin < end && std::isspace((unsigned char)value[begin]))
		++begin;
	while (end > begin && std::isspace((unsigned char)value[end - 1]))
		--end;
	return std::string(value.substr(begin, end - begin));
}

std::string to_lower(std::string value)
{
	for (char &c : value)
		c = (char)std::tolower((unsigned char)c);
	return value;
}

bool hex_value(char c, unsigned int &out)
{
	if (c >= '0' && c <= '9') {
		out = (unsigned int)(c - '0');
		return true;
	}
	if (c >= 'a' && c <= 'f') {
		out = (unsigned int)(c - 'a' + 10);
		return true;
	}
	if (c >= 'A' && c <= 'F') {
		out = (unsigned int)(c - 'A' + 10);
		return true;
	}
	return false;
}

bool parse_hex_color(const std::string &value, uint32_t &out)
{
	unsigned int r = 0;
	unsigned int g = 0;
	unsigned int b = 0;
	unsigned int a = 255;

	if (value.size() == 4) { // #rgb
		unsigned int rr = 0, gg = 0, bb = 0;
		if (!hex_value(value[1], rr) || !hex_value(value[2], gg) || !hex_value(value[3], bb))
			return false;
		r = rr * 17;
		g = gg * 17;
		b = bb * 17;
	} else if (value.size() == 7 || value.size() == 9) {
		unsigned int digits[8] = {0};
		for (size_t i = 1; i < value.size(); ++i) {
			if (!hex_value(value[i], digits[i - 1]))
				return false;
		}
		r = digits[0] * 16 + digits[1];
		g = digits[2] * 16 + digits[3];
		b = digits[4] * 16 + digits[5];
		if (value.size() == 9)
			a = digits[6] * 16 + digits[7];
	} else {
		return false;
	}

	out = (a << 24) | (b << 16) | (g << 8) | r;
	return true;
}

bool parse_rgb_function(const std::string &value, uint32_t &out)
{
	const size_t open = value.find('(');
	const size_t close = value.find(')', open == std::string::npos ? 0 : open);
	if (open == std::string::npos || close == std::string::npos)
		return false;

	std::string inner = value.substr(open + 1, close - open - 1);
	for (char &c : inner) {
		if (c == ',' || c == '/')
			c = ' ';
	}
	std::vector<double> parts;
	const char *cursor = inner.c_str();
	while (*cursor) {
		while (*cursor && std::isspace((unsigned char)*cursor))
			++cursor;
		if (!*cursor)
			break;
		char *end = nullptr;
		const double v = std::strtod(cursor, &end);
		if (end == cursor)
			return false;
		parts.push_back(v);
		cursor = end;
	}
	if (parts.size() < 3)
		return false;

	const double alpha = parts.size() >= 4 ? parts[3] : 1.0;
	const auto clamp_byte = [](double v) {
		const double c = v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v);
		return (unsigned int)std::lround(c);
	};
	const auto clamp_alpha = [](double v) {
		const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
		return (unsigned int)std::lround(c * 255.0);
	};

	const unsigned int r = clamp_byte(parts[0]);
	const unsigned int g = clamp_byte(parts[1]);
	const unsigned int b = clamp_byte(parts[2]);
	const unsigned int a = clamp_alpha(alpha);
	out = (a << 24) | (b << 16) | (g << 8) | r;
	return true;
}

const std::map<std::string, uint32_t> &named_colors()
{
	static const std::map<std::string, uint32_t> colors = {
		{"white", 0xFFFFFFFFu},  {"black", 0xFF000000u},  {"red", 0xFF0000FFu},
		{"green", 0xFF008000u},  {"lime", 0xFF00FF00u},   {"blue", 0xFFFF0000u},
		{"yellow", 0xFF00FFFFu}, {"cyan", 0xFFFFFF00u},   {"aqua", 0xFFFFFF00u},
		{"magenta", 0xFFFF00FFu}, {"fuchsia", 0xFFFF00FFu}, {"orange", 0xFF00A5FFu},
		{"purple", 0xFF800080u}, {"pink", 0xFFCBC0FFu},   {"gray", 0xFF808080u},
		{"grey", 0xFF808080u},   {"silver", 0xFFC0C0C0u}, {"navy", 0xFF800000u},
		{"transparent", 0x00000000u},
	};
	return colors;
}

// Extracts the first colour token from a CSS value.
bool extract_color(const std::string &value, uint32_t &out)
{
	const std::string lower = to_lower(value);
	if (lower.find("url(") != std::string::npos)
		return false;

	const size_t hash = value.find('#');
	if (hash != std::string::npos) {
		size_t end = hash;
		while (end < value.size() && (std::isxdigit((unsigned char)value[end]) || value[end] == '#'))
			++end;
		return parse_hex_color(value.substr(hash, end - hash), out);
	}

	for (const char *fn : {"rgba(", "rgb(", "hsla(", "hsl("}) {
		const size_t pos = lower.find(fn);
		if (pos != std::string::npos) {
			if (std::string(fn).find("hsl") != std::string::npos)
				return false; // hsl support is out of scope
			return parse_rgb_function(lower.substr(pos), out);
		}
	}

	for (const auto &entry : named_colors()) {
		const size_t pos = lower.find(entry.first);
		if (pos == std::string::npos)
			continue;
		// Require word boundaries so "red" does not match "colored".
		const bool left_ok = pos == 0 || !std::isalpha((unsigned char)lower[pos - 1]);
		const bool at_end = pos + entry.first.size() >= lower.size();
		const bool right_ok = at_end || !std::isalpha((unsigned char)lower[pos + entry.first.size()]);
		if (left_ok && right_ok) {
			out = entry.second;
			return true;
		}
	}
	return false;
}

bool parse_px(const std::string &value, double &out)
{
	const std::string text = to_lower(trim_copy(value));
	if (text.empty())
		return false;

	char *end = nullptr;
	const double number = std::strtod(text.c_str(), &end);
	if (end == text.c_str())
		return false;

	std::string unit = trim_copy(std::string_view(end));
	double result = number;
	if (unit == "pt")
		result = number * 4.0 / 3.0;
	else if (unit == "em" || unit == "rem")
		result = number * 16.0;
	else if (!unit.empty() && unit != "px")
		return false;

	out = result;
	return true;
}

std::string normalize_property(std::string prop)
{
	prop = trim_copy(prop);
	std::string out;
	for (size_t i = 0; i < prop.size(); ++i) {
		const char c = prop[i];
		if (c == '_') {
			out.push_back('-');
			continue;
		}
		if (c >= 'A' && c <= 'Z') {
			if (!out.empty() && out.back() != '-')
				out.push_back('-');
			out.push_back((char)(c - 'A' + 'a'));
			continue;
		}
		out.push_back((char)std::tolower((unsigned char)c));
	}
	// Collapse "--webkit" style double dashes produced by the camelCase pass.
	while (out.find("--") != std::string::npos)
		out.replace(out.find("--"), 2, "-");
	return out;
}

void apply_declaration(Theme &theme, const std::string &raw_property, const std::string &raw_value)
{
	const std::string prop = normalize_property(raw_property);
	std::string value = trim_copy(raw_value);
	if (prop.empty() || value.empty())
		return;

	// Drop "!important" and other priority markers.
	const size_t important = to_lower(value).find("!important");
	if (important != std::string::npos)
		value = trim_copy(value.substr(0, important));

	if (prop == "font-family") {
		std::string family = value;
		const size_t comma = family.find(',');
		if (comma != std::string::npos)
			family = family.substr(0, comma);
		family = trim_copy(family);
		if (family.size() >= 2 && (family.front() == '"' || family.front() == '\''))
			family = family.substr(1, family.size() - 2);
		family = trim_copy(family);
		if (!family.empty() && to_lower(family) != "inherit" && to_lower(family) != "initial")
			theme.font_face = family;
		return;
	}
	if (prop == "font-size") {
		double px = 0.0;
		if (parse_px(value, px) && px >= 6.0 && px <= 200.0)
			theme.font_size = (float)px;
		return;
	}
	if (prop == "font-weight") {
		const std::string lower = to_lower(value);
		if (lower == "bold" || lower == "bolder") {
			theme.bold = true;
			return;
		}
		double weight = 0.0;
		if (parse_px(lower, weight) && weight > 0.0) {
			theme.bold = weight >= 600.0;
			return;
		}
		if (lower == "normal" || lower == "lighter")
			theme.bold = false;
		return;
	}
	if (prop == "font-style") {
		const std::string lower = to_lower(value);
		theme.italic = lower.find("italic") != std::string::npos || lower.find("oblique") != std::string::npos;
		return;
	}
	if (prop == "color" || prop == "text-fill-color") {
		uint32_t color = 0;
		if (extract_color(value, color))
			theme.text_color = color;
		return;
	}
	if (prop == "background-color" || prop == "background") {
		uint32_t color = 0;
		if (extract_color(value, color))
			theme.bg_color = color;
		return;
	}
	if (prop == "border-radius") {
		double px = 0.0;
		if (parse_px(value, px) && px >= 0.0 && px <= 64.0)
			theme.bg_radius = (int)std::lround(px);
		return;
	}
	if (prop == "padding" || prop == "padding-left" || prop == "padding-inline") {
		double px = 0.0;
		if (parse_px(value, px) && px >= 0.0 && px <= 64.0)
			theme.bg_padding = (int)std::lround(px);
		return;
	}
	if (prop == "text-align") {
		const std::string lower = to_lower(value);
		if (lower.find("center") != std::string::npos)
			theme.text_align = 1;
		else if (lower.find("right") != std::string::npos || lower.find("end") != std::string::npos)
			theme.text_align = 2;
		else if (lower.find("left") != std::string::npos || lower.find("start") != std::string::npos)
			theme.text_align = 0;
		return;
	}
	if (prop == "-webkit-text-stroke" || prop == "text-stroke") {
		// "2px #000" (width and colour in one declaration)
		size_t cursor = 0;
		while (cursor < value.size()) {
			const size_t space = value.find(' ', cursor);
			const std::string token = value.substr(cursor, space == std::string::npos
									       ? std::string::npos
									       : space - cursor);
			double px = 0.0;
			uint32_t color = 0;
			if (parse_px(token, px) && px >= 0.0 && px <= 8.0)
				theme.outline_size = (int)std::lround(px);
			else if (extract_color(token, color))
				theme.outline_color = color;
			if (space == std::string::npos)
				break;
			cursor = space + 1;
		}
		return;
	}
	if (prop == "-webkit-text-stroke-width" || prop == "text-stroke-width") {
		double px = 0.0;
		if (parse_px(value, px) && px >= 0.0 && px <= 8.0)
			theme.outline_size = (int)std::lround(px);
		return;
	}
	if (prop == "-webkit-text-stroke-color" || prop == "text-stroke-color") {
		uint32_t color = 0;
		if (extract_color(value, color))
			theme.outline_color = color;
		return;
	}
	if (prop == "text-shadow") {
		// StreamElements chat styles usually rely on a shadow; map the colour to
		// the outline colour and keep a 1px outline so the text stays readable.
		uint32_t color = 0;
		if (extract_color(value, color)) {
			theme.outline_color = color;
			if (!theme.outline_size)
				theme.outline_size = 1;
		}
		return;
	}
	if (prop == "line-height") {
		double px = 0.0;
		if (parse_px(value, px) && px >= 0.0 && px <= 64.0)
			theme.line_spacing = (int)std::lround(px * 0.35);
		return;
	}
}

void parse_css_block(const std::string &block, Theme &theme)
{
	size_t pos = 0;
	while (pos < block.size()) {
		size_t semicolon = block.find(';', pos);
		if (semicolon == std::string::npos)
			semicolon = block.size();
		const std::string declaration = block.substr(pos, semicolon - pos);
		const size_t colon = declaration.find(':');
		if (colon != std::string::npos) {
			apply_declaration(theme, declaration.substr(0, colon),
					  declaration.substr(colon + 1));
		}
		pos = semicolon + 1;
	}
}

void apply_css_object(const json::Value &object, Theme &theme)
{
	for (const auto &kv : object.object) {
		if (kv.second.type == json::Value::Type::String) {
			apply_declaration(theme, kv.first, kv.second.string);
		} else if (kv.second.type == json::Value::Type::Number) {
			apply_declaration(theme, kv.first, std::to_string(kv.second.number));
		}
	}
}

std::string strip_css_comments(const std::string &css)
{
	std::string out;
	out.reserve(css.size());
	size_t i = 0;
	while (i < css.size()) {
		if (css[i] == '/' && i + 1 < css.size() && css[i + 1] == '*') {
			const size_t end = css.find("*/", i + 2);
			i = end == std::string::npos ? css.size() : end + 2;
			continue;
		}
		out.push_back(css[i]);
		++i;
	}
	return out;
}

std::string color_to_css(uint32_t color)
{
	const unsigned int r = color & 0xFF;
	const unsigned int g = (color >> 8) & 0xFF;
	const unsigned int b = (color >> 16) & 0xFF;
	const unsigned int a = (color >> 24) & 0xFF;
	char buffer[64];
	snprintf(buffer, sizeof(buffer), "rgba(%u, %u, %u, %.3f)", r, g, b, (double)a / 255.0);
	return std::string(buffer);
}

} // namespace

bool parse_css_color(const std::string &value, uint32_t &out)
{
	const std::string text = trim_copy(value);
	if (text.empty())
		return false;
	if (text.front() == '#')
		return parse_hex_color(text, out);
	if (text.find('(') != std::string::npos)
		return parse_rgb_function(to_lower(text), out);
	const auto it = named_colors().find(to_lower(text));
	if (it == named_colors().end())
		return false;
	out = it->second;
	return true;
}

bool theme_from_css(const std::string &css, Theme &out, std::string *error)
{
	const std::string cleaned = strip_css_comments(css);
	size_t pos = 0;
	size_t blocks = 0;
	while (pos < cleaned.size()) {
		const size_t open = cleaned.find('{', pos);
		if (open == std::string::npos)
			break;
		const size_t close = cleaned.find('}', open);
		if (close == std::string::npos)
			break;
		parse_css_block(cleaned.substr(open + 1, close - open - 1), out);
		++blocks;
		pos = close + 1;
	}
	if (blocks == 0) {
		// No braces: treat the whole text as a declaration list.
		parse_css_block(cleaned, out);
	}
	if (!out.font_face && !out.font_size && !out.text_color && !out.bg_color &&
	    !out.outline_color && !out.outline_size) {
		if (error)
			*error = "no supported CSS properties found";
		return false;
	}
	return true;
}

bool theme_from_stream_elements(const std::string &text, Theme &out, std::string *error)
{
	json::Value root;
	if (!json::parse(text, root, error))
		return false;

	const json::Value *widgets = root.find("widgets");
	if (!widgets || !widgets->is_array()) {
		if (error)
			*error = "no \"widgets\" array (not a StreamElements overlay export?)";
		return false;
	}

	int best_score = -1;
	const json::Value *best = nullptr;
	for (const json::Value &widget : widgets->array) {
		const json::Value *css = widget.find("css");
		if (!css)
			continue;
		if (css->type == json::Value::Type::String) {
			// Some widget exports keep raw CSS text instead of an object.
			if (best_score < 0) {
				best_score = 0;
				best = &widget;
			}
			continue;
		}
		if (!css->is_object())
			continue;
		int score = 0;
		const std::string type = to_lower(widget.get_string("type"));
		if (type.find("chat") != std::string::npos)
			score += 10;
		if (css->find("message"))
			score += 3;
		if (css->find("font-size"))
			score += 2;
		if (css->find("color"))
			score += 1;
		if (score > best_score) {
			best_score = score;
			best = &widget;
		}
	}

	if (!best) {
		if (error)
			*error = "no widget with a css object found";
		return false;
	}

	const json::Value *css = best->find("css");
	if (css->type == json::Value::Type::String)
		return theme_from_css(css->string, out, error);

	apply_css_object(*css, out);

	if (const json::Value *message = css->find("message"); message && message->is_object())
		apply_css_object(*message, out);
	if (const json::Value *highlights = css->find("highlights"); highlights && highlights->is_object()) {
		const json::Value *color = highlights->find("color");
		if (color && color->type == json::Value::Type::String) {
			uint32_t parsed = 0;
			if (parse_css_color(color->string, parsed))
				out.nick_color = parsed;
		}
	}

	const std::string name = best->get_string("name");
	if (!name.empty())
		out.name = name;

	return true;
}

bool theme_from_json(const std::string &text, Theme &out, std::string *error)
{
	json::Value root;
	if (!json::parse(text, root, error))
		return false;
	if (!root.is_object()) {
		if (error)
			*error = "theme JSON must be an object";
		return false;
	}

	out.name = root.get_string("name");

	if (const json::Value *font = root.find("font"); font && font->is_object()) {
		const std::string face = font->get_string("face");
		if (!face.empty())
			out.font_face = face;
		const double size = font->get_number("size");
		if (size >= 6.0 && size <= 200.0)
			out.font_size = (float)size;
		if (font->find("bold"))
			out.bold = font->get_bool("bold");
		if (font->find("italic"))
			out.italic = font->get_bool("italic");
	}

	if (const json::Value *colors = root.find("colors"); colors && colors->is_object()) {
		const auto read_color = [colors](const char *key) -> std::optional<uint32_t> {
			const json::Value *value = colors->find(key);
			if (!value || value->type != json::Value::Type::String)
				return std::nullopt;
			uint32_t parsed = 0;
			if (!parse_css_color(value->string, parsed))
				return std::nullopt;
			return parsed;
		};
		if (auto c = read_color("text"))
			out.text_color = *c;
		if (auto c = read_color("nick"))
			out.nick_color = *c;
		if (auto c = read_color("background"))
			out.bg_color = *c;
		if (auto c = read_color("outline"))
			out.outline_color = *c;
	}

	const auto read_int = [&root](const char *key, int min, int max) -> std::optional<int> {
		const json::Value *value = root.find(key);
		if (!value || value->type != json::Value::Type::Number)
			return std::nullopt;
		int result = (int)std::lround(value->number);
		if (result < min || result > max)
			return std::nullopt;
		return result;
	};

	if (auto v = read_int("outline", 0, 8))
		out.outline_size = *v;
	if (auto v = read_int("radius", 0, 64))
		out.bg_radius = *v;
	if (auto v = read_int("padding", 0, 64))
		out.bg_padding = *v;
	if (auto v = read_int("line_spacing", 0, 64))
		out.line_spacing = *v;

	const json::Value *scale = root.find("emote_scale");
	if (scale && scale->type == json::Value::Type::Number) {
		const double value = scale->number;
		if (value >= 0.5 && value <= 3.0)
			out.emote_scale = (float)value;
	}

	const json::Value *align = root.find("align");
	if (align) {
		if (align->type == json::Value::Type::Number) {
			const int value = (int)std::lround(align->number);
			if (value >= 0 && value <= 2)
				out.text_align = value;
		} else if (align->type == json::Value::Type::String) {
			const std::string value = to_lower(align->string);
			if (value.find("center") != std::string::npos)
				out.text_align = 1;
			else if (value.find("right") != std::string::npos)
				out.text_align = 2;
			else if (value.find("left") != std::string::npos)
				out.text_align = 0;
		}
	}

	return true;
}

void theme_apply(const Theme &theme, Config &cfg)
{
	if (theme.font_face)
		cfg.font_face = *theme.font_face;
	if (theme.font_size)
		cfg.font_size = std::max(6.0f, std::min(200.0f, *theme.font_size));
	if (theme.bold)
		cfg.bold = *theme.bold;
	if (theme.italic)
		cfg.italic = *theme.italic;
	if (theme.text_color)
		cfg.text_color = *theme.text_color;
	if (theme.nick_color)
		cfg.nick_color = *theme.nick_color;
	if (theme.bg_color)
		cfg.bg_color = *theme.bg_color;
	if (theme.outline_color)
		cfg.outline_color = *theme.outline_color;
	if (theme.outline_size)
		cfg.outline_size = std::max(0, std::min(8, *theme.outline_size));
	if (theme.bg_radius)
		cfg.bg_radius = std::max(0, std::min(64, *theme.bg_radius));
	if (theme.bg_padding)
		cfg.bg_padding = std::max(0, std::min(64, *theme.bg_padding));
	if (theme.line_spacing)
		cfg.line_spacing = std::max(0, std::min(64, *theme.line_spacing));
	if (theme.emote_scale)
		cfg.emote_scale = std::max(0.5f, std::min(3.0f, *theme.emote_scale));
	if (theme.text_align)
		cfg.text_align = std::max(0, std::min(2, *theme.text_align));
}

std::string theme_to_json(const Config &cfg, const std::string &name)
{
	std::string out;
	out += "{\n";
	out += "  \"name\": \"" + (name.empty() ? std::string("custom") : name) + "\",\n";
	out += "  \"font\": { \"face\": \"" + cfg.font_face + "\", \"size\": " +
	       std::to_string((int)std::lround(cfg.font_size)) + ", \"bold\": " +
	       (cfg.bold ? "true" : "false") + ", \"italic\": " + (cfg.italic ? "true" : "false") +
	       " },\n";
	out += "  \"colors\": { \"text\": \"" + color_to_css(cfg.text_color) + "\", \"nick\": \"" +
	       color_to_css(cfg.nick_color) + "\", \"background\": \"" +
	       color_to_css(cfg.bg_color) + "\", \"outline\": \"" +
	       color_to_css(cfg.outline_color) + "\" },\n";
	out += "  \"outline\": " + std::to_string(cfg.outline_size) + ",\n";
	out += "  \"radius\": " + std::to_string(cfg.bg_radius) + ",\n";
	out += "  \"padding\": " + std::to_string(cfg.bg_padding) + ",\n";
	out += "  \"line_spacing\": " + std::to_string(cfg.line_spacing) + ",\n";
	out += "  \"emote_scale\": " + std::to_string(cfg.emote_scale) + ",\n";
	out += "  \"align\": \"" + std::string(cfg.text_align == 1 ? "center"
						    : cfg.text_align == 2 ? "right"
									  : "left") +
	       "\"\n";
	out += "}\n";
	return out;
}

std::vector<std::pair<std::string, Theme>> theme_presets()
{
	std::vector<std::pair<std::string, Theme>> presets;

	{
		Theme theme;
		theme.name = "StreamElements classic";
		theme.font_face = "Segoe UI";
		theme.font_size = 30.0f;
		theme.bold = true;
		theme.text_color = 0xFFFFFFFFu;
		theme.outline_color = 0xE0000000u;
		theme.outline_size = 2;
		theme.bg_color = 0x00000000u;
		theme.bg_padding = 4;
		theme.bg_radius = 0;
		theme.line_spacing = 6;
		presets.emplace_back("se-classic", theme);
	}
	{
		Theme theme;
		theme.name = "Bubbles";
		theme.font_face = "Segoe UI";
		theme.font_size = 28.0f;
		theme.bold = false;
		theme.text_color = 0xFFFFFFFFu;
		theme.outline_color = 0x00000000u;
		theme.outline_size = 0;
		theme.bg_color = 0x99000000u;
		theme.bg_radius = 14;
		theme.bg_padding = 10;
		theme.line_spacing = 8;
		presets.emplace_back("bubbles", theme);
	}
	{
		Theme theme;
		theme.name = "Dark panel";
		theme.font_face = "Segoe UI";
		theme.font_size = 26.0f;
		theme.bold = false;
		theme.text_color = 0xFFEFEFEFu;
		theme.outline_color = 0x80000000u;
		theme.outline_size = 1;
		theme.bg_color = 0xB3000000u;
		theme.bg_radius = 6;
		theme.bg_padding = 12;
		theme.line_spacing = 6;
		presets.emplace_back("dark-panel", theme);
	}
	{
		Theme theme;
		theme.name = "Clean";
		theme.font_face = "Segoe UI";
		theme.font_size = 32.0f;
		theme.bold = true;
		theme.text_color = 0xFFFFFFFFu;
		theme.outline_color = 0x00000000u;
		theme.outline_size = 0;
		theme.bg_color = 0x00000000u;
		theme.bg_padding = 0;
		theme.bg_radius = 0;
		theme.line_spacing = 10;
		presets.emplace_back("clean", theme);
	}

	return presets;
}

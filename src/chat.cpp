#include "chat.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace {

std::string unescape_tag_value(std::string_view in)
{
	std::string out;
	out.reserve(in.size());
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] != '\\' || i + 1 >= in.size()) {
			out.push_back(in[i]);
			continue;
		}
		const char next = in[i + 1];
		switch (next) {
		case ':':
			out.push_back(';');
			break;
		case 's':
			out.push_back(' ');
			break;
		case '\\':
			out.push_back('\\');
			break;
		case 'r':
			out.push_back('\r');
			break;
		case 'n':
			out.push_back('\n');
			break;
		default:
			out.push_back(next);
			break;
		}
		++i;
	}
	return out;
}

// Splits "a=1;b=2" into pairs, unescaping values.
void parse_tags(std::string_view raw, std::vector<std::pair<std::string, std::string>> &out)
{
	size_t pos = 0;
	while (pos < raw.size()) {
		size_t end = raw.find(';', pos);
		if (end == std::string_view::npos)
			end = raw.size();
		const std::string_view item = raw.substr(pos, end - pos);
		const size_t eq = item.find('=');
		if (eq == std::string_view::npos) {
			out.emplace_back(std::string(item), std::string());
		} else {
			out.emplace_back(std::string(item.substr(0, eq)), unescape_tag_value(item.substr(eq + 1)));
		}
		pos = end + 1;
	}
}

const std::string *find_tag(const std::vector<std::pair<std::string, std::string>> &tags,
			    const char *name)
{
	for (const auto &kv : tags) {
		if (kv.first == name)
			return &kv.second;
	}
	return nullptr;
}

void parse_emotes_tag(const std::string &value, std::vector<EmoteSpan> &out)
{
	// Format: "25:0-4,12-16/1902:6-10"
	size_t pos = 0;
	while (pos < value.size()) {
		size_t slash = value.find('/', pos);
		if (slash == std::string::npos)
			slash = value.size();
		const std::string group = value.substr(pos, slash - pos);
		const size_t colon = group.find(':');
		if (colon != std::string::npos) {
			EmoteSpan span;
			span.id = group.substr(0, colon);
			size_t range_pos = colon + 1;
			while (range_pos < group.size()) {
				size_t comma = group.find(',', range_pos);
				if (comma == std::string::npos)
					comma = group.size();
				const std::string range = group.substr(range_pos, comma - range_pos);
				const size_t dash = range.find('-');
				if (dash != std::string::npos) {
					const uint32_t a = (uint32_t)std::strtoul(range.substr(0, dash).c_str(), nullptr, 10);
					const uint32_t b = (uint32_t)std::strtoul(range.substr(dash + 1).c_str(), nullptr, 10);
					EmoteSpan item = span;
					item.start_cp = a;
					item.end_cp = b;
					out.push_back(item);
				}
				range_pos = comma + 1;
			}
		}
		pos = slash + 1;
	}
	std::sort(out.begin(), out.end(),
		  [](const EmoteSpan &l, const EmoteSpan &r) { return l.start_cp < r.start_cp; });
}

void parse_badges_tag(const std::string &value, std::vector<BadgeRef> &out)
{
	// Format: "subscriber/12,premium/1"
	size_t pos = 0;
	while (pos < value.size()) {
		size_t comma = value.find(',', pos);
		if (comma == std::string::npos)
			comma = value.size();
		const std::string item = value.substr(pos, comma - pos);
		const size_t slash = item.find('/');
		if (slash != std::string::npos) {
			BadgeRef badge;
			badge.set_id = item.substr(0, slash);
			badge.version = item.substr(slash + 1);
			if (!badge.set_id.empty())
				out.push_back(badge);
		}
		pos = comma + 1;
	}
}

bool word_boundary_at(std::string_view text, size_t pos)
{
	if (pos >= text.size())
		return true;
	const unsigned char c = (unsigned char)text[pos];
	return !(std::isalnum(c) || c >= 0x80);
}

} // namespace

IrcKind parse_irc_line(const std::string &line, ChatMessage &msg, std::string &ping_token)
{
	if (line.empty())
		return IrcKind::Ignored;

	std::vector<std::pair<std::string, std::string>> tags;
	std::string_view rest(line);

	if (rest.front() == '@') {
		const size_t space = rest.find(' ');
		if (space == std::string_view::npos)
			return IrcKind::Ignored;
		parse_tags(rest.substr(1, space - 1), tags);
		rest = rest.substr(space + 1);
	}

	// Optional prefix ":nick!nick@nick"
	if (!rest.empty() && rest.front() == ':') {
		const size_t space = rest.find(' ');
		if (space == std::string_view::npos)
			return IrcKind::Ignored;
		rest = rest.substr(space + 1);
	}

	const size_t space = rest.find(' ');
	if (space == std::string_view::npos)
		return IrcKind::Ignored;

	const std::string_view command = rest.substr(0, space);
	rest = rest.substr(space + 1);

	if (command == "PING") {
		ping_token = std::string(rest);
		return IrcKind::Ping;
	}
	if (command != "PRIVMSG")
		return IrcKind::Ignored;

	// ":text"
	const size_t colon = rest.find(" :");
	if (colon == std::string_view::npos)
		return IrcKind::Ignored;

	msg.text = std::string(rest.substr(colon + 2));

	if (const std::string *v = find_tag(tags, "display-name"); v && !v->empty())
		msg.display = *v;
	if (const std::string *v = find_tag(tags, "color"); v)
		msg.color = *v;
	if (const std::string *v = find_tag(tags, "login"); v)
		msg.user = to_lower_ascii(*v);
	if (msg.user.empty() && !msg.display.empty())
		msg.user = to_lower_ascii(msg.display);
	if (const std::string *v = find_tag(tags, "emotes"); v && !v->empty())
		parse_emotes_tag(*v, msg.emotes);
	if (const std::string *v = find_tag(tags, "badges"); v && !v->empty())
		parse_badges_tag(*v, msg.badges);

	for (const auto &tag : tags)
		msg.tags[tag.first] = tag.second;

	// /me messages arrive wrapped in the ACTION CTCP marker.
	const std::string action_prefix = "\x01" "ACTION ";
	if (msg.text.size() > action_prefix.size() && msg.text.compare(0, action_prefix.size(), action_prefix) == 0) {
		msg.action = true;
		msg.text = msg.text.substr(action_prefix.size());
		if (!msg.text.empty() && msg.text.back() == '\x01')
			msg.text.pop_back();

		// Twitch usually reports emote ranges against the raw text including
		// the "\x01ACTION " wrapper. Detect that case (a range pointing past the
		// stripped text) and shift the ranges; otherwise leave them untouched.
		if (!msg.emotes.empty()) {
			const size_t stripped_cp = utf8_length(msg.text);
			const bool includes_prefix = std::any_of(
				msg.emotes.begin(), msg.emotes.end(),
				[stripped_cp](const EmoteSpan &span) { return span.end_cp >= stripped_cp; });
			if (includes_prefix) {
				const uint32_t shift = (uint32_t)action_prefix.size();
				std::vector<EmoteSpan> shifted;
				shifted.reserve(msg.emotes.size());
				for (const EmoteSpan &span : msg.emotes) {
					if (span.start_cp < shift || span.end_cp < span.start_cp)
						continue;
					shifted.push_back({span.start_cp - shift, span.end_cp - shift,
							   span.id});
				}
				msg.emotes.swap(shifted);
			}
		}
	}

	return IrcKind::Privmsg;
}

bool moderation_allows(const Config &cfg, const ChatMessage &msg)
{
	const std::string lower = to_lower_ascii(msg.text);

	if (!msg.user.empty()) {
		for (const std::string &ignored : split_list(cfg.ignore_users, ',')) {
			if (iequals_ascii(trim(ignored), msg.user))
				return false;
		}
	}

	if (cfg.hide_commands && !msg.action) {
		for (const std::string &prefix : split_list(cfg.command_prefix, ',')) {
			const std::string p = trim(prefix);
			if (!p.empty() && msg.text.rfind(p, 0) == 0)
				return false;
		}
	}

	if (cfg.hide_links) {
		static const char *patterns[] = {"http://", "https://", "www.", "discord.gg/", "t.me/",
						 "youtu.be/", "clips.twitch.tv/"};
		for (const char *pattern : patterns) {
			if (lower.find(pattern) != std::string::npos)
				return false;
		}
	}

	if (cfg.caps_threshold > 0 && !msg.action) {
		size_t letters = 0;
		size_t upper = 0;
		for (const char c : msg.text) {
			if (c >= 'a' && c <= 'z') {
				++letters;
			} else if (c >= 'A' && c <= 'Z') {
				++letters;
				++upper;
			}
		}
		if (letters >= 10 && (upper * 100) / letters >= (size_t)cfg.caps_threshold)
			return false;
	}

	if (!cfg.stopwords.empty()) {
		for (const std::string &word_raw : split_list(cfg.stopwords, ',')) {
			const std::string word = to_lower_ascii(trim(word_raw));
			if (word.empty())
				continue;
			size_t pos = lower.find(word);
			while (pos != std::string::npos) {
				const bool left_ok = pos == 0 || word_boundary_at(lower, pos - 1);
				const bool right_ok = word_boundary_at(lower, pos + word.size());
				if (left_ok && right_ok)
					return false;
				pos = lower.find(word, pos + 1);
			}
		}
	}

	return true;
}

std::string to_lower_ascii(std::string_view src)
{
	std::string out;
	out.reserve(src.size());
	for (const char c : src) {
		if (c >= 'A' && c <= 'Z')
			out.push_back((char)(c - 'A' + 'a'));
		else
			out.push_back(c);
	}
	return out;
}

bool iequals_ascii(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
		return false;
	for (size_t i = 0; i < a.size(); ++i) {
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'Z')
			ca = (char)(ca - 'A' + 'a');
		if (cb >= 'A' && cb <= 'Z')
			cb = (char)(cb - 'A' + 'a');
		if (ca != cb)
			return false;
	}
	return true;
}

bool starts_with_ci(std::string_view haystack, std::string_view needle)
{
	if (needle.size() > haystack.size())
		return false;
	return iequals_ascii(haystack.substr(0, needle.size()), needle);
}

std::string trim(std::string_view src)
{
	size_t start = 0;
	size_t end = src.size();
	while (start < end && std::isspace((unsigned char)src[start]))
		++start;
	while (end > start && std::isspace((unsigned char)src[end - 1]))
		--end;
	return std::string(src.substr(start, end - start));
}

std::vector<std::string> split_list(const std::string &value, char sep)
{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= value.size()) {
		size_t next = value.find(sep, pos);
		if (next == std::string::npos)
			next = value.size();
		out.push_back(value.substr(pos, next - pos));
		pos = next + 1;
	}
	return out;
}

std::wstring utf8_to_wide(std::string_view src)
{
	std::wstring out;
	out.reserve(src.size());
	size_t i = 0;
	while (i < src.size()) {
		const unsigned char c = (unsigned char)src[i];
		uint32_t cp = 0;
		size_t extra = 0;
		if (c < 0x80) {
			cp = c;
			extra = 0;
		} else if ((c & 0xE0) == 0xC0) {
			cp = c & 0x1F;
			extra = 1;
		} else if ((c & 0xF0) == 0xE0) {
			cp = c & 0x0F;
			extra = 2;
		} else if ((c & 0xF8) == 0xF0) {
			cp = c & 0x07;
			extra = 3;
		} else {
			cp = 0xFFFD;
			extra = 0;
		}
		if (i + extra >= src.size() && extra > 0) {
			out.push_back(0xFFFD);
			break;
		}
		for (size_t k = 1; k <= extra; ++k)
			cp = (cp << 6) | ((unsigned char)src[i + k] & 0x3F);
		i += extra + 1;

		if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
			cp -= 0x10000;
			out.push_back((wchar_t)(0xD800 + (cp >> 10)));
			out.push_back((wchar_t)(0xDC00 + (cp & 0x3FF)));
		} else {
			out.push_back((wchar_t)cp);
		}
	}
	return out;
}

std::string wide_to_utf8(std::wstring_view src)
{
	std::string out;
	out.reserve(src.size());
	for (size_t i = 0; i < src.size(); ++i) {
		uint32_t cp = (uint32_t)src[i];
		if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < src.size()) {
			const uint32_t low = (uint32_t)src[i + 1];
			if (low >= 0xDC00 && low <= 0xDFFF) {
				cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
				++i;
			}
		}
		if (cp < 0x80) {
			out.push_back((char)cp);
		} else if (cp < 0x800) {
			out.push_back((char)(0xC0 | (cp >> 6)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back((char)(0xE0 | (cp >> 12)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			out.push_back((char)(0xF0 | (cp >> 18)));
			out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}
	return out;
}

size_t utf8_length(std::string_view src)
{
	size_t count = 0;
	for (size_t i = 0; i < src.size();) {
		const unsigned char c = (unsigned char)src[i];
		size_t step = 1;
		if ((c & 0xE0) == 0xC0)
			step = 2;
		else if ((c & 0xF0) == 0xE0)
			step = 3;
		else if ((c & 0xF8) == 0xF0)
			step = 4;
		i += step;
		++count;
	}
	return count;
}

void EmoteRegistry::set_global(std::vector<EmoteEntry> entries)
{
	std::lock_guard<std::mutex> lock(mutex_);
	global_.clear();
	for (EmoteEntry &entry : entries) {
		if (!entry.name.empty() && !entry.key.empty())
			global_[to_lower_ascii(entry.name)] = entry.key;
	}
}

void EmoteRegistry::set_channel(std::vector<EmoteEntry> entries)
{
	std::lock_guard<std::mutex> lock(mutex_);
	channel_.clear();
	for (EmoteEntry &entry : entries) {
		if (!entry.name.empty() && !entry.key.empty())
			channel_[to_lower_ascii(entry.name)] = entry.key;
	}
}

void EmoteRegistry::clear()
{
	std::lock_guard<std::mutex> lock(mutex_);
	channel_.clear();
	global_.clear();
}

bool EmoteRegistry::lookup(const std::string &name, std::string &key) const
{
	const std::string lower = to_lower_ascii(name);
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = channel_.find(lower);
	if (it != channel_.end()) {
		key = it->second;
		return true;
	}
	it = global_.find(lower);
	if (it != global_.end()) {
		key = it->second;
		return true;
	}
	return false;
}

size_t EmoteRegistry::size() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return channel_.size() + global_.size();
}

double monotonic_seconds()
{
	static const auto start = std::chrono::steady_clock::now();
	const auto now = std::chrono::steady_clock::now();
	return std::chrono::duration<double>(now - start).count();
}

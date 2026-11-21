#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// ---------------------------------------------------------------------------
// Message model
// ---------------------------------------------------------------------------

struct BadgeRef {
	std::string set_id;
	std::string version;
};

// Emote replacements reported by Twitch in the IRC "emotes" tag.
// start_cp / end_cp are inclusive indexes in Unicode code points of the text.
struct EmoteSpan {
	uint32_t start_cp = 0;
	uint32_t end_cp = 0;
	std::string id;
};

struct ChatMessage {
	uint64_t seq = 0;
	std::string user;    // lowercase login
	std::string display; // display name as typed by the viewer
	std::string color;   // "#RRGGBB" or empty when Twitch did not send one
	std::string text;    // UTF-8 body
	bool action = false; // /me message
	std::vector<EmoteSpan> emotes;
	std::vector<BadgeRef> badges;
	// Raw IRC tags; the widget expects the full set (id, color, tmi-sent-ts...).
	std::map<std::string, std::string> tags;
	double recv_time = 0.0;
};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

enum class Anchor : int {
	BottomLeft = 0,
	BottomRight = 1,
	TopLeft = 2,
	TopRight = 3,
};

struct Config {
	// Connection
	std::string channel;
	std::string oauth;
	std::string username;
	std::string client_id;
	bool use_tls = true;
	int port = 443;

	// Canvas / appearance
	uint32_t width = 1920;
	uint32_t height = 1080;
	std::string font_face = "Segoe UI";
	float font_size = 28.0f;
	bool bold = true;
	bool italic = false;
	uint32_t text_color = 0xFFFFFFFFu;     // 0xAABBGGRR (OBS colour order)
	uint32_t nick_color = 0xFFD0A060u;     // fallback when no IRC colour is present
	uint32_t bg_color = 0x80000000u;
	uint32_t outline_color = 0xFF000000u;
	int outline_size = 2;
	int bg_padding = 8;
	int bg_radius = 6;
	int line_spacing = 4;
	float emote_scale = 1.0f;
	bool show_badges = true;
	bool show_timestamps = false;
	bool show_nick = true;

	// Layout
	Anchor anchor = Anchor::BottomLeft;
	int margin_x = 32;
	int margin_y = 32;
	int max_width = 900;
	int text_align = 0; // 0 left, 1 center, 2 right

	// Behaviour
	int max_messages = 15;
	int ttl_seconds = 0; // 0 = keep forever
	int fade_ms = 500;

	// Moderation
	bool hide_commands = true;
	std::string command_prefix = "!";
	bool hide_links = true;
	int caps_threshold = 70; // 0 disables the filter
	std::string stopwords;
	std::string ignore_users;

	// Emotes
	bool twitch_emotes = true;
	bool bttv_emotes = true;
	bool seventv_emotes = true;

	// Network
	std::string proxy; // HTTP proxy "host:port", empty for a direct connection
	bool debug_log = false; // log raw IRC traffic and HTTP failures
	bool demo_mode = false; // inject local test messages instead of connecting

	// Widget mode: run a StreamElements HTML/CSS/JS widget in a browser source
	bool widget_mode = false;
	std::string widget_dir;
	// Use the viewer's Twitch colour for nicknames instead of the widget's own.
	bool twitch_nick_colors = true;
	// Frames per second the embedded browser renders at. Lower values save a lot
	// of CPU on weak machines.
	int widget_fps = 15;
};

// ---------------------------------------------------------------------------
// IRC
// ---------------------------------------------------------------------------

enum class IrcKind {
	Ignored,
	Ping,
	Privmsg,
};

// Parses a single raw IRC line. For IrcKind::Ping, `ping_token` receives the
// payload that must be echoed back in a PONG.
IrcKind parse_irc_line(const std::string &line, ChatMessage &msg, std::string &ping_token);

// ---------------------------------------------------------------------------
// Third-party emote registry (BTTV / 7TV)
// ---------------------------------------------------------------------------

struct EmoteEntry {
	std::string name;
	std::string key;
};

class EmoteRegistry {
public:
	void set_global(std::vector<EmoteEntry> entries);
	void set_channel(std::vector<EmoteEntry> entries);
	void clear();

	// Looks up an emote by its text name; channel emotes win over global ones.
	bool lookup(const std::string &name, std::string &key) const;
	size_t size() const;

private:
	mutable std::mutex mutex_;
	std::map<std::string, std::string> channel_;
	std::map<std::string, std::string> global_;
};

// ---------------------------------------------------------------------------
// Moderation
// ---------------------------------------------------------------------------

bool moderation_allows(const Config &cfg, const ChatMessage &msg);

// ---------------------------------------------------------------------------
// Small utilities
// ---------------------------------------------------------------------------

std::string to_lower_ascii(std::string_view src);
bool iequals_ascii(std::string_view a, std::string_view b);
bool starts_with_ci(std::string_view haystack, std::string_view needle);
std::string trim(std::string_view src);
std::vector<std::string> split_list(const std::string &value, char sep);
std::wstring utf8_to_wide(std::string_view src);
std::string wide_to_utf8(std::wstring_view src);
// Length of the UTF-8 string in Unicode code points.
size_t utf8_length(std::string_view src);
// Monotonic clock in seconds, used for message ages and fade timing.
double monotonic_seconds();

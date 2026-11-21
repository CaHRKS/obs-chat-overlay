#include "twitch.hpp"

#include "json.hpp"

#include <obs-module.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <objbase.h>
#include <sstream>
#include <string_view>
#include <thread>
#include <utility>

namespace {

std::string strip_oauth(const std::string &token)
{
	std::string value = trim(token);
	if (starts_with_ci(value, "oauth:"))
		value = trim(value.substr(6));
	return value;
}

std::string url_encode(const std::string &value)
{
	static const char *kHex = "0123456789ABCDEF";
	std::string out;
	for (const unsigned char c : value) {
		const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
				   (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~';
		if (plain) {
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(kHex[c >> 4]);
			out.push_back(kHex[c & 0x0F]);
		}
	}
	return out;
}

std::string twitch_emote_url(const std::string &id)
{
	return "https://static-cdn.jtvnw.net/emoticons/v2/" + id + "/default/dark/2.0";
}

// Pulls the numeric channel id out of an IRC tag block ("...room-id=12345;...").
std::string extract_room_id(std::string_view line)
{
	const size_t pos = line.find("room-id=");
	if (pos == std::string_view::npos)
		return std::string();
	size_t begin = pos + 8;
	size_t end = begin;
	while (end < line.size() && line[end] >= '0' && line[end] <= '9')
		++end;
	if (end == begin)
		return std::string();
	return std::string(line.substr(begin, end - begin));
}

} // namespace

// ---------------------------------------------------------------------------
// Emote cache: keeps the BTTV/7TV sets on disk so emotes are available right
// after OBS starts, before (or even without) any network round-trip.
// ---------------------------------------------------------------------------

namespace {

std::string emote_cache_dir()
{
	const char *appdata = std::getenv("APPDATA");
	std::string base = (appdata && *appdata) ? appdata : ".";
	base += "\\obs-studio\\plugin_config\\obs-chat-overlay";
	return base;
}

std::string emote_cache_path(const std::string &login)
{
	std::string safe;
	for (const char c : to_lower_ascii(login)) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
		if (ok)
			safe.push_back(c);
	}
	if (safe.empty())
		safe = "default";
	return emote_cache_dir() + "\\emotes-" + safe + ".json";
}

std::string emote_url_for_key(const std::string &key)
{
	if (key.rfind("7tv:", 0) == 0)
		return "https://cdn.7tv.app/emote/" + key.substr(4) + "/2x.png";
	if (key.rfind("bttv:", 0) == 0)
		return "https://cdn.betterttv.net/emote/" + key.substr(5) + "/2x";
	return std::string();
}

std::string cache_escape(const std::string &text)
{
	std::string out;
	out.reserve(text.size() + 8);
	for (const char c : text) {
		if (c == '"' || c == '\\')
			out.push_back('\\');
		if (c == '\n' || c == '\r')
			out.push_back(' ');
		else
			out.push_back(c);
	}
	return out;
}

bool load_emote_cache(const std::string &login, std::string &cached_id,
		      std::vector<EmoteEntry> &global, std::vector<EmoteEntry> &channel)
{
	std::ifstream file(emote_cache_path(login), std::ios::binary);
	if (!file)
		return false;

	std::ostringstream buffer;
	buffer << file.rdbuf();

	json::Value root;
	if (!json::parse(buffer.str(), root) || !root.is_object())
		return false;

	cached_id = root.get_string("id");
	const auto add = [](const json::Value &items, std::vector<EmoteEntry> &out) {
		for (size_t i = 0; i < items.size(); ++i) {
			const std::string name = items.at(i).get_string("n");
			const std::string source = items.at(i).get_string("s");
			const std::string id = items.at(i).get_string("i");
			if (name.empty() || id.empty())
				continue;
			out.push_back({name, source + ":" + id});
		}
	};
	add(root["global"], global);
	add(root["channel"], channel);
	return !global.empty() || !channel.empty();
}

void save_emote_cache(const std::string &login, const std::string &channel_id,
		      const std::vector<EmoteEntry> &global,
		      const std::vector<EmoteEntry> &channel)
{
	std::error_code ec;
	std::filesystem::create_directories(emote_cache_dir(), ec);
	if (ec)
		return;

	const auto append = [](std::string &out, const std::vector<EmoteEntry> &items) {
		out += "[";
		bool first = true;
		for (const EmoteEntry &entry : items) {
			const size_t colon = entry.key.find(':');
			if (colon == std::string::npos)
				continue;
			const std::string source = entry.key.substr(0, colon);
			const std::string id = entry.key.substr(colon + 1);
			if (source != "7tv" && source != "bttv")
				continue;
			if (!first)
				out += ",";
			first = false;
			out += "{\"n\":\"" + cache_escape(entry.name) + "\",\"s\":\"" + source +
			       "\",\"i\":\"" + cache_escape(id) + "\"}";
		}
		out += "]";
	};

	std::string body = "{\"login\":\"" + cache_escape(login) + "\",\"id\":\"" +
			   cache_escape(channel_id) + "\",\"global\":";
	append(body, global);
	body += ",\"channel\":";
	append(body, channel);
	body += "}";

	std::ofstream out(emote_cache_path(login), std::ios::binary | std::ios::trunc);
	if (out)
		out << body;
}

} // namespace

// ---------------------------------------------------------------------------
// Shared state: everything the network thread touches lives here, so the thread
// may safely outlive the TwitchWorker instance.
// ---------------------------------------------------------------------------

struct TwitchWorker::State {
	Config cfg;
	std::shared_ptr<EmoteRegistry> emotes;
	std::shared_ptr<ImageCache> images;
	WebSocket socket;

	mutable std::mutex mutex;
	std::deque<ChatMessage> queue;
	std::string status;
	std::string room_id; // numeric channel id from the IRC "room-id" tag

	std::atomic<bool> stop{false};
	std::atomic<bool> done{false};
	std::atomic<bool> connected{false};
	std::atomic<uint64_t> seq{0};

	// Only touched by the network thread.
	bool first_message_logged = false;
};

void TwitchWorker::apply_cached_emotes(State &state)
{
	std::string cached_id;
	std::vector<EmoteEntry> global_entries;
	std::vector<EmoteEntry> channel_entries;
	if (!load_emote_cache(state.cfg.channel, cached_id, global_entries, channel_entries))
		return;

	if (state.images) {
		for (const EmoteEntry &entry : global_entries) {
			const std::string url = emote_url_for_key(entry.key);
			if (!url.empty())
				state.images->register_image(entry.key, url);
		}
		for (const EmoteEntry &entry : channel_entries) {
			const std::string url = emote_url_for_key(entry.key);
			if (!url.empty())
				state.images->register_image(entry.key, url);
		}
	}

	if (state.emotes) {
		state.emotes->set_global(std::move(global_entries));
		state.emotes->set_channel(std::move(channel_entries));
	}
	if (!cached_id.empty() && room_id(state).empty())
		set_room_id(state, cached_id);

	blog(LOG_INFO, "[chat-overlay] emotes restored from cache for #%s (channel id %s)",
	     state.cfg.channel.c_str(), cached_id.empty() ? "unknown" : cached_id.c_str());
}

std::string TwitchWorker::resolve_channel_id_public(State &state)
{
	const std::string login = to_lower_ascii(state.cfg.channel);
	if (login.empty())
		return std::string();

	// ivr.fi mirrors Twitch without requiring Client-ID/OAuth, so the channel id
	// is available even when the IRC "room-id" tag never arrives.
	const HttpResult user = http_get(
		"https://api.ivr.fi/v2/twitch/user?login=" + url_encode(login), {},
		state.cfg.proxy);
	if (!user.ok)
		return std::string();

	json::Value root;
	if (!json::parse(user.body, root))
		return std::string();

	std::string id;
	if (root.is_array() && root.size() > 0)
		id = root.at(0).get_string("id");
	else if (root.is_object())
		id = root.get_string("id");

	if (!id.empty())
		blog(LOG_INFO, "[chat-overlay] channel id %s resolved by login", id.c_str());
	return id;
}

TwitchWorker::TwitchWorker(const Config &cfg, std::shared_ptr<EmoteRegistry> emotes,
			   std::shared_ptr<ImageCache> images)
	: state_(std::make_shared<State>())
{
	state_->cfg = cfg;
	state_->emotes = std::move(emotes);
	state_->images = std::move(images);
}

TwitchWorker::~TwitchWorker()
{
	stop();
}

void TwitchWorker::start()
{
	if (thread_.joinable())
		return;
	if (state_->stop.load())
		return; // already stopped once, a stopped worker cannot be reused
	state_->done = false;
	std::shared_ptr<State> state = state_;
	thread_ = std::thread([state]() { TwitchWorker::thread_main(state); });
}

void TwitchWorker::stop()
{
	if (!thread_.joinable())
		return;

	state_->stop = true;
	state_->socket.abort();

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
	while (std::chrono::steady_clock::now() < deadline) {
		if (state_->done.load())
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	if (state_->done.load())
		thread_.join();
	else
		thread_.detach();
}

std::string TwitchWorker::status() const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	return state_->status;
}

bool TwitchWorker::connected() const
{
	return state_->connected.load();
}

size_t TwitchWorker::drain(std::vector<ChatMessage> &out, size_t max)
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	size_t count = 0;
	while (!state_->queue.empty() && count < max) {
		out.push_back(std::move(state_->queue.front()));
		state_->queue.pop_front();
		++count;
	}
	return count;
}

void TwitchWorker::set_status(State &state, const std::string &text)
{
	std::lock_guard<std::mutex> lock(state.mutex);
	state.status = text;
}

void TwitchWorker::push(State &state, ChatMessage msg)
{
	std::lock_guard<std::mutex> lock(state.mutex);
	if (state.queue.size() > 200)
		state.queue.pop_front();
	state.queue.push_back(std::move(msg));
}

bool TwitchWorker::sleep_interruptible(State &state, int milliseconds)
{
	const int step = 50;
	int elapsed = 0;
	while (elapsed < milliseconds) {
		if (state.stop.load())
			return false;
		std::this_thread::sleep_for(std::chrono::milliseconds(step));
		elapsed += step;
	}
	return !state.stop.load();
}

void TwitchWorker::thread_main(std::shared_ptr<State> state)
{
	run(state);
	state->connected = false;
	state->done = true;
}

void TwitchWorker::run(std::shared_ptr<State> state_ptr)
{
	State &state = *state_ptr;
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	blog(LOG_INFO, "[chat-overlay] worker start: channel=%s tls=%d proxy=%s debug=%d",
	     state.cfg.channel.empty() ? "<none>" : state.cfg.channel.c_str(),
	     state.cfg.use_tls ? 1 : 0, state.cfg.proxy.empty() ? "<direct>" : state.cfg.proxy.c_str(),
	     state.cfg.debug_log ? 1 : 0);

	// Badges and BTTV/7TV emotes take many seconds to load (several HTTP calls).
	// Do that in the background so the IRC connection is established immediately
	// and chat shows up within a second.
	std::thread metadata_thread([state_ptr]() {
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		TwitchWorker::load_metadata(*state_ptr);
		CoUninitialize();
	});
	metadata_thread.detach();

	int backoff = 2;
	while (!state.stop.load()) {
		if (state.cfg.channel.empty()) {
			set_status(state, "channel is not configured");
			if (!sleep_interruptible(state, 1000))
				break;
			continue;
		}

		std::string error;
		if (connect_irc(state, error)) {
			backoff = 2;
			read_loop(state);
			state.connected = false;
			if (state.stop.load())
				break;
			set_status(state, "disconnected, reconnecting");
			blog(LOG_WARNING, "[chat-overlay] connection lost, reconnecting");
		} else {
			set_status(state, error);
			blog(LOG_WARNING, "[chat-overlay] twitch: %s", error.c_str());
		}

		if (!sleep_interruptible(state, backoff * 1000))
			break;
		backoff = backoff * 2 > 30 ? 30 : backoff * 2;
	}

	state.connected = false;
	state.socket.close();
	blog(LOG_INFO, "[chat-overlay] worker stopped");
	CoUninitialize();
}

bool TwitchWorker::connect_irc(State &state, std::string &error)
{
	const bool use_tls = state.cfg.use_tls;
	const std::string url =
		use_tls ? "wss://irc-ws.chat.twitch.tv:443" : "ws://irc-ws.chat.twitch.tv:80";

	blog(LOG_INFO, "[chat-overlay] connecting to %s", url.c_str());
	if (!state.socket.connect(url, {}, state.cfg.proxy, &error)) {
		blog(LOG_WARNING, "[chat-overlay] websocket handshake failed: %s", error.c_str());
		return false;
	}
	blog(LOG_INFO, "[chat-overlay] websocket open");

	state.socket.send_text("CAP REQ :twitch.tv/tags twitch.tv/commands\r\n");

	const std::string token = strip_oauth(state.cfg.oauth);
	if (!token.empty() && !state.cfg.username.empty()) {
		state.socket.send_text("PASS oauth:" + token + "\r\n");
		state.socket.send_text("NICK " + to_lower_ascii(state.cfg.username) + "\r\n");
	} else {
		static std::atomic<int> fan_index{0};
		const int index = fan_index.fetch_add(1);
		state.socket.send_text("NICK justinfan" + std::to_string(10000 + index % 80000) + "\r\n");
	}

	state.socket.send_text("JOIN #" + to_lower_ascii(state.cfg.channel) + "\r\n");
	state.connected = true;
	set_status(state, "connected to #" + state.cfg.channel);
	blog(LOG_INFO, "[chat-overlay] connected to #%s", state.cfg.channel.c_str());
	return true;
}

void TwitchWorker::read_loop(State &state)
{
	std::string buffer;
	std::string message;

	while (!state.stop.load()) {
		if (!state.socket.receive_text(message, nullptr))
			break;

		buffer += message;
		if (buffer.size() > 512u * 1024u)
			buffer.clear();

		size_t newline = 0;
		while ((newline = buffer.find('\n')) != std::string::npos) {
			std::string line = buffer.substr(0, newline);
			buffer.erase(0, newline + 1);
			if (!line.empty() && line.back() == '\r')
				line.pop_back();
			if (line.empty())
				continue;

			// The numeric channel id comes for free in the IRC tags; it unlocks
			// channel badges and channel emotes without any credentials.
			if (line.find("room-id=") != std::string::npos) {
				const std::string id = extract_room_id(line);
				if (!id.empty() && room_id(state).empty()) {
					set_room_id(state, id);
					blog(LOG_INFO, "[chat-overlay] room-id from IRC: %s", id.c_str());
				}
			}

			if (state.cfg.debug_log)
				blog(LOG_INFO, "[chat-overlay] << %s", line.c_str());

			if (starts_with_ci(line, "PING")) {
				const std::string payload = trim(line.substr(4));
				state.socket.send_text("PONG " + payload + "\r\n");
				continue;
			}
			if (line.find("RECONNECT") != std::string::npos) {
				blog(LOG_INFO, "[chat-overlay] twitch asked to reconnect");
				state.connected = false;
				return;
			}

			ChatMessage msg;
			std::string ping;
			if (parse_irc_line(line, msg, ping) == IrcKind::Privmsg) {
				if (!state.first_message_logged) {
					blog(LOG_INFO,
					     "[chat-overlay] first message received from %s",
					     msg.user.empty() ? "<unknown>" : msg.user.c_str());
					state.first_message_logged = true;
				}
				msg.seq = ++state.seq;
				msg.recv_time = monotonic_seconds();
				register_twitch_emotes(state, msg);
				push(state, std::move(msg));
			}
		}
	}

	state.connected = false;
}

void TwitchWorker::register_twitch_emotes(State &state, const ChatMessage &msg)
{
	if (!state.cfg.twitch_emotes)
		return;
	for (const EmoteSpan &span : msg.emotes) {
		if (!span.id.empty())
			state.images->register_image("tw:" + span.id, twitch_emote_url(span.id));
	}
}

std::string TwitchWorker::helix_get(State &state, const std::string &path)
{
	const std::string token = strip_oauth(state.cfg.oauth);
	if (state.cfg.client_id.empty() || token.empty())
		return std::string();

	const HttpResult result = http_get("https://api.twitch.tv/helix" + path,
					   {{"Client-Id", state.cfg.client_id},
					    {"Authorization", "Bearer " + token}},
					   state.cfg.proxy);
	if (!result.ok) {
		blog(LOG_INFO, "[chat-overlay] helix %s failed: %ld %s", path.c_str(), result.status,
		     result.error.c_str());
		return std::string();
	}
	return result.body;
}

void TwitchWorker::set_room_id(State &state, const std::string &id)
{
	std::lock_guard<std::mutex> lock(state.mutex);
	if (state.room_id.empty())
		state.room_id = id;
}

std::string TwitchWorker::room_id(State &state)
{
	std::lock_guard<std::mutex> lock(state.mutex);
	return state.room_id;
}

void TwitchWorker::register_known_badges(State &state)
{
	// Global badges that do not depend on the channel. Used when the public
	// badge API is unavailable and no Client-ID/OAuth is configured.
	struct Known {
		const char *key;
		const char *uuid;
	};
	static const Known kKnown[] = {
		{"moderator/1", "3267646d-33f0-4b17-b3df-f923a41db1d0"},
		{"broadcaster/1", "5527c58c-fb7d-422d-b71b-f309dcb85cc1"},
		{"vip/1", "b817aba4-fad8-49e2-b88a-7cc744dfa6ec"},
		{"premium/1", "bbbe0db0-a598-423e-86d0-f9fb98ca1933"},
		{"partner/1", "d12a2e27-16f6-41d0-ab77-b780518f00a3"},
		{"staff/1", "d97c37bd-a6f5-4c38-8f57-4e4bef88af34"},
	};

	for (const Known &badge : kKnown) {
		state.images->register_image(
			"badge:" + std::string(badge.key),
			"https://static-cdn.jtvnw.net/badges/v1/" + std::string(badge.uuid) + "/2");
	}
}

// Reads badges from the legacy public endpoint, which needs no OAuth token.
// Returns the number of registered badge versions.
size_t TwitchWorker::load_badges_public(State &state, const std::string &room_id_value)
{
	if (state.stop.load())
		return 0;

	size_t registered = 0;

	const auto consume = [&state, &registered](const std::string &body) {
		if (body.empty())
			return;
		json::Value root;
		if (!json::parse(body, root))
			return;
		const json::Value *sets = root.find("badge_sets");
		if (!sets || !sets->is_object())
			return;
		for (const auto &set : sets->object) {
			const json::Value &versions = set.second["versions"];
			for (const auto &version : versions.object) {
				std::string url = version.second.get_string("image_url_2x");
				if (url.empty())
					url = version.second.get_string("image_url_1x");
				if (url.empty())
					continue;
				state.images->register_image("badge:" + set.first + "/" + version.first, url);
				++registered;
			}
		}
	};

	HttpResult global =
		http_get("https://badges.twitch.tv/v1/badges/global/display?language=en", {},
			 state.cfg.proxy);
	if (global.ok)
		consume(global.body);

	if (!room_id_value.empty()) {
		HttpResult channel = http_get("https://badges.twitch.tv/v1/badges/channels/" +
						      room_id_value + "/display?language=en",
					      {}, state.cfg.proxy);
		if (channel.ok)
			consume(channel.body);
	}

	return registered;
}

// Badges from the public ivr.fi mirror of the Twitch API. Needs no credentials
// and returns both global and channel badge sets for a channel login.
size_t TwitchWorker::load_badges_ivr(State &state)
{
	if (state.cfg.channel.empty())
		return 0;

	const HttpResult response = http_get(
		"https://api.ivr.fi/v2/twitch/badges/channel?login=" +
			url_encode(to_lower_ascii(state.cfg.channel)),
		{}, state.cfg.proxy);
	if (!response.ok || response.body.empty())
		return 0;
	// A newer channel may have replaced this worker while the request was in
	// flight; dropping the result keeps the new channel's badges intact.
	if (state.stop.load())
		return 0;

	json::Value root;
	if (!json::parse(response.body, root) || !root.is_array())
		return 0;

	size_t registered = 0;
	for (const json::Value &set : root.array) {
		const std::string set_id = set.get_string("set_id");
		if (set_id.empty())
			continue;
		const json::Value &versions = set["versions"];
		for (const json::Value &version : versions.array) {
			const std::string version_id = version.get_string("id");
			if (version_id.empty())
				continue;
			std::string url = version.get_string("image_url_2x");
			if (url.empty())
				url = version.get_string("image_url_1x");
			if (url.empty())
				continue;
			state.images->register_image("badge:" + set_id + "/" + version_id, url);
			++registered;
		}
	}
	return registered;
}

void TwitchWorker::load_metadata(State &state)
{
	// Restore the cached BTTV/7TV sets first: emotes then work immediately, even
	// while the network requests below are still running (or fail).
	apply_cached_emotes(state);

	// The numeric channel id arrives in the IRC "room-id" tag right after JOIN.
	// It is enough to fetch channel badges and channel BTTV/7TV emotes without
	// any Client-ID/OAuth.
	for (int i = 0; i < 100 && room_id(state).empty() && !state.stop.load(); ++i)
		std::this_thread::sleep_for(std::chrono::milliseconds(100));

	std::string id = room_id(state);
	const bool has_credentials =
		!state.cfg.client_id.empty() && !strip_oauth(state.cfg.oauth).empty();

	if (!state.cfg.channel.empty() && has_credentials) {
		const std::string body =
			helix_get(state, "/users?login=" + url_encode(to_lower_ascii(state.cfg.channel)));
		json::Value root;
		if (!body.empty() && json::parse(body, root) && root["data"].is_array() &&
		    root["data"].size() > 0) {
			const std::string resolved = root["data"].at(0).get_string("id");
			if (!resolved.empty())
				id = resolved;
		}
	}

	// No credentials and no IRC tag: ask the public mirror by login.
	if (id.empty())
		id = resolve_channel_id_public(state);
	if (!id.empty())
		set_room_id(state, id);

	if (id.empty())
		blog(LOG_WARNING,
		     "[chat-overlay] channel id is unknown: channel badges and channel emotes may be missing");
	else
		blog(LOG_INFO, "[chat-overlay] channel id %s", id.c_str());

	// Built-in global badges are the canonical art for mod/vip/prime/partner/
	// staff, so they are always registered first as a baseline; the remote
	// sources below only add channel-specific sets.
	register_known_badges(state);

	// Badge sources, best-effort without any user credentials:
	//   1. ivr.fi mirror (global + channel, merged)
	//   2. legacy badges.twitch.tv
	//   3. Helix, only when Client-ID + OAuth are configured
	size_t badges = load_badges_ivr(state);
	if (badges > 0)
		blog(LOG_INFO, "[chat-overlay] badges from ivr.fi mirror: %zu", badges);

	if (badges == 0) {
		badges = load_badges_public(state, id);
		if (badges > 0)
			blog(LOG_INFO, "[chat-overlay] badges from public API: %zu", badges);
	}

	if (badges == 0 && has_credentials)
		badges = load_badges(state, id);

	if (badges == 0)
		blog(LOG_WARNING,
		     "[chat-overlay] badge APIs unavailable: only built-in global badges "
		     "(mod/vip/prime/partner/staff) will be shown");

	load_emotes(state, id);
}

size_t TwitchWorker::load_badges(State &state, const std::string &broadcaster_id)
{
	if (state.stop.load())
		return 0;

	size_t registered = 0;
	auto consume = [&state, &registered](const std::string &body) {
		json::Value root;
		if (body.empty() || !json::parse(body, root))
			return;
		const json::Value &data = root["data"];
		for (size_t i = 0; i < data.size(); ++i) {
			const json::Value &set = data.at(i);
			const std::string set_id = set.get_string("set_id");
			if (set_id.empty())
				continue;
			const json::Value &versions = set["versions"];
			for (size_t v = 0; v < versions.size(); ++v) {
				const json::Value &version = versions.at(v);
				std::string url = version.get_string("image_url_2x");
				if (url.empty())
					url = version.get_string("image_url_1x");
				const std::string version_id = version.get_string("id");
				if (url.empty() || version_id.empty())
					continue;
				state.images->register_image("badge:" + set_id + "/" + version_id, url);
				++registered;
			}
		}
	};

	consume(helix_get(state, "/chat/badges/global"));
	if (!broadcaster_id.empty())
		consume(helix_get(state, "/chat/badges?broadcaster_id=" + broadcaster_id));
	blog(LOG_INFO, "[chat-overlay] badges from Helix: %zu", registered);
	return registered;
}

void TwitchWorker::load_emotes(State &state, const std::string &broadcaster_id)
{
	if (state.stop.load())
		return;

	std::vector<EmoteEntry> global_entries;
	std::vector<EmoteEntry> channel_entries;
	const std::string &proxy = state.cfg.proxy;

	if (state.cfg.seventv_emotes) {
		const HttpResult global = http_get("https://7tv.io/v3/emote-sets/global", {}, proxy);
		if (global.ok) {
			json::Value root;
			if (json::parse(global.body, root)) {
				const json::Value &items = root["emotes"];
				for (size_t i = 0; i < items.size(); ++i) {
					const std::string id = items.at(i).get_string("id");
					const std::string name = items.at(i).get_string("name");
					if (id.empty() || name.empty())
						continue;
					const std::string key = "7tv:" + id;
					state.images->register_image(
						key, "https://cdn.7tv.app/emote/" + id + "/2x.png");
					global_entries.push_back({name, key});
				}
			}
		}

		if (!broadcaster_id.empty()) {
			const HttpResult channel = http_get(
				"https://7tv.io/v3/users/twitch/" + broadcaster_id, {}, proxy);
			if (channel.ok) {
				json::Value root;
				if (json::parse(channel.body, root)) {
					const json::Value &items = root["emote_set"]["emotes"];
					for (size_t i = 0; i < items.size(); ++i) {
						const std::string id = items.at(i).get_string("id");
						const std::string name = items.at(i).get_string("name");
						if (id.empty() || name.empty())
							continue;
						const std::string key = "7tv:" + id;
						state.images->register_image(
							key, "https://cdn.7tv.app/emote/" + id +
								     "/2x.png");
						channel_entries.push_back({name, key});
					}
				}
			}
		}
	}

	if (state.cfg.bttv_emotes) {
		const HttpResult global =
			http_get("https://api.betterttv.net/3/cached/emotes/global", {}, proxy);
		if (global.ok) {
			json::Value root;
			if (json::parse(global.body, root) && root.is_array()) {
				for (size_t i = 0; i < root.size(); ++i) {
					const std::string id = root.at(i).get_string("id");
					const std::string code = root.at(i).get_string("code");
					if (id.empty() || code.empty())
						continue;
					const std::string key = "bttv:" + id;
					state.images->register_image(
						key, "https://cdn.betterttv.net/emote/" + id + "/2x");
					global_entries.push_back({code, key});
				}
			}
		}

		if (!broadcaster_id.empty()) {
			const HttpResult channel = http_get(
				"https://api.betterttv.net/3/cached/users/twitch/" + broadcaster_id,
				{}, proxy);
			if (channel.ok) {
				json::Value root;
				if (json::parse(channel.body, root)) {
					const char *sections[] = {"channelEmotes", "sharedEmotes"};
					for (const char *section : sections) {
						const json::Value &items = root[section];
						for (size_t i = 0; i < items.size(); ++i) {
							const std::string id =
								items.at(i).get_string("id");
							const std::string code =
								items.at(i).get_string("code");
							if (id.empty() || code.empty())
								continue;
							const std::string key = "bttv:" + id;
							state.images->register_image(
								key,
								"https://cdn.betterttv.net/emote/" +
									id + "/2x");
							channel_entries.push_back({code, key});
						}
					}
				}
			}
		}
	}

	// The channel may have been switched while the requests were running.
	if (state.stop.load())
		return;

	// If the channel API returned nothing (network hiccup, unknown id), keep the
	// channel set from the cache instead of losing it.
	if (channel_entries.empty() && !broadcaster_id.empty()) {
		std::string cached_id;
		std::vector<EmoteEntry> cached_global;
		std::vector<EmoteEntry> cached_channel;
		if (load_emote_cache(state.cfg.channel, cached_id, cached_global, cached_channel) &&
		    !cached_channel.empty()) {
			if (state.images) {
				for (const EmoteEntry &entry : cached_channel) {
					const std::string url = emote_url_for_key(entry.key);
					if (!url.empty())
						state.images->register_image(entry.key, url);
				}
			}
			channel_entries = std::move(cached_channel);
			blog(LOG_INFO, "[chat-overlay] channel emotes kept from cache: %zu",
			     channel_entries.size());
		}
	}

	const size_t global_count = global_entries.size();
	const size_t channel_count = channel_entries.size();

	// Remember the fresh sets for the next start. Only overwrite when the network
	// really returned something, otherwise a transient failure would wipe the cache.
	if (global_count > 0)
		save_emote_cache(state.cfg.channel, broadcaster_id, global_entries, channel_entries);

	state.emotes->set_global(std::move(global_entries));
	state.emotes->set_channel(std::move(channel_entries));
	blog(LOG_INFO, "[chat-overlay] emotes loaded: %zu (global %zu, channel %zu)",
	     state.emotes->size(), global_count, channel_count);
	if (!broadcaster_id.empty() && channel_count == 0)
		blog(LOG_INFO,
		     "[chat-overlay] channel emotes unavailable: 7TV/BTTV channel API returned "
		     "nothing for id %s",
		     broadcaster_id.c_str());
}

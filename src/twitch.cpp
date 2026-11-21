#include "twitch.hpp"

#include "json.hpp"

#include <obs-module.h>

#include <algorithm>
#include <chrono>
#include <objbase.h>
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

	std::atomic<bool> stop{false};
	std::atomic<bool> done{false};
	std::atomic<bool> connected{false};
	std::atomic<uint64_t> seq{0};

	// Only touched by the network thread.
	bool first_message_logged = false;
};

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

void TwitchWorker::load_metadata(State &state)
{
	std::string broadcaster_id;
	const bool has_credentials =
		!state.cfg.client_id.empty() && !strip_oauth(state.cfg.oauth).empty();

	if (!state.cfg.channel.empty() && has_credentials) {
		const std::string body =
			helix_get(state, "/users?login=" + url_encode(to_lower_ascii(state.cfg.channel)));
		json::Value root;
		if (!body.empty() && json::parse(body, root) && root["data"].is_array() &&
		    root["data"].size() > 0) {
			broadcaster_id = root["data"].at(0).get_string("id");
		}
		if (!broadcaster_id.empty())
			blog(LOG_INFO, "[chat-overlay] channel id %s", broadcaster_id.c_str());
		else
			blog(LOG_WARNING,
			     "[chat-overlay] cannot resolve channel id (check Client-ID/OAuth)");
	} else if (!state.cfg.channel.empty()) {
		blog(LOG_INFO,
		     "[chat-overlay] no Client-ID/OAuth: badges and channel emotes are disabled");
	}

	load_badges(state, broadcaster_id);
	load_emotes(state, broadcaster_id);
}

void TwitchWorker::load_badges(State &state, const std::string &broadcaster_id)
{
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
	blog(LOG_INFO, "[chat-overlay] badges registered: %zu", registered);
}

void TwitchWorker::load_emotes(State &state, const std::string &broadcaster_id)
{
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

	state.emotes->set_global(std::move(global_entries));
	state.emotes->set_channel(std::move(channel_entries));
	blog(LOG_INFO, "[chat-overlay] emotes loaded: %zu", state.emotes->size());
}

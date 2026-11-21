#pragma once

#include "chat.hpp"
#include "images.hpp"
#include "net.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Reads Twitch chat (IRC over websocket, anonymous or authenticated), resolves
// badges through the Helix API and loads BTTV / 7TV emotes. Everything runs on a
// background thread; messages are collected in a queue drained by the source.
//
// The network thread only ever touches an internal shared state, so it can
// safely outlive the TwitchWorker object during shutdown.
class TwitchWorker {
public:
	TwitchWorker(const Config &cfg, std::shared_ptr<EmoteRegistry> emotes,
		     std::shared_ptr<ImageCache> images);
	~TwitchWorker();

	TwitchWorker(const TwitchWorker &) = delete;
	TwitchWorker &operator=(const TwitchWorker &) = delete;

	void start();
	void stop();

	bool connected() const;
	std::string status() const;

	// Moves pending messages into `out`.
	size_t drain(std::vector<ChatMessage> &out, size_t max = 64);

private:
	struct State;

	static void thread_main(std::shared_ptr<State> state);
	static void run(std::shared_ptr<State> state);
	static bool connect_irc(State &state, std::string &error);
	static void read_loop(State &state);
	static void load_metadata(State &state);
	// Channel id resolution that does not depend on IRC/OAuth.
	static std::string resolve_channel_id_public(State &state);
	static size_t load_badges_ivr(State &state);
	static size_t load_badges_public(State &state, const std::string &room_id);
	static void register_known_badges(State &state);
	static size_t load_badges(State &state, const std::string &broadcaster_id);
	static void load_emotes(State &state, const std::string &broadcaster_id);
	static void set_room_id(State &state, const std::string &id);
	static std::string room_id(State &state);
	static void register_twitch_emotes(State &state, const ChatMessage &msg);
	static std::string helix_get(State &state, const std::string &path);
	static void push(State &state, ChatMessage msg);
	static void set_status(State &state, const std::string &text);
	static bool sleep_interruptible(State &state, int milliseconds);

	std::shared_ptr<State> state_;
	std::thread thread_;
};

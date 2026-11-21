#pragma once

#include "chat.hpp"
#include "httpd.hpp"

#include <cstdint>
#include <deque>
#include <string>

struct obs_source;
class ImageCache;

// Runs a StreamElements-style widget (HTML + CSS + JS + fields + data) inside an
// embedded OBS browser source. The plugin serves the assembled page from a local
// HTTP server and pushes chat events in the StreamElements format, so widgets
// written for StreamElements work without changes.
class WidgetHost {
public:
	WidgetHost() = default;
	~WidgetHost();

	WidgetHost(const WidgetHost &) = delete;
	WidgetHost &operator=(const WidgetHost &) = delete;

	bool start(obs_source *parent, const std::string &dir, EmoteRegistry *emotes,
		   ImageCache *images, uint32_t width, uint32_t height, std::string *error);
	void stop();
	bool active() const { return source_ != nullptr; }
	// True once the page has connected to the local server (CEF is up).
	bool ready() const;
	obs_source *source() const { return source_; }

	void set_size(uint32_t width, uint32_t height);
	// Rebuilds the page when the widget files changed on disk.
	void poll();
	// Tracks watchdog timing; the browser source itself is driven by OBS because
	// it is registered as an active child of our source.
	void tick(float seconds);
	// True when the page never connected to the local server (black screen).
	bool failed() const;
	void feed(const ChatMessage &msg, const std::string &channel, const Config &cfg);
	void render();
	int viewers() const;

private:
	bool rebuild(std::string *error);
	void apply_size();

	HttpServer server_;
	obs_source *source_ = nullptr;
	obs_source *parent_ = nullptr;
	EmoteRegistry *emotes_ = nullptr;
	ImageCache *images_ = nullptr;
	std::string dir_;
	std::string url_;
	uint64_t stamp_ = 0;
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	int last_viewers_ = -1;
	double next_poll_ = 0.0;
	double started_at_ = 0.0;
	bool warned_ = false;
	std::deque<std::string> backlog_; // events sent before the page connected
};

#pragma once

#include "chat.hpp"
#include "httpd.hpp"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <utility>
#include <vector>

struct obs_source;
class ImageCache;

// One editable parameter declared by the widget in fields.txt.
struct WidgetFieldInfo {
	std::string name;
	std::string label;
	std::string type;  // colorpicker, checkbox, dropdown, text, number
	std::string value; // default from fields.txt / data.txt
	std::vector<std::pair<std::string, std::string>> options; // dropdown: key -> label
};

std::vector<WidgetFieldInfo> widget_fields(const std::string &dir);

// Crash-loop protection: a marker file is written while a browser source exists
// and removed on a clean shutdown. If OBS is killed while the widget is active,
// the marker survives and the next start skips the embedded browser.
bool widget_crash_suspected();
void widget_mark_active();
void widget_mark_clean();

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
		   ImageCache *images, const std::map<std::string, std::string> &overrides,
		   uint32_t width, uint32_t height, int fps, std::string *error);
	void stop();
	// Field values edited in the OBS properties dialog.
	void set_overrides(const std::map<std::string, std::string> &overrides);
	// Browser frame rate; lowering it is the cheapest way to cut CPU usage.
	void set_fps(int fps);
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
	uint64_t generation_ = 0; // bumped on every rebuild to force a page reload
	uint32_t width_ = 0;
	uint32_t height_ = 0;
	int fps_ = 15;
	int last_viewers_ = -1;
	double next_poll_ = 0.0;
	double started_at_ = 0.0;
	bool warned_ = false;
	std::deque<std::string> backlog_; // events sent before the page connected
	std::map<std::string, std::string> overrides_; // field values edited in OBS
	std::string css_; // colour overrides injected through the browser source
};

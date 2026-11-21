#include "widget.hpp"

#include "images.hpp"
#include "json.hpp"

#include <obs-module.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

std::string read_file(const std::filesystem::path &path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return std::string();
	std::ostringstream buffer;
	buffer << file.rdbuf();
	return buffer.str();
}

std::string read_first(const std::string &dir, const std::vector<const char *> &names,
		       std::filesystem::path *used = nullptr)
{
	for (const char *name : names) {
		const std::filesystem::path path = std::filesystem::path(dir) / name;
		std::error_code error;
		if (!std::filesystem::exists(path, error))
			continue;
		if (used)
			*used = path;
		return read_file(path);
	}
	return std::string();
}

uint64_t directory_stamp(const std::string &dir)
{
	static const char *kNames[] = {"html.txt", "css.txt", "js.txt", "fields.txt", "data.txt",
				       "index.html", "style.css", "widget.js", "fields.json", "data.json"};
	uint64_t hash = 1469598103934665603ull;
	for (const char *name : kNames) {
		const std::filesystem::path path = std::filesystem::path(dir) / name;
		std::error_code error;
		if (!std::filesystem::exists(path, error))
			continue;
		const auto size = std::filesystem::file_size(path, error);
		const auto time = std::filesystem::last_write_time(path, error);
		const uint64_t time_value = (uint64_t)time.time_since_epoch().count();
		hash = (hash ^ (uint64_t)size) * 1099511628211ull;
		hash = (hash ^ time_value) * 1099511628211ull;
	}
	return hash;
}

std::string json_escape(const std::string &text)
{
	std::string out;
	out.reserve(text.size() + 16);
	for (const unsigned char c : text) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char buffer[8];
				snprintf(buffer, sizeof(buffer), "\\u%04x", c);
				out += buffer;
			} else {
				out.push_back((char)c);
			}
			break;
		}
	}
	return out;
}

// Extracts the substring between two inclusive Unicode code point indexes.
std::string slice_by_cp(const std::string &text, uint32_t start_cp, uint32_t end_cp)
{
	uint32_t cp = 0;
	size_t begin = std::string::npos;
	size_t end = std::string::npos;
	for (size_t i = 0; i < text.size();) {
		if (cp == start_cp)
			begin = i;
		const unsigned char c = (unsigned char)text[i];
		size_t step = 1;
		if ((c & 0xE0) == 0xC0)
			step = 2;
		else if ((c & 0xF0) == 0xE0)
			step = 3;
		else if ((c & 0xF8) == 0xF0)
			step = 4;
		if (cp == end_cp) {
			end = i + step;
			break;
		}
		i += step;
		++cp;
	}
	if (begin == std::string::npos)
		return std::string();
	if (end == std::string::npos)
		end = text.size();
	return text.substr(begin, end - begin);
}

uint64_t now_ms()
{
	return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
		       std::chrono::system_clock::now().time_since_epoch())
		.count();
}

std::string twitch_emote_url(const std::string &id, const char *size)
{
	return "https://static-cdn.jtvnw.net/emoticons/v2/" + id + "/default/dark/" + size;
}

std::string bttv_emote_url(const std::string &id, int size)
{
	return "https://cdn.betterttv.net/emote/" + id + "/" + std::to_string(size) + "x";
}

std::string seventv_emote_url(const std::string &id, int size)
{
	return "https://cdn.7tv.app/emote/" + id + "/" + std::to_string(size) + "x.png";
}

// Re-emits the "value" members of a StreamElements fields definition so that the
// widget receives them as fieldData.
std::string field_values(const std::string &fields_json)
{
	json::Value root;
	if (fields_json.empty() || !json::parse(fields_json, root) || !root.is_object())
		return "{}";

	std::string out = "{";
	bool first = true;
	for (const auto &field : root.object) {
		const json::Value *value = field.second.find("value");
		if (!value)
			continue;
		if (!first)
			out += ",";
		first = false;
		out += "\"" + json_escape(field.first) + "\":";
		switch (value->type) {
		case json::Value::Type::String:
			out += "\"" + json_escape(value->string) + "\"";
			break;
		case json::Value::Type::Number:
			out += std::to_string(value->number);
			break;
		case json::Value::Type::Bool:
			out += value->boolean ? "true" : "false";
			break;
		default:
			out += "null";
			break;
		}
	}
	out += "}";
	return out;
}

std::string build_message_event(const ChatMessage &msg, const std::string &channel,
				EmoteRegistry *registry, ImageCache *images)
{
	std::string emotes = "[";
	bool first = true;

	const auto push_emote = [&emotes, &first](const std::string &name, const std::string &id,
						 const char *type, const std::string &u1,
						 const std::string &u2, const std::string &u3) {
		if (!first)
			emotes += ",";
		first = false;
		emotes += "{\"name\":\"" + json_escape(name) + "\",\"id\":\"" + json_escape(id) +
			  "\",\"type\":\"" + type + "\",\"gif\":false,\"urls\":{\"1\":\"" + u1 +
			  "\",\"2\":\"" + u2 + "\",\"4\":\"" + u3 + "\"}}";
	};

	for (const EmoteSpan &span : msg.emotes) {
		const std::string name = slice_by_cp(msg.text, span.start_cp, span.end_cp);
		if (name.empty())
			continue;
		push_emote(name, span.id, "twitch", twitch_emote_url(span.id, "1.0"),
			   twitch_emote_url(span.id, "2.0"), twitch_emote_url(span.id, "3.0"));
	}

	// Third-party emotes (BTTV / 7TV) are not announced by Twitch, so the words
	// are matched against the shared emote registry.
	if (registry) {
		size_t pos = 0;
		while (pos < msg.text.size()) {
			size_t end = msg.text.find(' ', pos);
			if (end == std::string::npos)
				end = msg.text.size();
			const std::string word = msg.text.substr(pos, end - pos);
			pos = end + 1;
			if (word.size() < 2)
				continue;

			std::string key;
			if (!registry->lookup(word, key) || key.rfind("tw:", 0) == 0)
				continue;

			if (key.rfind("bttv:", 0) == 0) {
				const std::string id = key.substr(5);
				push_emote(word, id, "bttv", bttv_emote_url(id, 1),
					   bttv_emote_url(id, 2), bttv_emote_url(id, 3));
			} else if (key.rfind("7tv:", 0) == 0) {
				const std::string id = key.substr(4);
				push_emote(word, id, "7tv", seventv_emote_url(id, 1),
					   seventv_emote_url(id, 2), seventv_emote_url(id, 3));
			}
		}
	}
	emotes += "]";

	std::string badges = "{";
	std::string badge_urls = "{";
	for (size_t i = 0; i < msg.badges.size(); ++i) {
		if (i) {
			badges += ",";
		}
		badges += "\"" + json_escape(msg.badges[i].set_id) + "\":\"" +
			  json_escape(msg.badges[i].version) + "\"";

		std::string url;
		const std::string key =
			"badge:" + msg.badges[i].set_id + "/" + msg.badges[i].version;
		if (images && images->url_for(key, url)) {
			if (i)
				badge_urls += ",";
			badge_urls += "\"" + json_escape(msg.badges[i].set_id) + "/" +
				      json_escape(msg.badges[i].version) + "\":\"" + json_escape(url) +
				      "\"";
		}
	}
	badges += "}";
	badge_urls += "}";

	std::string event = "{\"listener\":\"message\",\"event\":{\"service\":\"twitch\",\"data\":{";
	event += "\"time\":" + std::to_string(now_ms());
	event += ",\"nick\":\"" + json_escape(msg.user) + "\"";
	event += ",\"displayName\":\"" + json_escape(msg.display.empty() ? msg.user : msg.display) + "\"";
	event += ",\"displayColor\":\"" + json_escape(msg.color) + "\"";
	event += ",\"channel\":\"" + json_escape(channel) + "\"";
	event += ",\"text\":\"" + json_escape(msg.text) + "\"";
	event += ",\"isAction\":" + std::string(msg.action ? "true" : "false");
	event += ",\"msgId\":\"\"";
	event += ",\"userId\":\"\"";
	event += ",\"badges\":" + badges;
	event += ",\"badgeUrls\":" + badge_urls;
	event += ",\"emotes\":" + emotes;
	event += ",\"tags\":{}";
	event += "}}}";
	return event;
}

std::string build_page(const std::string &html, const std::string &css, const std::string &js,
		       const std::string &fields_json, const std::string &data_json)
{
	std::string page;
	page.reserve(html.size() + css.size() + js.size() + 4096);

	page += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
	page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
	page += "<style>html,body{margin:0;padding:0;background:transparent;overflow:hidden;}"
		"</style>";
	page += "<script src=\"https://code.jquery.com/jquery-3.7.1.min.js\"></script>";
	page += "<link rel=\"stylesheet\" "
		"href=\"https://cdnjs.cloudflare.com/ajax/libs/animate.css/4.1.1/animate.min.css\">";
	page += "<style>";
	page += css;
	page += "</style></head><body>";
	page += html;
	page += "<script>";
	// Report page errors and console output to the plugin log: the embedded
	// browser has no visible devtools, and this is the only way to diagnose
	// why a third-party widget misbehaves.
	page += "(function(){var send=function(m){try{fetch('/log?m='+encodeURIComponent(String(m)"
		".slice(0,400)));}catch(e){}};"
		"['log','warn','error'].forEach(function(k){var o=console[k];console[k]=function(){"
		"try{o.apply(console,arguments);}catch(e){}"
		"send(k+': '+Array.prototype.map.call(arguments,function(a){try{return typeof a==='string'?"
		"a:JSON.stringify(a);}catch(e){return String(a);}}).join(' '));};});"
		"window.addEventListener('error',function(e){send('error: '+e.message+' @'+(e.filename||'')+':'+"
		"(e.lineno||0));});"
		"window.addEventListener('unhandledrejection',function(e){send('rejection: '+((e.reason&&"
		"e.reason.message)||e.reason));});})();";
	page += "</script><script>";
	// Minimal StreamElements runtime stubs so widgets do not throw on load.
	page += "window.SE_API=window.SE_API||{store:{get:function(){return Promise.resolve(null);},"
		"set:function(){return Promise.resolve();}},counters:{get:function(){return "
		"Promise.resolve(0);},set:function(){return Promise.resolve();}},sanitize:function(h){return "
		"h;},resumeQueue:function(){},setField:function(){},console:console};";
	page += "</script><script>";
	page += js;
	page += "</script><script>(function(){var FIELDS=";
	page += fields_json.empty() ? std::string("{}") : fields_json;
	page += ";var DATA=";
	page += data_json.empty() ? std::string("{}") : data_json;
	page += ";function fire(){try{window.dispatchEvent(new CustomEvent('onWidgetLoad',{detail:{"
		"fieldData:FIELDS,data:DATA}}));}catch(e){}}"
		"if(document.readyState==='complete'){setTimeout(fire,150);}"
		"else{window.addEventListener('load',function(){setTimeout(fire,150);});}"
		"try{var es=new EventSource('/events');es.onmessage=function(e){try{var p=JSON.parse(e.data);"
		"window.dispatchEvent(new CustomEvent('onEventReceived',{detail:p}));}catch(err){}};"
		"}catch(err){}})();</script>";
	page += "</body></html>";
	return page;
}

} // namespace

WidgetHost::~WidgetHost()
{
	stop();
}

bool WidgetHost::start(obs_source *parent, const std::string &dir, EmoteRegistry *emotes,
		       ImageCache *images, uint32_t width, uint32_t height, std::string *error)
{
	stop();

	parent_ = parent;
	emotes_ = emotes;
	images_ = images;
	dir_ = dir;
	width_ = width;
	height_ = height;

	if (dir_.empty()) {
		if (error)
			*error = "widget folder is not configured";
		return false;
	}

	std::string server_error;
	server_.set_logger([](const std::string &message) {
		blog(LOG_WARNING, "[chat-overlay] widget console: %s", message.c_str());
	});
	if (!server_.start(&server_error)) {
		if (error)
			*error = "local server: " + server_error;
		return false;
	}

	if (!rebuild(error)) {
		server_.stop();
		return false;
	}

	apply_size();
	if (!source_) {
		if (error)
			*error = "browser_source is unavailable (obs-browser is not loaded?)";
		server_.stop();
		return false;
	}

	blog(LOG_INFO, "[chat-overlay] widget hosted at %s (viewers: %d)", url_.c_str(),
	     (int)server_.client_count());
	return true;
}

void WidgetHost::stop()
{
	if (source_) {
		if (parent_)
			obs_source_remove_active_child(parent_, source_);
		obs_source_dec_showing(source_);
		obs_source_release(source_);
		source_ = nullptr;
	}
	parent_ = nullptr;
	server_.stop();
	last_viewers_ = -1;
}

void WidgetHost::set_size(uint32_t width, uint32_t height)
{
	if (width_ == width && height_ == height)
		return;
	width_ = width;
	height_ = height;
	apply_size();
}

void WidgetHost::apply_size()
{
	if (!source_ && url_.empty())
		return;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "url", url_.c_str());
	obs_data_set_bool(settings, "is_local_file", false);
	obs_data_set_int(settings, "width", (long long)width_);
	obs_data_set_int(settings, "height", (long long)height_);
	obs_data_set_int(settings, "fps", 30);
	obs_data_set_bool(settings, "restart_when_active", true);

	if (!source_) {
		source_ = obs_source_create("browser_source", "chat-overlay-widget", settings, nullptr);
		if (source_) {
			obs_source_inc_showing(source_);
			// Registers the browser as an active child of our source: OBS then
			// activates it together with the parent, which is what makes
			// obs-browser create and tick its CEF browser.
			if (parent_)
				obs_source_add_active_child(parent_, source_);
		}
	} else {
		obs_source_update(source_, settings);
	}
	obs_data_release(settings);

	if (source_)
		blog(LOG_INFO, "[chat-overlay] browser source ready: id=%s size=%ux%u",
		     obs_source_get_id(source_), width_, height_);
}

bool WidgetHost::rebuild(std::string *error)
{
	const std::string html = read_first(dir_, {"html.txt", "index.html", "widget.html"});
	const std::string css = read_first(dir_, {"css.txt", "style.css", "widget.css"});
	const std::string js = read_first(dir_, {"js.txt", "widget.js", "script.js"});
	const std::string fields = read_first(dir_, {"fields.txt", "fields.json"});
	const std::string data = read_first(dir_, {"data.txt", "data.json"});

	if (html.empty() && css.empty() && js.empty()) {
		if (error)
			*error = "no html/css/js files found in " + dir_;
		return false;
	}

	const std::string page = build_page(html, css, js, field_values(fields), data.empty() ? "{}" : data);
	server_.set_page(page);
	stamp_ = directory_stamp(dir_);
	url_ = "http://127.0.0.1:" + std::to_string(server_.port()) + "/?v=" +
	       std::to_string(stamp_);
	apply_size();

	blog(LOG_INFO, "[chat-overlay] widget page rebuilt (%zu bytes, html=%zu css=%zu js=%zu)",
	     page.size(), html.size(), css.size(), js.size());
	return true;
}

void WidgetHost::tick(float seconds)
{
	if (!source_)
		return;

	(void)seconds;
	const double now = monotonic_seconds();
	if (started_at_ == 0.0)
		started_at_ = now;

	if (!warned_ && server_.client_count() == 0 && now - started_at_ > 8.0) {
		warned_ = true;
		blog(LOG_WARNING,
		     "[chat-overlay] widget page did not connect to %s: the embedded browser "
		     "did not open the page (obs-browser/CEF issue?)",
		     url_.c_str());
	}
}

bool WidgetHost::failed() const
{
	return warned_ && server_.client_count() == 0;
}

void WidgetHost::poll()
{
	if (!source_)
		return;

	const double now = monotonic_seconds();
	if (now < next_poll_)
		return;
	next_poll_ = now + 2.0;

	const uint64_t stamp = directory_stamp(dir_);
	if (stamp != stamp_ && stamp != 0) {
		blog(LOG_INFO, "[chat-overlay] widget files changed, reloading");
		rebuild(nullptr);
	}

	const int viewers = (int)server_.client_count();
	if (viewers != last_viewers_) {
		last_viewers_ = viewers;
		blog(LOG_INFO, "[chat-overlay] widget viewers: %d", viewers);
	}
}

void WidgetHost::feed(const ChatMessage &msg, const std::string &channel, const Config &cfg)
{
	if (!source_)
		return;
	(void)cfg;
	server_.broadcast(build_message_event(msg, channel, emotes_, images_));
}

void WidgetHost::render()
{
	if (source_)
		obs_source_video_render(source_);
}

int WidgetHost::viewers() const
{
	return (int)server_.client_count();
}

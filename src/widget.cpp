#include "widget.hpp"

#include "images.hpp"
#include "json.hpp"

#include <obs-module.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
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

// StreamElements replaces {fieldName} placeholders inside HTML, CSS and JS with
// the current field values. Without this step colours such as
// "color: {textmesscol};" are invalid CSS and the widget loses its design.
struct FieldValues {
	std::map<std::string, std::string> values;
	std::string json;
};

std::string value_to_string(const json::Value &value)
{
	switch (value.type) {
	case json::Value::Type::String:
		return value.string;
	case json::Value::Type::Number: {
		char buffer[64];
		snprintf(buffer, sizeof(buffer), "%g", value.number);
		return buffer;
	}
	case json::Value::Type::Bool:
		return value.boolean ? "true" : "false";
	default:
		return std::string();
	}
}

FieldValues collect_fields(const std::string &fields_json, const std::string &data_json,
			   const std::map<std::string, std::string> &overrides)
{
	FieldValues result;

	// data.txt holds the values saved by the StreamElements editor, and they are
	// exactly what the widget's CSS placeholders refer to.
	json::Value data;
	if (!data_json.empty() && json::parse(data_json, data) && data.is_object()) {
		for (const auto &item : data.object)
			result.values[item.first] = value_to_string(item.second);
	}

	json::Value fields;
	if (!fields_json.empty() && json::parse(fields_json, fields) && fields.is_object()) {
		for (const auto &field : fields.object) {
			const json::Value *type_value = field.second.find("type");
			const std::string type = type_value ? type_value->string : std::string();

			if (const json::Value *value = field.second.find("value")) {
				result.values[field.first] = value_to_string(*value);
				continue;
			}
			if (type == "button" || type == "hidden")
				continue;
			if (result.values.count(field.first))
				continue;

			if (type == "colorpicker") {
				result.values[field.first] = "#ffffff";
			} else if (type == "checkbox") {
				result.values[field.first] = "false";
			} else if (type == "dropdown") {
				const json::Value *options = field.second.find("options");
				if (options && options->is_object() && !options->object.empty())
					result.values[field.first] = options->object.front().first;
			} else if (type == "number") {
				result.values[field.first] = "0";
			} else if (type == "text") {
				result.values[field.first] = "";
			}
		}
	}

	// Values edited in the OBS properties dialog win over everything from files.
	for (const auto &item : overrides)
		result.values[item.first] = item.second;

	// Fallbacks for the placeholders this widget uses. Without them an unresolved
	// "{emoteSize}" would survive into the CSS/JS and break that declaration.
	static const struct {
		const char *key;
		const char *value;
	} kDefaults[] = {{"emoteSize", "51"},    {"namecol", "#ffffff"},
			 {"textmesscol", "#ffffff"}, {"backgroundcol", "rgba(0, 0, 0, 0.15)"},
			 {"shadownamecol", "#ffffff"}, {"shadowtextcol", "#ffffff"},
			 {"shadowelemcol", "#ffffff"}, {"flowercol", "#ffffff"},
			 {"dividercol", "#ffffff"}};
	for (const auto &entry : kDefaults) {
		if (!result.values.count(entry.key))
			result.values[entry.key] = entry.value;
	}

	result.json = "{";
	bool first = true;
	for (const auto &item : result.values) {
		if (!first)
			result.json += ",";
		first = false;
		result.json += "\"" + json_escape(item.first) + "\":";
		// Numbers and booleans stay unquoted so the widget receives real types.
		const std::string &value = item.second;
		const bool numeric = !value.empty() &&
				     value.find_first_not_of("0123456789.-") == std::string::npos;
		if (numeric || value == "true" || value == "false")
			result.json += value;
		else
			result.json += "\"" + json_escape(value) + "\"";
	}
	result.json += "}";
	return result;
}

std::string substitute_fields(const std::string &text, const std::map<std::string, std::string> &values)
{
	std::string out;
	out.reserve(text.size());

	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] != '{') {
			out.push_back(text[i]);
			continue;
		}
		const size_t close = text.find('}', i + 1);
		if (close == std::string::npos) {
			out.push_back(text[i]);
			continue;
		}
		const std::string key = text.substr(i + 1, close - i - 1);
		const auto it = values.find(key);
		if (it == values.end()) {
			out.append(text, i, close - i + 1);
			i = close;
			continue;
		}
		out += it->second;
		i = close;
	}
	return out;
}

std::string build_message_event(const ChatMessage &msg, const std::string &channel,
				EmoteRegistry *registry, ImageCache *images, const Config &cfg)
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
			  "\",\"type\":\"" + type + "\",\"gif\":false,\"urls\":[\"" + u1 + "\",\"" +
			  u2 + "\",\"" + u3 + "\"]}";
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

	// StreamElements sends "badges" as an array of objects; the widget calls
	// data.badges.push() when the provider is "twitch", so an object would throw.
	std::string badges = "[";
	std::string badge_urls = "{";
	bool first_badge = true;
	for (size_t i = 0; i < msg.badges.size(); ++i) {
		std::string url;
		const std::string key =
			"badge:" + msg.badges[i].set_id + "/" + msg.badges[i].version;
		if (images)
			images->url_for(key, url);

		if (!first_badge)
			badges += ",";
		first_badge = false;
		badges += "{\"type\":\"" + json_escape(msg.badges[i].set_id) + "\",\"version\":\"" +
			  json_escape(msg.badges[i].version) + "\",\"url\":\"" + json_escape(url) +
			  "\"}";

		if (!url.empty()) {
			if (!badge_urls.empty() && badge_urls.size() > 1)
				badge_urls += ",";
			badge_urls += "\"" + json_escape(msg.badges[i].set_id) + "/" +
				      json_escape(msg.badges[i].version) + "\":\"" + json_escape(url) +
				      "\"";
		}
	}
	badges += "]";
	badge_urls += "}";

	std::string event = "{\"listener\":\"message\",\"event\":{\"service\":\"twitch\",\"data\":{";
	event += "\"time\":" + std::to_string(now_ms());
	event += ",\"nick\":\"" + json_escape(msg.user) + "\"";
	event += ",\"displayName\":\"" + json_escape(msg.display.empty() ? msg.user : msg.display) + "\"";
	event += ",\"displayColor\":\"" + json_escape(cfg.twitch_nick_colors ? msg.color : "") +
		 "\"";
	event += ",\"channel\":\"" + json_escape(channel) + "\"";
	// Full IRC tag set: the widget reads tags.id, tags.color, tags['tmi-sent-ts']
	// and throws "Cannot read properties of undefined (reading 'id')" if it is
	// missing — which aborted its initialisation and broke message limits.
	std::string tags = "{";
	bool first_tag = true;
	for (const auto &tag : msg.tags) {
		if (!first_tag)
			tags += ",";
		first_tag = false;
		tags += "\"" + json_escape(tag.first) + "\":\"" + json_escape(tag.second) + "\"";
	}
	tags += "}";

	const auto tag_value = [&msg](const char *name) -> std::string {
		const auto it = msg.tags.find(name);
		return it == msg.tags.end() ? std::string() : it->second;
	};

	event += ",\"text\":\"" + json_escape(msg.text) + "\"";
	event += ",\"isAction\":" + std::string(msg.action ? "true" : "false");
	event += ",\"msgId\":\"" + json_escape(tag_value("id")) + "\"";
	event += ",\"userId\":\"" + json_escape(tag_value("user-id")) + "\"";
	event += ",\"badges\":" + badges;
	event += ",\"badgeUrls\":" + badge_urls;
	event += ",\"emotes\":" + emotes;
	event += ",\"tags\":" + tags;
	event += "}}}";
	return event;
}

std::string build_page(const std::string &html, const std::string &css, const std::string &js,
		       const std::string &fields_raw, const std::string &data_raw,
		       const std::map<std::string, std::string> &overrides)
{
	// StreamElements substitutes {field} placeholders in HTML/CSS/JS before the
	// widget runs; the same has to happen here, otherwise colours written as
	// "color: {textmesscol}" would be dropped by the CSS parser.
	const FieldValues fields = collect_fields(fields_raw, data_raw, overrides);
	const std::string page_html = substitute_fields(html, fields.values);
	const std::string page_css = substitute_fields(css, fields.values);
	const std::string page_js = substitute_fields(js, fields.values);

	std::string page;
	page.reserve(page_html.size() + page_css.size() + page_js.size() + 8192);

	page += "<!DOCTYPE html><html><head><meta charset=\"utf-8\">";
	page += "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">";
	page += "<style>html,body{margin:0;padding:0;background:transparent;overflow:hidden;}"
		"</style>";
	page += "<script src=\"https://code.jquery.com/jquery-3.7.1.min.js\"></script>";
	page += "<link rel=\"stylesheet\" "
		"href=\"https://cdnjs.cloudflare.com/ajax/libs/animate.css/4.1.1/animate.min.css\">";
	page += "<style>";
	page += page_css;
	page += "</style>";
	// Local fallback for animate.css: the widget hangs "animate__fadeIn" /
	// "animate__fadeOut" classes on messages, and the CDN may be unreachable.
	page += "<style>"
		".animate__animated{animation-duration:var(--animate-duration,1s);"
		"animation-fill-mode:both;}"
		"@keyframes fadeIn{from{opacity:0}to{opacity:1}}"
		"@keyframes fadeOut{from{opacity:1}to{opacity:0}}"
		"@keyframes fadeInUp{from{opacity:0;transform:translate3d(0,20%,0)}"
		"to{opacity:1;transform:none}}"
		"@keyframes fadeOutDown{from{opacity:1}to{opacity:0;transform:translate3d(0,20%,0)}}"
		"@keyframes zoomIn{from{opacity:0;transform:scale3d(.7,.7,.7)}"
		"to{opacity:1;transform:scale3d(1,1,1)}}"
		"@keyframes bounceIn{from{opacity:0;transform:scale3d(.9,.9,.9)}"
		"60%{opacity:1;transform:scale3d(1.05,1.05,1.05)}to{transform:none}}"
		".animate__fadeIn{animation-name:fadeIn}"
		".animate__fadeOut{animation-name:fadeOut}"
		".animate__fadeInUp{animation-name:fadeInUp}"
		".animate__fadeOutDown{animation-name:fadeOutDown}"
		".animate__zoomIn{animation-name:zoomIn}"
		".animate__bounceIn{animation-name:bounceIn}"
		"</style></head><body>";
	page += page_html;
	page += "<script>";
	// Report page errors and console output to the plugin log: the embedded
	// browser has no visible devtools, and this is the only way to diagnose
	// why a third-party widget misbehaves.
	page += "(function(){var send=function(m){try{fetch('/log?m='+encodeURIComponent(String(m)"
		".slice(0,1200)));}catch(e){}};"
		// Later <script> blocks need the logger too (payload diagnostics).
		"window.__chatLog=send;"
		"['log','warn','error'].forEach(function(k){var o=console[k];console[k]=function(){"
		"try{o.apply(console,arguments);}catch(e){}"
		"send(k+': '+Array.prototype.map.call(arguments,function(a){try{return typeof a==='string'?"
		"a:JSON.stringify(a);}catch(e){return String(a);}}).join(' '));};});"
		"window.addEventListener('error',function(e){send('error: '+e.message+' @'+(e.filename||'')+':'+"
		"(e.lineno||0)+(e.error&&e.error.stack?' stack: '+String(e.error.stack).slice(0,600):''));});"
		"window.addEventListener('unhandledrejection',function(e){send('rejection: '+((e.reason&&"
		"e.reason.message)||e.reason));});"
		// Log the network requests the widget makes, but never the log endpoint
		// itself: send() uses fetch('/log'), so intercepting it would recurse
		// (each logged line produced another fetch and flooded the connection).
		"try{var of=window.fetch;if(of){var nf=0;window.fetch=function(u,o){var url=("
		"typeof u==='string')?u:((u&&u.url)||'');"
		// The widget asks StreamElements for the emote provider
		// (fetch('…/kappa/v2/channels/'+data.id+'/').then(j=>provider=j.provider)).
		// Without that answer emotes take a slow fallback path, so answer locally.
		"if(url.indexOf('api.streamelements.com')>=0){"
		"return Promise.resolve(new Response(JSON.stringify({provider:'twitch'}),"
		"{status:200,headers:{'Content-Type':'application/json'}}));}"
		"if(url.indexOf('/log')>=0||url.indexOf('/events')>=0)return of.call(this,u,o);"
		"try{if(nf<40){nf++;send('fetch: '+url);}}catch(e){}return of.call(this,u,o);};}}catch(e){}"
		"try{var xo=XMLHttpRequest.prototype.open;var nx=0;"
		"XMLHttpRequest.prototype.open=function(m,u){var url=String(u||'');"
		"if(url.indexOf('/log')<0&&url.indexOf('/events')<0){"
		"try{if(nx<40){nx++;send('xhr: '+m+' '+url);}}catch(e){}}"
		"return xo.apply(this,arguments);};}catch(e){}"
		"})();";
	page += "</script><script>";
	// Minimal StreamElements runtime stubs so widgets do not throw on load.
	page += "window.SE_API=window.SE_API||{store:{get:function(){return Promise.resolve(null);},"
		"set:function(){return Promise.resolve();}},counters:{get:function(){return "
		"Promise.resolve(0);},set:function(){return Promise.resolve();}},sanitize:function(h){return "
		"h;},resumeQueue:function(){},setField:function(){},console:console};";
	page += "</script><script>";
	page += page_js;
	page += "</script><script>(function(){var FIELDS=";
	page += fields.json.empty() ? std::string("{}") : fields.json;
	page += ";var DATA=";
	page += data_raw.empty() ? std::string("{}") : data_raw;
	// StreamElements session data always carries "id" (used to resolve the emote
	// provider) and "channel"; the widget reads e.detail.data.id, so a missing id
	// throws "Cannot read properties of undefined (reading 'id')".
	page += ";if(!DATA.id)DATA.id='0';";
	page += ";if(!DATA.channel)DATA.channel={id:DATA.id,username:'channel'};";
	page += ";function fire(){try{window.dispatchEvent(new CustomEvent('onWidgetLoad',{detail:{"
		"fieldData:FIELDS,data:DATA}}));}catch(e){}}"
		"if(document.readyState==='complete'){setTimeout(fire,150);}"
		"else{window.addEventListener('load',function(){setTimeout(fire,150);});}"
		"try{var es=new EventSource('/events');es.onmessage=function(e){try{var p=JSON.parse(e.data);"
		// Diagnostics: show what the page actually receives, the widget needs
		// emotes[i].urls[2] and silently draws nothing when it is missing.
		"try{if(window.__emoteLogs===undefined)window.__emoteLogs=0;"
		"if(window.__emoteLogs<5){var dd=p&&p.event&&p.event.data;var ee=dd&&dd.emotes;"
		"if(ee&&ee.length){window.__emoteLogs++;if(window.__chatLog)window.__chatLog("
		"'payload emotes: '+ee.length+' '+JSON.stringify(ee[0]).slice(0,200));}}}catch(x){}"
		"window.dispatchEvent(new CustomEvent('onEventReceived',{detail:p}));}catch(err){}};"
		"}catch(err){}})();</script>";
	page += "</body></html>";
	return page;
}

// CSS patched into the browser source through its "css" setting. Applying field
// edits this way avoids reloading (and re-creating) the CEF browser, which used
// to crash on rapid property changes.
std::string build_css_override(const FieldValues &values)
{
	const auto get = [&values](const char *key) {
		const auto it = values.values.find(key);
		return it != values.values.end() ? it->second : std::string();
	};

	std::string css;
	css += ".name{color:" + get("namecol") + " !important;text-shadow:0 0 9.9px " +
	       get("shadownamecol") + " !important;}";
	css += ".message_text{color:" + get("textmesscol") +
	       " !important;text-shadow:0 0 4.4px " + get("shadowtextcol") + " !important;}";
	css += ".message{background:" + get("backgroundcol") + " !important;}";
	css += ".flower path:nth-of-type(1){fill:" + get("flowercol") + " !important;}";
	css += ".divider path:nth-of-type(1){fill:" + get("dividercol") + " !important;}";
	css += ".flower,.divider,.divider_container,.cat,.eyes,.heart,.tilda{filter:drop-shadow(0 0 "
	       "9.9px " +
	       get("shadowelemcol") + ") !important;}";
	return css;
}

} // namespace

namespace {

std::string widget_lock_path()
{
	const char *temp = std::getenv("TEMP");
	const std::string base = (temp && *temp) ? temp : ".";
	return base + "\\obs-chat-overlay-widget.lock";
}

} // namespace

bool widget_crash_suspected()
{
	std::FILE *file = std::fopen(widget_lock_path().c_str(), "rb");
	if (!file)
		return false;
	std::fclose(file);
	return true;
}

void widget_mark_active()
{
	std::FILE *file = std::fopen(widget_lock_path().c_str(), "wb");
	if (!file)
		return;
	std::fputs("active", file);
	std::fclose(file);
}

void widget_mark_clean()
{
	std::remove(widget_lock_path().c_str());
}

std::vector<WidgetFieldInfo> widget_fields(const std::string &dir)
{	std::vector<WidgetFieldInfo> result;
	if (dir.empty())
		return result;

	const std::string fields_raw = read_first(dir, {"fields.txt", "fields.json"});
	if (fields_raw.empty())
		return result;

	json::Value fields;
	if (!json::parse(fields_raw, fields) || !fields.is_object())
		return result;

	const std::string data_raw = read_first(dir, {"data.txt", "data.json"});
	json::Value data;
	const bool has_data =
		!data_raw.empty() && json::parse(data_raw, data) && data.is_object();

	for (const auto &field : fields.object) {
		const json::Value *type_value = field.second.find("type");
		const std::string type =
			(type_value && type_value->type == json::Value::Type::String)
				? type_value->string
				: std::string();
		if (type.empty() || type == "button" || type == "hidden")
			continue;

		WidgetFieldInfo info;
		info.name = field.first;
		info.type = type;
		info.label = field.second.get_string("label");
		if (info.label.empty())
			info.label = field.first;

		if (const json::Value *value = field.second.find("value"))
			info.value = value_to_string(*value);
		else if (has_data) {
			if (const json::Value *stored = data.find(field.first))
				info.value = value_to_string(*stored);
		}

		if (const json::Value *options = field.second.find("options");
		    options && options->is_object()) {
			for (const auto &option : options->object) {
				const std::string label =
					option.second.type == json::Value::Type::String
						? option.second.string
						: option.first;
				info.options.emplace_back(option.first, label);
			}
		}

		result.push_back(std::move(info));
	}

	return result;
}

WidgetHost::~WidgetHost()
{
	stop();
}

bool WidgetHost::start(obs_source *parent, const std::string &dir, EmoteRegistry *emotes,
		       ImageCache *images, const std::map<std::string, std::string> &overrides,
		       uint32_t width, uint32_t height, int fps, std::string *error)
{
	stop();

	parent_ = parent;
	emotes_ = emotes;
	images_ = images;
	overrides_ = overrides;
	dir_ = dir;
	width_ = width;
	height_ = height;
	fps_ = fps < 5 ? 5 : (fps > 60 ? 60 : fps);

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

	// Verify the local server end-to-end before blaming the browser.
	std::string probe;
	if (server_.self_test(&probe))
		blog(LOG_INFO, "[chat-overlay] http self-test ok: %s", url_.c_str());
	else
		blog(LOG_WARNING, "[chat-overlay] http self-test FAILED (%s) for %s", probe.c_str(),
		     url_.c_str());

	blog(LOG_INFO, "[chat-overlay] browser source ready: id=%s size=%ux%u",
	     obs_source_get_id(source_), width_, height_);

	blog(LOG_INFO, "[chat-overlay] widget hosted at %s (viewers: %d)", url_.c_str(),
	     (int)server_.client_count());
	return true;
}

bool WidgetHost::ready() const
{
	return server_.client_count() > 0;
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

void WidgetHost::set_overrides(const std::map<std::string, std::string> &overrides)
{
	if (overrides == overrides_)
		return;
	overrides_ = overrides;
	if (!source_)
		return;

	// Field edits rebuild the page and navigate to a new URL. The browser source
	// itself is kept alive (no destroy/create), which is what used to crash CEF.
	const std::string fields = read_first(dir_, {"fields.txt", "fields.json"});
	const std::string data = read_first(dir_, {"data.txt", "data.json"});
	const FieldValues resolved =
		collect_fields(fields, data.empty() ? "{}" : data, overrides_);
	// Rebuild the page and navigate to a new URL: this guarantees the edited
	// colours are visible (obs-browser may only apply the "css" property on
	// load). A navigation is safe — the browser source itself is not re-created.
	css_ = build_css_override(resolved);
	rebuild(nullptr);
	blog(LOG_INFO, "[chat-overlay] widget fields updated (%zu overrides)", overrides_.size());
}

void WidgetHost::set_fps(int fps)
{
	if (fps < 5)
		fps = 5;
	if (fps > 60)
		fps = 60;
	if (fps == fps_)
		return;
	fps_ = fps;
	if (source_)
		apply_size();
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
	obs_data_set_int(settings, "fps", fps_);
	// Keeping the browser alive when the source is hidden avoids a full reload
	// (and a CPU spike) every time the scene switches.
	obs_data_set_bool(settings, "restart_when_active", false);
	// Colour patches are applied through this property: no page reload, so the
	// CEF browser is never torn down while the user edits fields.
	obs_data_set_string(settings, "css", css_.c_str());

	if (!source_) {
		// A *private* source: obs_source_create() would register the browser in the
		// scene collection, so OBS saved it as an orphan. On the next start it was
		// loaded but never shown, and destroying a browser source that never got a
		// CEF browser crashes libcef (obs_source_destroy_defer -> CefPostTask).
		static int instance = 0;
		const std::string name = "chat-overlay-widget-" + std::to_string(++instance);
		source_ = obs_source_create_private("browser_source", name.c_str(), settings);
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

	const std::string page =
		build_page(html, css, js, fields, data.empty() ? "{}" : data, overrides_);
	const FieldValues resolved =
		collect_fields(fields, data.empty() ? "{}" : data, overrides_);
	css_ = build_css_override(resolved);
	server_.set_page(page);
	stamp_ = directory_stamp(dir_);
	// The URL must change on every rebuild, otherwise obs_source_update() keeps
	// the same URL and the browser never reloads the new page (field edits would
	// be invisible).
	++generation_;
	url_ = "http://127.0.0.1:" + std::to_string(server_.port()) + "/?v=" +
	       std::to_string(generation_) + "_" + std::to_string(stamp_);
	apply_size();

	blog(LOG_INFO, "[chat-overlay] widget page rebuilt (%zu bytes, html=%zu css=%zu js=%zu)",
	     page.size(), html.size(), css.size(), js.size());

	// Log the colour values that actually went into the CSS, so the on-screen
	// result can be compared with the properties dialog.
	static const char *kColorKeys[] = {"namecol", "textmesscol",   "backgroundcol",
					   "shadownamecol", "shadowtextcol", "shadowelemcol",
					   "flowercol", "dividercol"};
	for (const char *key : kColorKeys) {
		const auto it = resolved.values.find(key);
		if (it != resolved.values.end())
			blog(LOG_INFO, "[chat-overlay] widget field %s = %s", key,
			     it->second.c_str());
	}
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

	const double elapsed = now - started_at_;
	// CEFF cold start on slow machines can take tens of seconds: only warn, never
	// tear the widget down (the server must stay alive for the page to load).
	if (!warned_ && server_.client_count() == 0 && elapsed > 30.0) {
		warned_ = true;
		blog(LOG_WARNING,
		     "[chat-overlay] widget page still has not connected to %s after 30 s; "
		     "check whether the URL opens in a normal browser",
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
	if (viewers > 0 && last_viewers_ <= 0) {
		// The page just connected: replay the messages that arrived while CEF was
		// still starting, otherwise the first seconds of chat would be lost.
		for (const std::string &event : backlog_)
			server_.broadcast(event);
		backlog_.clear();
	}
	if (viewers != last_viewers_) {
		last_viewers_ = viewers;
		blog(LOG_INFO, "[chat-overlay] widget viewers: %d", viewers);
	}
}

void WidgetHost::feed(const ChatMessage &msg, const std::string &channel, const Config &cfg)
{
	if (!source_)
		return;

	const std::string event = build_message_event(msg, channel, emotes_, images_, cfg);
	if (!ready()) {
		backlog_.push_back(event);
		if (backlog_.size() > 30)
			backlog_.pop_front();
		return;
	}
	server_.broadcast(event);
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

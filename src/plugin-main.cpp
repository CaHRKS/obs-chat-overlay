#include <obs-module.h>

#include <graphics/vec4.h>

#include "chat.hpp"
#include "images.hpp"
#include "render.hpp"
#include "theme.hpp"
#include "twitch.hpp"
#include "widget.hpp"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-chat-overlay", "en-US")

namespace {

constexpr const char *kSourceId = "obs_chat_overlay";
// Keep in sync with buildspec.json; printed on load so the log always shows which
// build is installed.
constexpr const char *CHAT_OVERLAY_VERSION = "1.3.6";

void chat_overlay_update(void *data, obs_data_t *settings);

Config config_from_settings(obs_data_t *settings)
{
	Config cfg;
	cfg.channel = to_lower_ascii(trim(obs_data_get_string(settings, "channel")));
	if (!cfg.channel.empty() && cfg.channel.front() == '#')
		cfg.channel.erase(cfg.channel.begin());
	cfg.oauth = obs_data_get_string(settings, "oauth");
	cfg.username = obs_data_get_string(settings, "username");
	cfg.client_id = obs_data_get_string(settings, "client_id");
	cfg.use_tls = obs_data_get_bool(settings, "use_tls");
	cfg.proxy = trim(obs_data_get_string(settings, "proxy"));
	cfg.debug_log = obs_data_get_bool(settings, "debug_log");
	cfg.demo_mode = obs_data_get_bool(settings, "demo_mode");
	cfg.widget_mode = obs_data_get_bool(settings, "widget_mode");
	cfg.widget_dir = trim(obs_data_get_string(settings, "widget_dir"));
	cfg.twitch_nick_colors = obs_data_get_bool(settings, "twitch_nick_colors");
	cfg.widget_fps = (int)obs_data_get_int(settings, "widget_fps");
	if (cfg.widget_fps < 5)
		cfg.widget_fps = 5;
	if (cfg.widget_fps > 60)
		cfg.widget_fps = 60;

	cfg.width = (uint32_t)obs_data_get_int(settings, "width");
	cfg.height = (uint32_t)obs_data_get_int(settings, "height");
	if (cfg.width < 320)
		cfg.width = 320;
	if (cfg.height < 240)
		cfg.height = 240;

	cfg.font_face = obs_data_get_string(settings, "font_face");
	cfg.font_size = (float)obs_data_get_int(settings, "font_size");
	if (cfg.font_size < 6.0f)
		cfg.font_size = 6.0f;
	cfg.bold = obs_data_get_bool(settings, "bold");
	cfg.italic = obs_data_get_bool(settings, "italic");

	cfg.text_color = (uint32_t)obs_data_get_int(settings, "text_color");
	cfg.nick_color = (uint32_t)obs_data_get_int(settings, "nick_color");
	cfg.bg_color = (uint32_t)obs_data_get_int(settings, "bg_color");
	cfg.outline_color = (uint32_t)obs_data_get_int(settings, "outline_color");
	cfg.outline_size = (int)obs_data_get_int(settings, "outline_size");
	cfg.bg_padding = (int)obs_data_get_int(settings, "bg_padding");
	cfg.bg_radius = (int)obs_data_get_int(settings, "bg_radius");
	cfg.line_spacing = (int)obs_data_get_int(settings, "line_spacing");
	cfg.emote_scale = (float)obs_data_get_double(settings, "emote_scale");
	cfg.show_badges = obs_data_get_bool(settings, "show_badges");
	cfg.show_nick = obs_data_get_bool(settings, "show_nick");
	cfg.show_timestamps = obs_data_get_bool(settings, "show_timestamps");

	cfg.anchor = (Anchor)(int)obs_data_get_int(settings, "anchor");
	cfg.margin_x = (int)obs_data_get_int(settings, "margin_x");
	cfg.margin_y = (int)obs_data_get_int(settings, "margin_y");
	cfg.max_width = (int)obs_data_get_int(settings, "max_width");
	cfg.text_align = (int)obs_data_get_int(settings, "text_align");

	cfg.max_messages = (int)obs_data_get_int(settings, "max_messages");
	cfg.ttl_seconds = (int)obs_data_get_int(settings, "ttl_seconds");
	cfg.fade_ms = (int)obs_data_get_int(settings, "fade_ms");

	cfg.hide_commands = obs_data_get_bool(settings, "hide_commands");
	cfg.command_prefix = obs_data_get_string(settings, "command_prefix");
	cfg.hide_links = obs_data_get_bool(settings, "hide_links");
	cfg.caps_threshold = (int)obs_data_get_int(settings, "caps_threshold");
	cfg.stopwords = obs_data_get_string(settings, "stopwords");
	cfg.ignore_users = obs_data_get_string(settings, "ignore_users");

	cfg.twitch_emotes = obs_data_get_bool(settings, "twitch_emotes");
	cfg.bttv_emotes = obs_data_get_bool(settings, "bttv_emotes");
	cfg.seventv_emotes = obs_data_get_bool(settings, "seventv_emotes");

	if (cfg.max_messages < 1)
		cfg.max_messages = 1;
	if (cfg.max_messages > 60)
		cfg.max_messages = 60;
	return cfg;
}

struct ChatOverlay {
	obs_source_t *source = nullptr;
	Config cfg;
	std::shared_ptr<EmoteRegistry> emotes;
	std::shared_ptr<ImageCache> images;
	std::unique_ptr<ChatRenderer> renderer;
	std::unique_ptr<TwitchWorker> worker;
	std::unique_ptr<WidgetHost> widget;

	// Guards the renderer and the worker: update() runs on the UI thread while
	// video_tick()/video_render() run on the graphics thread.
	std::mutex mutex;

	gs_texture_t *texture = nullptr;
	uint32_t texture_w = 0;
	uint32_t texture_h = 0;
	int draw_x = 0;
	int draw_y = 0;
	RenderedPanel panel;

	// Theme import bookkeeping (update() only runs on the UI thread).
	std::string applied_theme_file;
	std::string applied_theme_preset;
	size_t applied_css_hash = 0;

	// Demo mode state (graphics thread only).
	size_t demo_index = 0;
	double next_demo_time = 0.0;

	// Widget start is deferred: creating (and especially destroying) a browser
	// source before CEF finished initialising crashes libcef, which is why the
	// embedded browser is only started after OBS has been running for a while.
	bool widget_wanted = false;
	double widget_arm_time = 0.0;
	bool started_once = false;
	bool crash_warned = false;
	std::map<std::string, std::string> pending_overrides;
};

void destroy_texture(ChatOverlay *ctx)
{
	if (ctx->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ctx->texture);
		obs_leave_graphics();
		ctx->texture = nullptr;
	}
	ctx->texture_w = 0;
	ctx->texture_h = 0;
}

// Builds one of the local test messages used by demo mode. The text is ASCII
// where emote ranges are involved, so code point offsets equal character offsets.
ChatMessage make_demo_message(size_t index)
{
	ChatMessage msg;
	msg.user = "testviewer";
	msg.display = "TestViewer";
	msg.color = "#FF7F50";

	const auto add_emote = [&msg](const char *id, uint32_t start, uint32_t end) {
		EmoteSpan span;
		span.id = id;
		span.start_cp = start;
		span.end_cp = end;
		msg.emotes.push_back(span);
	};

	switch (index % 8) {
	case 0:
		msg.text = "Проверка чата: обычное сообщение, ник окрашен в цвет Twitch.";
		break;
	case 1:
		msg.display = "EmoteFan";
		msg.user = "emotefan";
		msg.color = "#1E90FF";
		msg.text = "Kappa PogChamp LUL 4Head BibleThump";
		add_emote("25", 0, 4);        // Kappa
		add_emote("88", 6, 13);       // PogChamp
		add_emote("425618", 15, 17);  // LUL
		add_emote("354", 19, 23);     // 4Head
		add_emote("86", 25, 34);      // BibleThump
		break;
	case 2:
		msg.display = "LongMessage";
		msg.user = "longmessage";
		msg.text = "Длинное сообщение для проверки переноса строк: плагин должен "
			   "разбить его по словам и не выпустить за пределы блока чата даже "
			   "при маленькой ширине панели.";
		break;
	case 3:
		msg.display = "Moderator";
		msg.user = "moderator";
		msg.color = "#00FF7F";
		msg.badges.push_back({"moderator", "1"});
		msg.badges.push_back({"premium", "1"});
		msg.text = "Сообщение с бейджами модератора и Prime.";
		break;
	case 4:
		msg.display = "ActionUser";
		msg.user = "actionuser";
		msg.color = "#FF69B4";
		msg.action = true;
		msg.text = "танцует и машет всем в чате";
		break;
	case 5:
		msg.display = "ChatBot";
		msg.user = "chatbot";
		msg.text = "!команда — это сообщение должно быть скрыто модерацией команд";
		break;
	case 6:
		msg.display = "CapsUser";
		msg.user = "capsuser";
		msg.text = "ЭТО СООБЩЕНИЕ НАПИСАНО КАПСОМ И ДОЛЖНО БЫТЬ СКРЫТО";
		break;
	default:
		msg.display = "LinkUser";
		msg.user = "linkuser";
		msg.text = "смотрите стрим на https://example.com (должно быть скрыто)";
		break;
	}

	return msg;
}

// Must be called with ctx->mutex held.
void start_worker_locked(ChatOverlay *ctx)
{
	if (ctx->cfg.demo_mode) {
		// Demo mode renders local test messages; no network connection is needed.
		return;
	}
	blog(LOG_INFO, "[chat-overlay] starting worker for #%s",
	     ctx->cfg.channel.empty() ? "<none>" : ctx->cfg.channel.c_str());
	ctx->worker.reset(new TwitchWorker(ctx->cfg, ctx->emotes, ctx->images));
	ctx->worker->start();
}

bool read_file_bytes(const std::string &path, std::string &out)
{
	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	std::ostringstream buffer;
	buffer << file.rdbuf();
	out = buffer.str();
	return !out.empty();
}

// Applies a preset, an imported StreamElements export or pasted CSS exactly once
// per change, and writes the resulting values back into the source settings so
// they are stored with the scene and shown in the properties dialog.
void apply_pending_theme(ChatOverlay *ctx, obs_data_t *settings, Config &cfg)
{
	const std::string preset = obs_data_get_string(settings, "theme_preset");
	const std::string file = trim(obs_data_get_string(settings, "theme_file"));
	const std::string css = obs_data_get_string(settings, "theme_css");
	const size_t css_hash = std::hash<std::string>{}(css);

	bool applied = false;
	std::string source;

	if (preset != "none" && preset != ctx->applied_theme_preset) {
		for (const auto &entry : theme_presets()) {
			if (entry.first != preset)
				continue;
			theme_apply(entry.second, cfg);
			applied = true;
			source = "preset " + preset;
			break;
		}
		ctx->applied_theme_preset = preset;
	}

	if (!file.empty() && file != ctx->applied_theme_file) {
		std::string bytes;
		if (!read_file_bytes(file, bytes)) {
			blog(LOG_WARNING, "[chat-overlay] cannot read theme file: %s", file.c_str());
		} else {
			Theme theme;
			std::string error;
			bool ok = false;
			const bool looks_like_json = trim(bytes).rfind("{", 0) == 0;
			if (looks_like_json) {
				ok = theme_from_stream_elements(bytes, theme, &error);
				if (!ok) {
					error.clear();
					ok = theme_from_json(bytes, theme, &error);
				}
			} else {
				ok = theme_from_css(bytes, theme, &error);
			}
			if (ok) {
				theme_apply(theme, cfg);
				applied = true;
				source = "file " + file;
			} else {
				blog(LOG_WARNING, "[chat-overlay] theme file rejected: %s", error.c_str());
			}
		}
		ctx->applied_theme_file = file;
	}

	if (!css.empty() && css_hash != ctx->applied_css_hash) {
		Theme theme;
		std::string error;
		if (theme_from_css(css, theme, &error)) {
			theme_apply(theme, cfg);
			applied = true;
			source = "pasted css";
		} else {
			blog(LOG_WARNING, "[chat-overlay] pasted CSS rejected: %s", error.c_str());
		}
		ctx->applied_css_hash = css_hash;
	}

	if (!applied)
		return;

	obs_data_set_string(settings, "font_face", cfg.font_face.c_str());
	obs_data_set_int(settings, "font_size", (long long)std::lround(cfg.font_size));
	obs_data_set_bool(settings, "bold", cfg.bold);
	obs_data_set_bool(settings, "italic", cfg.italic);
	obs_data_set_int(settings, "text_color", cfg.text_color);
	obs_data_set_int(settings, "nick_color", cfg.nick_color);
	obs_data_set_int(settings, "bg_color", cfg.bg_color);
	obs_data_set_int(settings, "outline_color", cfg.outline_color);
	obs_data_set_int(settings, "outline_size", cfg.outline_size);
	obs_data_set_int(settings, "bg_radius", cfg.bg_radius);
	obs_data_set_int(settings, "bg_padding", cfg.bg_padding);
	obs_data_set_int(settings, "line_spacing", cfg.line_spacing);
	obs_data_set_double(settings, "emote_scale", cfg.emote_scale);
	obs_data_set_int(settings, "text_align", cfg.text_align);

	blog(LOG_INFO, "[chat-overlay] theme applied (%s): font=%s/%d", source.c_str(),
	     cfg.font_face.c_str(), (int)std::lround(cfg.font_size));
}

// Field defaults are only *defaults*: the widget keeps using the values from
// data.txt until the user actually edits a control in the dialog.
void ensure_widget_field_defaults(obs_data_t *settings, const std::string &dir)
{
	for (const WidgetFieldInfo &field : widget_fields(dir)) {
		const std::string key = "wf_" + field.name;
		if (obs_data_has_user_value(settings, key.c_str()))
			continue;

		if (field.type == "colorpicker") {
			uint32_t color = 0xFFFFFFFFu;
			uint32_t parsed = 0;
			if (parse_css_color(field.value, parsed))
				color = parsed;
			obs_data_set_default_int(settings, key.c_str(), (long long)color);
		} else if (field.type == "checkbox") {
			obs_data_set_default_bool(settings, key.c_str(), field.value == "true");
		} else if (field.type == "number") {
			obs_data_set_default_int(settings, key.c_str(), std::atoll(field.value.c_str()));
		} else {
			obs_data_set_default_string(settings, key.c_str(), field.value.c_str());
		}
	}
}

std::map<std::string, std::string> widget_overrides(obs_data_t *settings, const std::string &dir)
{
	std::map<std::string, std::string> result;
	for (const WidgetFieldInfo &field : widget_fields(dir)) {
		const std::string key = "wf_" + field.name;
		if (!obs_data_has_user_value(settings, key.c_str()))
			continue;

		if (field.type == "colorpicker") {
			result[field.name] =
				css_color_from_obs((uint32_t)obs_data_get_int(settings, key.c_str()));
		} else if (field.type == "checkbox") {
			result[field.name] = obs_data_get_bool(settings, key.c_str()) ? "true" : "false";
		} else if (field.type == "number") {
			result[field.name] = std::to_string(obs_data_get_int(settings, key.c_str()));
		} else {
			result[field.name] = obs_data_get_string(settings, key.c_str());
		}
	}
	return result;
}

void *chat_overlay_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new ChatOverlay();
	ctx->source = source;
	ctx->cfg = Config();
	ctx->emotes = std::make_shared<EmoteRegistry>();
	ctx->images = std::make_shared<ImageCache>(2, ctx->cfg.proxy);
	ctx->renderer.reset(new ChatRenderer(ctx->cfg, *ctx->emotes, ctx->images));
	chat_overlay_update(ctx, settings);
	return ctx;
}

void chat_overlay_destroy(void *data)
{
	auto *ctx = (ChatOverlay *)data;
	if (!ctx)
		return;

	std::unique_ptr<TwitchWorker> worker;
	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		worker = std::move(ctx->worker);
		if (ctx->widget) {
			ctx->widget->stop();
			ctx->widget.reset();
		}
		ctx->renderer.reset();
	}
	if (worker)
		worker->stop();
	// Clean shutdown: the next OBS start may create the browser source again.
	widget_mark_clean();

	destroy_texture(ctx);
	ctx->images.reset();
	ctx->emotes.reset();
	delete ctx;
}

void chat_overlay_update(void *data, obs_data_t *settings)
{
	auto *ctx = (ChatOverlay *)data;
	if (!ctx)
		return;

	Config cfg = config_from_settings(settings);
	// Fill dialog defaults for the widget's editable fields, then collect only
	// the values the user actually changed.
	ensure_widget_field_defaults(settings, cfg.widget_dir);
	std::map<std::string, std::string> overrides = widget_overrides(settings, cfg.widget_dir);
	// Keep the conversation from growing without limit: every message on screen
	// carries infinite CSS animations, so a growing list slowly eats the CPU.
	if (!overrides.count("hideAfter")) {
		const long long seconds = obs_data_get_int(settings, "widget_hide_after");
		if (seconds > 0)
			overrides["hideAfter"] = std::to_string(seconds);
	}
	if (!overrides.count("messagesLimit")) {
		const long long limit = obs_data_get_int(settings, "widget_messages_limit");
		if (limit > 0)
			overrides["messagesLimit"] = std::to_string(limit);
	}
	apply_pending_theme(ctx, settings, cfg);

	std::unique_ptr<TwitchWorker> previous_worker;
	bool restart = false;

	{
		std::lock_guard<std::mutex> lock(ctx->mutex);
		const Config previous = ctx->cfg;
		ctx->cfg = cfg;
		if (ctx->images)
			ctx->images->set_debug(cfg.debug_log);

		if (ctx->renderer)
			ctx->renderer->configure(cfg);

		const bool proxy_changed = cfg.proxy != previous.proxy;
		const bool channel_changed = cfg.channel != previous.channel;
		const bool connection_changed = cfg.channel != previous.channel ||
						cfg.oauth != previous.oauth ||
						cfg.username != previous.username ||
						cfg.client_id != previous.client_id ||
						cfg.use_tls != previous.use_tls;
		const bool emote_sources_changed = cfg.twitch_emotes != previous.twitch_emotes ||
						   cfg.bttv_emotes != previous.bttv_emotes ||
						   cfg.seventv_emotes != previous.seventv_emotes;
		const bool demo_changed = cfg.demo_mode != previous.demo_mode;

		if (proxy_changed) {
			// The image cache owns its proxy configuration. Swapping it is
			// enough: the renderer keeps its already decoded textures.
			ctx->images = std::make_shared<ImageCache>(2, cfg.proxy);
			ctx->images->set_debug(cfg.debug_log);
			if (ctx->renderer)
				ctx->renderer->set_images(ctx->images);
			restart = true;
		}

		if (channel_changed) {
			// Badge and emote art is per-channel: drop everything cached for the
			// previous channel and start with a clean renderer.
			ctx->images->clear();
			if (ctx->emotes)
				ctx->emotes->clear();
			ctx->renderer.reset(new ChatRenderer(ctx->cfg, *ctx->emotes, ctx->images));
			restart = true;
		}

		if (connection_changed || emote_sources_changed)
			restart = true;

		if (demo_changed) {
			restart = true;
			blog(LOG_INFO, "[chat-overlay] demo mode %s",
			     cfg.demo_mode ? "enabled: local test messages, no network"
					   : "disabled: back to the live channel");
			if (cfg.demo_mode) {
				if (ctx->renderer)
					ctx->renderer->clear();
				ctx->demo_index = 0;
				ctx->next_demo_time = 0.0;
				// Badge images for the demo messages (global Twitch badges).
				ctx->images->register_image(
					"badge:moderator/1",
					"https://static-cdn.jtvnw.net/badges/v1/3267646d-33f0-4b17-b3df-f923a41db1d0/2");
				ctx->images->register_image(
					"badge:premium/1",
					"https://static-cdn.jtvnw.net/badges/v1/bbbe0db0-a598-423e-86d0-f9fb98ca1933/2");
			}
		}

		const bool widget_mode_changed = cfg.widget_mode != previous.widget_mode ||
						 cfg.widget_dir != previous.widget_dir;
		const bool widget_size_changed = cfg.width != previous.width ||
						 cfg.height != previous.height;
		const bool widget_enabled = cfg.widget_mode && !cfg.widget_dir.empty();
		const bool user_enabled = cfg.widget_mode && !previous.widget_mode && ctx->started_once;
		ctx->pending_overrides = overrides;

		if (!widget_enabled) {
			if (ctx->widget) {
				ctx->widget->stop();
				ctx->widget.reset();
				widget_mark_clean();
			}
			ctx->widget_wanted = false;
		} else if (!ctx->widget_wanted) {
			// Schedule the start; chat_overlay_tick() creates the browser source
			// once CEF has had time to initialise.
			if (widget_crash_suspected() && !user_enabled) {
				if (!ctx->crash_warned) {
					ctx->crash_warned = true;
					blog(LOG_WARNING,
					     "[chat-overlay] OBS seems to have crashed with the widget "
					     "running; automatic start is skipped. Turn 'widget mode' "
					     "off and on again to retry.");
				}
			} else {
				widget_mark_clean();
				ctx->widget_wanted = true;
				ctx->widget_arm_time = monotonic_seconds() + 4.0;
				blog(LOG_INFO, "[chat-overlay] widget scheduled in 4 s: %s",
				     cfg.widget_dir.c_str());
			}
		} else if (widget_mode_changed) {
			// The widget folder changed while running: restart with the new one.
			if (ctx->widget) {
				ctx->widget->stop();
				ctx->widget.reset();
				widget_mark_clean();
			}
			ctx->widget_arm_time = monotonic_seconds() + 4.0;
			blog(LOG_INFO, "[chat-overlay] widget folder changed, restarting in 4 s: %s",
			     cfg.widget_dir.c_str());
		} else if (ctx->widget) {
			// Size changes only resize the browser; field edits only patch CSS.
			// Both avoid re-creating the CEF browser (which could crash it).
			if (widget_size_changed)
				ctx->widget->set_size(cfg.width, cfg.height);
			if (cfg.widget_fps != previous.widget_fps)
				ctx->widget->set_fps(cfg.widget_fps);
			ctx->widget->set_overrides(overrides);
		}

		if (restart)
			previous_worker = std::move(ctx->worker);
	}

	// Stopping the old worker may block for a few seconds, so do it without
	// holding the mutex that the video thread needs.
	if (previous_worker)
		previous_worker->stop();

	if (restart) {
		std::lock_guard<std::mutex> lock(ctx->mutex);
		start_worker_locked(ctx);
	}
}

void chat_overlay_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	auto *ctx = (ChatOverlay *)data;
	if (!ctx)
		return;

	const double now = monotonic_seconds();

	std::lock_guard<std::mutex> lock(ctx->mutex);

	// Deferred widget start: the browser source is created only after OBS and CEF
	// have settled, never during scene loading (that path crashed libcef).
	if (ctx->widget_wanted && !ctx->widget && now >= ctx->widget_arm_time) {
		std::string error;
		auto host = std::make_unique<WidgetHost>();
		if (host->start(ctx->source, ctx->cfg.widget_dir, ctx->emotes.get(),
				ctx->images.get(), ctx->pending_overrides, ctx->cfg.width,
				ctx->cfg.height, ctx->cfg.widget_fps, &error)) {
			ctx->widget = std::move(host);
			widget_mark_active();
			blog(LOG_INFO, "[chat-overlay] widget mode enabled: %s",
			     ctx->cfg.widget_dir.c_str());
		} else {
			ctx->widget_wanted = false;
			blog(LOG_WARNING,
			     "[chat-overlay] widget mode unavailable, using the native renderer: %s",
			     error.c_str());
		}
	}

	const bool widget_active = ctx->widget && ctx->widget->active();
	if (widget_active)
		ctx->widget->tick(seconds);
	// While the widget is live it owns the screen, so the native renderer is not
	// updated at all — text layout is the most expensive part of our own work.
	const bool widget_ready = widget_active && ctx->widget->ready();

	if (ctx->cfg.demo_mode) {
		if (now >= ctx->next_demo_time) {
			ctx->next_demo_time = now + 1.5;
			ChatMessage msg = make_demo_message(ctx->demo_index++);
			msg.seq = ctx->demo_index;
			msg.recv_time = now;
			if (moderation_allows(ctx->cfg, msg)) {
				// Feed both paths: the native renderer covers the moment while the
				// embedded browser is still starting up.
				if (!widget_ready)
					ctx->renderer->add(msg);
				if (widget_active)
					ctx->widget->feed(msg, ctx->cfg.channel, ctx->cfg);
			}
		}
	} else if (ctx->worker) {
		std::vector<ChatMessage> incoming;
		ctx->worker->drain(incoming, 128);
		for (ChatMessage &msg : incoming) {
			if (!moderation_allows(ctx->cfg, msg))
				continue;
			if (!widget_ready)
				ctx->renderer->add(msg);
			if (widget_active)
				ctx->widget->feed(msg, ctx->cfg.channel, ctx->cfg);
		}
	}

	if (!widget_ready)
		ctx->renderer->tick(now);
	if (widget_active)
		ctx->widget->poll();

	// From now on settings changes are user actions, so an explicit "widget mode"
	// toggle is allowed to retry after a crash.
	ctx->started_once = true;
}

// Draws the prepared panel texture. Because the source uses
// OBS_SOURCE_CUSTOM_DRAW, OBS passes a NULL effect and does not bind any, so the
// effect loop is set up here (obs_source_draw() would fail with
// "no active effect").
void draw_panel_texture(ChatOverlay *ctx)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (!effect)
		return;

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	if (image)
		gs_effect_set_texture(image, ctx->texture);

	gs_eparam_t *color = gs_effect_get_param_by_name(effect, "color");
	if (color) {
		struct vec4 white;
		vec4_set(&white, 1.0f, 1.0f, 1.0f, 1.0f);
		gs_effect_set_vec4(color, &white);
	}

	gs_matrix_push();
	gs_matrix_translate3f((float)ctx->draw_x, (float)ctx->draw_y, 0.0f);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(ctx->texture, 0, ctx->texture_w, ctx->texture_h);
	gs_matrix_pop();
}

void chat_overlay_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	auto *ctx = (ChatOverlay *)data;
	if (!ctx)
		return;

	std::lock_guard<std::mutex> lock(ctx->mutex);
	// Render the widget only once its page has connected; before that the native
	// renderer keeps the chat visible instead of a black rectangle.
	if (ctx->widget && ctx->widget->ready()) {
		ctx->widget->render();
		return;
	}
	if (!ctx->renderer || ctx->renderer->empty())
		return;

	RenderedPanel &panel = ctx->panel;
	const bool rebuilt = ctx->renderer->build(monotonic_seconds(), panel);
	if (rebuilt) {
		ctx->draw_x = panel.offset_x;
		ctx->draw_y = panel.offset_y;
	}

	if (panel.rgba.empty() || panel.width <= 0 || panel.height <= 0)
		return;

	const bool size_mismatch = ctx->texture == nullptr ||
				   ctx->texture_w != (uint32_t)panel.width ||
				   ctx->texture_h != (uint32_t)panel.height;

	if (rebuilt || size_mismatch) {
		// video_render() already runs inside the graphics context, so textures
		// are created and uploaded directly without obs_enter_graphics().
		if (size_mismatch) {
			if (ctx->texture) {
				gs_texture_destroy(ctx->texture);
				ctx->texture = nullptr;
			}
			ctx->texture = gs_texture_create((uint32_t)panel.width, (uint32_t)panel.height,
							 GS_RGBA, 1, nullptr, GS_DYNAMIC);
			ctx->texture_w = (uint32_t)panel.width;
			ctx->texture_h = (uint32_t)panel.height;
		}
		if (ctx->texture) {
			gs_texture_set_image(ctx->texture, panel.rgba.data(),
					     (uint32_t)panel.width * 4, false);
		}
	}

	if (ctx->texture)
		draw_panel_texture(ctx);
}

void chat_overlay_enum_active_sources(void *data, obs_source_enum_proc_t enum_callback,
				      void *param)
{
	auto *ctx = (ChatOverlay *)data;
	if (!ctx)
		return;

	std::lock_guard<std::mutex> lock(ctx->mutex);
	if (ctx->widget && ctx->widget->source())
		enum_callback(ctx->source, ctx->widget->source(), param);
}

uint32_t chat_overlay_width(void *data)
{
	auto *ctx = (ChatOverlay *)data;
	return ctx ? ctx->cfg.width : 1920;
}

uint32_t chat_overlay_height(void *data)
{
	auto *ctx = (ChatOverlay *)data;
	return ctx ? ctx->cfg.height : 1080;
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

void add_font_list(obs_properties_t *props)
{
	obs_property_t *list = obs_properties_add_list(props, "font_face", "Шрифт",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	const std::vector<std::string> families = ChatRenderer::font_families();
	if (families.empty()) {
		obs_property_list_add_string(list, "Segoe UI", "Segoe UI");
		return;
	}
	for (const std::string &family : families)
		obs_property_list_add_string(list, family.c_str(), family.c_str());
}

void add_anchor_list(obs_properties_t *props)
{
	obs_property_t *list = obs_properties_add_list(props, "anchor", "Привязка",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "Снизу слева", (long long)Anchor::BottomLeft);
	obs_property_list_add_int(list, "Снизу справа", (long long)Anchor::BottomRight);
	obs_property_list_add_int(list, "Сверху слева", (long long)Anchor::TopLeft);
	obs_property_list_add_int(list, "Сверху справа", (long long)Anchor::TopRight);
}

void add_align_list(obs_properties_t *props)
{
	obs_property_t *list = obs_properties_add_list(props, "text_align", "Выравнивание текста",
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(list, "Слева", 0);
	obs_property_list_add_int(list, "По центру", 1);
	obs_property_list_add_int(list, "Справа", 2);
}

void config_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "channel", "");
	obs_data_set_default_string(settings, "username", "");
	obs_data_set_default_string(settings, "oauth", "");
	obs_data_set_default_string(settings, "client_id", "");
	obs_data_set_default_bool(settings, "use_tls", true);
	obs_data_set_default_string(settings, "proxy", "");
	obs_data_set_default_bool(settings, "debug_log", false);

	obs_data_set_default_int(settings, "width", 1920);
	obs_data_set_default_int(settings, "height", 1080);
	obs_data_set_default_string(settings, "font_face", "Segoe UI");
	obs_data_set_default_int(settings, "font_size", 28);
	obs_data_set_default_bool(settings, "bold", true);
	obs_data_set_default_bool(settings, "italic", false);
	obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "nick_color", 0xFFD0A060);
	obs_data_set_default_int(settings, "bg_color", 0x80000000);
	obs_data_set_default_int(settings, "outline_color", 0xFF000000);
	obs_data_set_default_int(settings, "outline_size", 2);
	obs_data_set_default_int(settings, "bg_padding", 8);
	obs_data_set_default_int(settings, "bg_radius", 6);
	obs_data_set_default_int(settings, "line_spacing", 4);
	obs_data_set_default_double(settings, "emote_scale", 1.0);
	obs_data_set_default_bool(settings, "show_badges", true);
	obs_data_set_default_bool(settings, "show_nick", true);
	obs_data_set_default_bool(settings, "show_timestamps", false);

	obs_data_set_default_int(settings, "anchor", (long long)Anchor::BottomLeft);
	obs_data_set_default_int(settings, "margin_x", 32);
	obs_data_set_default_int(settings, "margin_y", 32);
	obs_data_set_default_int(settings, "max_width", 900);
	obs_data_set_default_int(settings, "text_align", 0);

	obs_data_set_default_int(settings, "max_messages", 15);
	obs_data_set_default_int(settings, "ttl_seconds", 0);
	obs_data_set_default_int(settings, "fade_ms", 500);

	obs_data_set_default_bool(settings, "hide_commands", true);
	obs_data_set_default_string(settings, "command_prefix", "!");
	obs_data_set_default_bool(settings, "hide_links", true);
	obs_data_set_default_int(settings, "caps_threshold", 0);
	obs_data_set_default_string(settings, "stopwords", "");
	obs_data_set_default_string(settings, "ignore_users", "");

	obs_data_set_default_bool(settings, "twitch_emotes", true);
	obs_data_set_default_bool(settings, "bttv_emotes", true);
	obs_data_set_default_bool(settings, "seventv_emotes", true);

	obs_data_set_default_string(settings, "theme_preset", "none");
	obs_data_set_default_string(settings, "theme_file", "");
	obs_data_set_default_string(settings, "theme_css", "");

	obs_data_set_default_bool(settings, "demo_mode", false);
	obs_data_set_default_bool(settings, "widget_mode", false);
	obs_data_set_default_string(settings, "widget_dir", "");
	obs_data_set_default_bool(settings, "twitch_nick_colors", true);
	obs_data_set_default_int(settings, "widget_fps", 15);
	obs_data_set_default_int(settings, "widget_hide_after", 60);
	obs_data_set_default_int(settings, "widget_messages_limit", 15);
}

obs_properties_t *chat_overlay_properties(void *data)
{
	auto *ctx = (ChatOverlay *)data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_t *connection = obs_properties_create();
	obs_properties_add_text(connection, "channel", "Канал Twitch (логин)", OBS_TEXT_DEFAULT);
	obs_properties_add_text(connection, "username", "Ваш логин (для OAuth)", OBS_TEXT_DEFAULT);
	obs_properties_add_text(connection, "oauth", "OAuth-токен", OBS_TEXT_PASSWORD);
	obs_properties_add_text(connection, "client_id", "Twitch Client-ID (для бейджей)",
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(connection, "use_tls", "Шифрованное соединение (TLS)");
	obs_properties_add_text(connection, "proxy", "HTTP-прокси (host:port, необязательно)",
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(connection, "debug_log", "Подробный лог (для диагностики)");
	obs_properties_add_group(props, "connection_group", "Подключение", OBS_GROUP_NORMAL,
				 connection);

	obs_properties_t *look = obs_properties_create();
	add_font_list(look);
	obs_properties_add_int(look, "font_size", "Размер шрифта", 6, 200, 1);
	obs_properties_add_bool(look, "bold", "Жирный");
	obs_properties_add_bool(look, "italic", "Курсив");
	obs_properties_add_color_alpha(look, "text_color", "Цвет текста");
	obs_properties_add_color_alpha(look, "nick_color", "Цвет ника (если Twitch не задал)");
	obs_properties_add_color_alpha(look, "bg_color", "Фон панели");
	obs_properties_add_int(look, "bg_radius", "Скругление фона", 0, 64, 1);
	obs_properties_add_int(look, "bg_padding", "Отступ текста от края", 0, 64, 1);
	obs_properties_add_color_alpha(look, "outline_color", "Цвет обводки");
	obs_properties_add_int(look, "outline_size", "Толщина обводки", 0, 8, 1);
	obs_properties_add_float_slider(look, "emote_scale", "Масштаб смайлов", 0.5, 3.0, 0.05);
	obs_properties_add_bool(look, "show_badges", "Показывать бейджи");
	obs_properties_add_bool(look, "show_nick", "Показывать ник");
	obs_properties_add_group(props, "look_group", "Внешний вид", OBS_GROUP_NORMAL, look);

	obs_properties_t *theme = obs_properties_create();
	obs_property_t *preset_list = obs_properties_add_list(
		theme, "theme_preset", "Готовый пресет", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset_list, "Не применять", "none");
	obs_property_list_add_string(preset_list, "StreamElements — классика", "se-classic");
	obs_property_list_add_string(preset_list, "Пузырьки", "bubbles");
	obs_property_list_add_string(preset_list, "Тёмная панель", "dark-panel");
	obs_property_list_add_string(preset_list, "Чистый (без плашки)", "clean");
	obs_properties_add_path(theme, "theme_file",
				"Файл темы (экспорт StreamElements, .css или .json)", OBS_PATH_FILE,
				"Темы (*.json;*.css;*.txt);;Все файлы (*.*)", nullptr);
	obs_properties_add_text(theme, "theme_css",
				"Вставить CSS из StreamElements / Streamlabs", OBS_TEXT_MULTILINE);
	obs_properties_add_text(
		theme, "theme_help",
		"Пресет, файл или CSS применяются один раз — при нажатии «Применить». "
		"Дальше правьте настройки вручную: плагин не будет их перезаписывать. "
		"Подробности в docs/THEMES.md.",
		OBS_TEXT_INFO);
	obs_properties_add_group(props, "theme_group", "Тема и импорт", OBS_GROUP_NORMAL, theme);

	obs_properties_t *widget = obs_properties_create();
	obs_properties_add_bool(widget, "widget_mode",
				"Запускать HTML/CSS/JS виджет (StreamElements)");
	obs_properties_add_path(
		widget, "widget_dir", "Папка виджета (html/css/js/fields/data)", OBS_PATH_DIRECTORY,
		nullptr, nullptr);
	obs_properties_add_bool(
		widget, "twitch_nick_colors",
		"Красить ники цветом Twitch (выключить — использовать цвет из виджета)");
	obs_properties_add_int(
		widget, "widget_fps",
		"Частота кадров виджета (меньше — меньше нагрузка на CPU)", 5, 60, 1);
	obs_properties_add_int(widget, "widget_hide_after",
			       "Скрывать сообщение через, сек (0 = как в виджете)", 0, 3600, 5);
	obs_properties_add_int(widget, "widget_messages_limit",
			       "Максимум сообщений на экране (0 = как в виджете)", 0, 50, 1);
	obs_properties_add_text(
		widget, "widget_help",
		"Включите режим и укажите папку с файлами виджета из StreamElements "
		"(html.txt, css.txt, js.txt, fields.txt, data.txt). Плагин соберёт из них "
		"страницу, откроет её во встроенном браузере OBS и будет передавать "
		"сообщения чата событиями onWidgetLoad / onEventReceived — виджет работает "
		"как в StreamElements. Настройки «Внешнего вида» и модерация при этом не "
		"применяются: вид оформляется самим виджетом. Нужен установленный "
		"obs-browser (идёт с OBS).",
		OBS_TEXT_INFO);
	obs_properties_add_group(props, "widget_group", "Виджет (HTML/CSS/JS)", OBS_GROUP_NORMAL,
				 widget);

	// Editable parameters declared by the widget in fields.txt. Values come from
	// data.txt until the user changes a control here.
	if (ctx && !ctx->cfg.widget_dir.empty()) {
		const std::vector<WidgetFieldInfo> fields = widget_fields(ctx->cfg.widget_dir);
		if (!fields.empty()) {
			obs_properties_t *params = obs_properties_create();
			for (const WidgetFieldInfo &field : fields) {
				const std::string key = "wf_" + field.name;
				if (field.type == "colorpicker") {
					obs_properties_add_color_alpha(params, key.c_str(),
								       field.label.c_str());
				} else if (field.type == "checkbox") {
					obs_properties_add_bool(params, key.c_str(), field.label.c_str());
				} else if (field.type == "dropdown") {
					obs_property_t *list = obs_properties_add_list(
						params, key.c_str(), field.label.c_str(),
						OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
					for (const auto &option : field.options)
						obs_property_list_add_string(
							list, option.second.c_str(), option.first.c_str());
				} else if (field.type == "number") {
					obs_properties_add_int(params, key.c_str(), field.label.c_str(), 0,
							       10000, 1);
				} else {
					obs_properties_add_text(params, key.c_str(), field.label.c_str(),
								OBS_TEXT_DEFAULT);
				}
			}
			obs_properties_add_text(
				params, "widget_fields_help",
				"Значения по умолчанию берутся из data.txt. Меняйте только то, что "
				"нужно; изменения применяются к виджету по кнопке «Применить».",
				OBS_TEXT_INFO);
			obs_properties_add_group(props, "widget_fields_group", "Параметры виджета",
						 OBS_GROUP_NORMAL, params);
		}
	}

	obs_properties_t *diagnostics = obs_properties_create();
	obs_properties_add_bool(diagnostics, "demo_mode",
				"Демо-режим: тестовые сообщения без сети");
	obs_properties_add_text(
		diagnostics, "diag_help",
		"Демо-режим каждые 1,5 секунды добавляет тестовые сообщения: обычный текст, "
		"смайлы Twitch, длинную строку для проверки переноса, бейджи, /me и три "
		"сообщения, которые должна скрыть модерация. Сеть не используется — "
		"удобно проверять оформление, когда канал оффлайн.",
		OBS_TEXT_INFO);
	obs_properties_add_group(props, "diag_group", "Диагностика", OBS_GROUP_NORMAL,
				 diagnostics);

	obs_properties_t *layout = obs_properties_create();
	obs_properties_add_int(layout, "width", "Ширина источника", 320, 7680, 1);
	obs_properties_add_int(layout, "height", "Высота источника", 240, 4320, 1);
	obs_properties_add_int(layout, "max_width", "Ширина блока чата", 120, 3840, 1);
	obs_properties_add_int(layout, "max_messages", "Максимум сообщений", 1, 60, 1);
	obs_properties_add_int(layout, "ttl_seconds", "Время жизни сообщения, сек (0 = всегда)",
			       0, 3600, 1);
	obs_properties_add_int(layout, "fade_ms", "Плавное исчезновение, мс", 0, 5000, 50);
	obs_properties_add_int(layout, "line_spacing", "Отступ между сообщениями", 0, 64, 1);
	add_anchor_list(layout);
	obs_properties_add_int(layout, "margin_x", "Отступ по X", 0, 2000, 1);
	obs_properties_add_int(layout, "margin_y", "Отступ по Y", 0, 2000, 1);
	add_align_list(layout);
	obs_properties_add_group(props, "layout_group", "Разметка", OBS_GROUP_NORMAL, layout);

	obs_properties_t *moderation = obs_properties_create();
	obs_properties_add_bool(moderation, "hide_commands", "Скрывать команды");
	obs_properties_add_text(moderation, "command_prefix", "Префиксы команд (через запятую)",
				OBS_TEXT_DEFAULT);
	obs_properties_add_bool(moderation, "hide_links", "Скрывать ссылки");
	obs_properties_add_int(moderation, "caps_threshold", "Порог капса, % (0 = выключено)", 0,
			       100, 5);
	obs_properties_add_text(moderation, "stopwords", "Стоп-слова (через запятую)",
				OBS_TEXT_MULTILINE);
	obs_properties_add_text(moderation, "ignore_users", "Игнорировать пользователей (логины)",
				OBS_TEXT_MULTILINE);
	obs_properties_add_group(props, "mod_group", "Модерация", OBS_GROUP_NORMAL, moderation);

	obs_properties_t *emotes = obs_properties_create();
	obs_properties_add_bool(emotes, "twitch_emotes", "Смайлы Twitch");
	obs_properties_add_bool(emotes, "bttv_emotes", "Смайлы BTTV");
	obs_properties_add_bool(emotes, "seventv_emotes", "Смайлы 7TV");
	obs_properties_add_group(props, "emote_group", "Смайлы", OBS_GROUP_NORMAL, emotes);

	return props;
}

void chat_overlay_defaults(obs_data_t *settings)
{
	config_defaults(settings);
}

const char *chat_overlay_name(void *type_data)
{
	UNUSED_PARAMETER(type_data);
	return "Chat Overlay";
}

} // namespace

bool obs_module_load(void)
{
	static obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	info.get_name = chat_overlay_name;
	info.create = chat_overlay_create;
	info.destroy = chat_overlay_destroy;
	info.get_width = chat_overlay_width;
	info.get_height = chat_overlay_height;
	info.get_defaults = chat_overlay_defaults;
	info.get_properties = chat_overlay_properties;
	info.update = chat_overlay_update;
	info.video_tick = chat_overlay_tick;
	info.video_render = chat_overlay_render;
	info.enum_active_sources = chat_overlay_enum_active_sources;

	obs_register_source(&info);
	blog(LOG_INFO, "[chat-overlay] loaded, source id %s (version %s)", kSourceId,
	     CHAT_OVERLAY_VERSION);
	return true;
}

void obs_module_unload(void)
{
	blog(LOG_INFO, "[chat-overlay] unloaded");
}

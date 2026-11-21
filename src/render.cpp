#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "render.hpp"

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>
#include <set>

using Microsoft::WRL::ComPtr;

namespace {

D2D1_COLOR_F color_from(uint32_t obs_color, float alpha_mul)
{
	// OBS stores colours as 0xAABBGGRR.
	D2D1_COLOR_F color;
	color.r = (float)(obs_color & 0xFF) / 255.0f;
	color.g = (float)((obs_color >> 8) & 0xFF) / 255.0f;
	color.b = (float)((obs_color >> 16) & 0xFF) / 255.0f;
	color.a = ((float)((obs_color >> 24) & 0xFF) / 255.0f) * alpha_mul;
	return color;
}

bool parse_hex_color(const std::string &hex, uint32_t &out)
{
	if (hex.size() < 7 || hex[0] != '#')
		return false;
	unsigned int value = 0;
	for (size_t i = 1; i < 7; ++i) {
		const char c = hex[i];
		unsigned int digit = 0;
		if (c >= '0' && c <= '9')
			digit = (unsigned int)(c - '0');
		else if (c >= 'a' && c <= 'f')
			digit = (unsigned int)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			digit = (unsigned int)(c - 'A' + 10);
		else
			return false;
		value = (value << 4) | digit;
	}
	const uint32_t r = (value >> 16) & 0xFF;
	const uint32_t g = (value >> 8) & 0xFF;
	const uint32_t b = value & 0xFF;
	out = 0xFF000000u | (b << 16) | (g << 8) | r;
	return true;
}

} // namespace

struct ChatRenderer::Impl {
	struct Op {
		enum class Kind { Text, Image } kind = Kind::Text;
		std::wstring text;
		std::string image_key;
		float x = 0.0f;
		float w = 0.0f;
		float h = 0.0f;
		uint32_t color = 0xFFFFFFFFu;
	};

	struct Line {
		std::vector<Op> ops;
		float width = 0.0f;
	};

	struct Message {
		ChatMessage msg;
		std::vector<Line> lines;
		float alpha = 1.0f;
		double recv = 0.0;
		float height = 0.0f;
	};

	struct Word {
		std::vector<Op> ops;
		float width = 0.0f;
	};

	Config cfg;
	EmoteRegistry &emotes;
	std::shared_ptr<ImageCache> images;

	ComPtr<IDWriteFactory> dwrite;
	ComPtr<ID2D1Factory> d2d;
	ComPtr<IWICImagingFactory> wic;
	ComPtr<IDWriteTextFormat> format;
	ComPtr<IWICBitmap> bitmap;
	ComPtr<ID2D1RenderTarget> rt;
	ComPtr<ID2D1SolidColorBrush> brush;

	std::vector<Message> messages;
	std::map<std::wstring, float> measure_cache;
	std::map<std::string, DecodedImage> decoded;
	std::map<std::string, ComPtr<ID2D1Bitmap>> bitmaps;
	std::set<std::string> pending;
	std::set<std::string> missing;
	std::set<std::string> waiting; // keys whose URL is not known yet

	bool gfx_ready = false;
	bool layout_dirty = true;
	bool draw_dirty = true;
	int tex_w = 0;
	int tex_h = 0;
	int panel_w = 0;
	int panel_h = 0;
	float line_h = 0.0f;
	float space_w = 0.0f;

	Impl(const Config &config, EmoteRegistry &emote_registry, std::shared_ptr<ImageCache> image_cache)
		: cfg(config), emotes(emote_registry), images(std::move(image_cache))
	{
	}

	~Impl() { release_target(); }

	// ---------------------------------------------------------------- graphics

	bool init_gfx()
	{
		if (gfx_ready)
			return true;

		CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf())))
			return false;
		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
					       reinterpret_cast<IUnknown **>(dwrite.GetAddressOf()))))
			return false;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
					    IID_PPV_ARGS(wic.GetAddressOf()))))
			return false;

		if (!ensure_format())
			return false;

		gfx_ready = true;
		return true;
	}

	bool create_format(const wchar_t *face, IDWriteTextFormat **out)
	{
		const float size = cfg.font_size < 6.0f ? 6.0f : cfg.font_size;
		const DWRITE_FONT_WEIGHT weight =
			cfg.bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL;
		const DWRITE_FONT_STYLE style =
			cfg.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL;
		if (FAILED(dwrite->CreateTextFormat(face, nullptr, weight, style, DWRITE_FONT_STRETCH_NORMAL,
						    size, L"en-us", out)))
			return false;
		(*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
		return true;
	}

	bool ensure_format()
	{
		if (format)
			return true;
		const std::wstring face = utf8_to_wide(cfg.font_face.empty() ? "Segoe UI" : cfg.font_face);
		if (create_format(face.c_str(), format.GetAddressOf()))
			return true;
		if (create_format(L"Segoe UI", format.GetAddressOf()))
			return true;
		return false;
	}

	float measure(const std::wstring &text)
	{
		if (text.empty())
			return 0.0f;
		const auto cached = measure_cache.find(text);
		if (cached != measure_cache.end())
			return cached->second;

		float width = 0.0f;
		ComPtr<IDWriteTextLayout> layout;
		if (SUCCEEDED(dwrite->CreateTextLayout(text.c_str(), (UINT32)text.size(), format.Get(),
						       400000.0f, 400000.0f, layout.GetAddressOf()))) {
			DWRITE_TEXT_METRICS metrics;
			if (SUCCEEDED(layout->GetMetrics(&metrics)))
				width = metrics.widthIncludingTrailingWhitespace;
		}
		if (measure_cache.size() > 3000)
			measure_cache.clear();
		measure_cache[text] = width;
		return width;
	}

	float text_line_height()
	{
		if (line_h > 0.0f)
			return line_h;
		float height = cfg.font_size * 1.35f;
		ComPtr<IDWriteTextLayout> layout;
		if (SUCCEEDED(dwrite->CreateTextLayout(L"Ag", 2, format.Get(), 400000.0f, 400000.0f,
						       layout.GetAddressOf()))) {
			DWRITE_TEXT_METRICS metrics;
			if (SUCCEEDED(layout->GetMetrics(&metrics)) && metrics.height > 0.0f)
				height = metrics.height;
		}
		line_h = height;
		return line_h;
	}

	float emote_height()
	{
		const float scale = cfg.emote_scale <= 0.0f ? 1.0f : cfg.emote_scale;
		return text_line_height() * scale;
	}

	float line_advance()
	{
		const float text = text_line_height();
		const float emote = emote_height();
		return emote > text ? emote : text;
	}

	float space_width()
	{
		if (space_w <= 0.0f)
			space_w = measure(L" ");
		if (space_w <= 0.0f)
			space_w = cfg.font_size * 0.3f;
		return space_w;
	}

	// ------------------------------------------------------------------ layout

	void add_image_op(Word &word, const std::string &key, float height)
	{
		float width = height;
		const auto known = decoded.find(key);
		if (known != decoded.end() && known->second.height > 0)
			width = height * (float)known->second.width / (float)known->second.height;
		else if (images->failed(key) || missing.count(key) > 0)
			return; // broken asset: render nothing, keep the slot zero-width
		else if (!images->has_url(key)) {
			// The URL is not known yet (badge/emote metadata is still loading).
			// Remember the key so the panel is re-laid out as soon as it appears.
			waiting.insert(key);
			return;
		} else if (!pending.count(key)) {
			images->request(key);
			pending.insert(key);
		}

		Op op;
		op.kind = Op::Kind::Image;
		op.image_key = key;
		op.w = width;
		op.h = height;
		op.x = word.width;
		word.ops.push_back(std::move(op));
		word.width += width;
	}

	void append_text_word(Word &word, const std::wstring &text)
	{
		if (text.empty())
			return;
		Op op;
		op.kind = Op::Kind::Text;
		op.text = text;
		op.color = cfg.text_color;
		op.w = measure(text);
		op.x = word.width;
		word.ops.push_back(std::move(op));
		word.width += op.w;
	}

	void build_body_words(const ChatMessage &msg, std::vector<Word> &words)
	{
		if (msg.text.empty())
			return;
		const std::wstring body = utf8_to_wide(msg.text);

		std::vector<EmoteSpan> spans;
		if (cfg.twitch_emotes)
			spans = msg.emotes;
		std::sort(spans.begin(), spans.end(),
			  [](const EmoteSpan &l, const EmoteSpan &r) { return l.start_cp < r.start_cp; });

		std::vector<uint32_t> cp_to_u16;
		for (size_t i = 0; i < body.size();) {
			cp_to_u16.push_back((uint32_t)i);
			const wchar_t c = body[i];
			i += (c >= 0xD800 && c <= 0xDBFF && i + 1 < body.size()) ? 2 : 1;
		}
		cp_to_u16.push_back((uint32_t)body.size());
		const size_t total = cp_to_u16.size() - 1;

		Word current;
		std::wstring run;

		auto flush_text = [&]() {
			if (run.empty())
				return;
			append_text_word(current, run);
			run.clear();
		};

		auto flush_word = [&]() {
			flush_text();
			if (current.ops.empty())
				return;
			// A standalone word can itself be a BTTV/7TV emote.
			if (current.ops.size() == 1 && current.ops[0].kind == Op::Kind::Text) {
				const std::string name = wide_to_utf8(current.ops[0].text);
				std::string key;
				if ((cfg.bttv_emotes || cfg.seventv_emotes) &&
				    emotes.lookup(name, key)) {
					Word emote_word;
					add_image_op(emote_word, key, emote_height());
					if (!emote_word.ops.empty())
						words.push_back(std::move(emote_word));
					current = Word();
					return;
				}
			}
			words.push_back(std::move(current));
			current = Word();
		};

		size_t cp = 0;
		size_t span_index = 0;
		while (cp < total) {
			while (span_index < spans.size() && spans[span_index].end_cp < cp)
				++span_index;

			if (span_index < spans.size() && spans[span_index].start_cp == cp) {
				flush_text();
				add_image_op(current, "tw:" + spans[span_index].id, emote_height());
				cp = (size_t)spans[span_index].end_cp + 1;
				if (cp > total)
					cp = total;
				++span_index;
				continue;
			}

			const size_t begin = cp_to_u16[cp];
			const size_t end = cp_to_u16[cp + 1];
			const wchar_t c = body[begin];
			if (c == L' ' || c == L'\t' || c == 0x00A0) {
				flush_word();
				++cp;
				continue;
			}
			for (size_t k = begin; k < end; ++k)
				run.push_back(body[k]);
			++cp;
		}
		flush_word();
	}

	void layout_message(Message &item, float content_w)
	{
		const ChatMessage &msg = item.msg;
		item.lines.clear();
		if (!format)
			return;

		std::vector<Word> words;

		Word prefix;
		if (cfg.show_badges) {
			float badge_h = text_line_height() * 0.95f;
			for (size_t i = 0; i < msg.badges.size() && i < 4; ++i) {
				const std::string key =
					"badge:" + msg.badges[i].set_id + "/" + msg.badges[i].version;
				add_image_op(prefix, key, badge_h);
			}
			if (prefix.width > 0.0f) {
				Op gap;
				gap.kind = Op::Kind::Text;
				gap.text = L" ";
				gap.color = cfg.text_color;
				gap.w = text_line_height() * 0.18f;
				gap.x = prefix.width;
				prefix.ops.push_back(std::move(gap));
				prefix.width += gap.w;
			}
		}
		if (cfg.show_nick && !msg.display.empty()) {
			uint32_t nick = cfg.nick_color;
			uint32_t parsed = 0;
			if (!msg.color.empty() && parse_hex_color(msg.color, parsed))
				nick = parsed;

			Op op;
			op.kind = Op::Kind::Text;
			op.text = utf8_to_wide(msg.display);
			op.color = nick;
			op.w = measure(op.text);
			op.x = prefix.width;
			prefix.ops.push_back(std::move(op));
			prefix.width += op.w;

			if (!msg.action) {
				Op colon;
				colon.kind = Op::Kind::Text;
				colon.text = L":";
				colon.color = nick;
				colon.w = measure(colon.text);
				colon.x = prefix.width;
				prefix.ops.push_back(std::move(colon));
				prefix.width += colon.w;
			}
			Op gap;
			gap.kind = Op::Kind::Text;
			gap.text = L" ";
			gap.color = cfg.text_color;
			gap.w = space_width();
			gap.x = prefix.width;
			prefix.ops.push_back(std::move(gap));
			prefix.width += gap.w;
		}
		if (prefix.width > 0.0f)
			words.push_back(std::move(prefix));

		build_body_words(msg, words);

		const float content = content_w < 60.0f ? 60.0f : content_w;
		Line line;
		float x = 0.0f;
		for (Word &word : words) {
			if (word.width <= 0.0f)
				continue;
			const float gap = line.ops.empty() ? 0.0f : space_width();
			if (!line.ops.empty() && x + gap + word.width > content) {
				line.width = x;
				item.lines.push_back(std::move(line));
				line = Line();
				x = 0.0f;
			}
			if (!line.ops.empty())
				x += space_width();
			for (Op &op : word.ops) {
				op.x += x;
				line.ops.push_back(std::move(op));
			}
			x += word.width;
		}
		if (!line.ops.empty() || item.lines.empty()) {
			line.width = x;
			item.lines.push_back(std::move(line));
		}

		float factor = 0.0f;
		if (cfg.text_align == 1)
			factor = 0.5f;
		else if (cfg.text_align == 2)
			factor = 1.0f;
		if (factor > 0.0f) {
			for (Line &l : item.lines) {
				const float shift = (content - l.width) * factor;
				if (shift <= 0.0f)
					continue;
				for (Op &op : l.ops)
					op.x += shift;
			}
		}

		item.height = (float)item.lines.size() * line_advance();
	}

	int content_width() const
	{
		int available = (int)cfg.width - 2 * cfg.margin_x;
		if (available < 200)
			available = 200;
		int width = cfg.max_width;
		if (width > available)
			width = available;
		if (width < 120)
			width = 120;
		return width;
	}

	int available_height() const
	{
		int available = (int)cfg.height - 2 * cfg.margin_y;
		return available < 120 ? 120 : available;
	}

	void layout_all()
	{
		const float pad = (float)cfg.bg_padding;
		const float content = (float)content_width() - 2.0f * pad;
		const float spacing = (float)cfg.line_spacing;
		const int max_height = available_height();

		for (;;) {
			float total = 2.0f * pad;
			for (size_t i = 0; i < messages.size(); ++i) {
				layout_message(messages[i], content);
				total += messages[i].height;
				if (i + 1 < messages.size())
					total += spacing;
			}
			if (total <= (float)max_height || messages.size() <= 1)
				break;
			messages.erase(messages.begin());
		}

		panel_w = content_width();
		float total = 2.0f * pad;
		for (size_t i = 0; i < messages.size(); ++i) {
			total += messages[i].height;
			if (i + 1 < messages.size())
				total += spacing;
		}
		int height = (int)std::ceil(total);
		if (height < (int)(2.0f * pad) + 1)
			height = (int)(2.0f * pad) + 1;
		if (height > (int)cfg.height)
			height = (int)cfg.height;
		panel_h = height;
	}

	// ----------------------------------------------------------------- drawing

	void release_target()
	{
		bitmaps.clear();
		brush.Reset();
		rt.Reset();
		bitmap.Reset();
		tex_w = 0;
		tex_h = 0;
	}

	bool ensure_target(int width, int height)
	{
		if (rt && tex_w == width && tex_h == height)
			return true;

		release_target();

		if (FAILED(wic->CreateBitmap((UINT)width, (UINT)height, GUID_WICPixelFormat32bppPBGRA,
					     WICBitmapCacheOnLoad, bitmap.GetAddressOf())))
			return false;

		const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_SOFTWARE,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
			96.0f, 96.0f);
		if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, rt.GetAddressOf())))
			return false;

		rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
		rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
		if (FAILED(rt->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
						     brush.GetAddressOf())))
			return false;

		tex_w = width;
		tex_h = height;
		return true;
	}

	ComPtr<ID2D1Bitmap> bitmap_for(const std::string &key)
	{
		const auto cached = bitmaps.find(key);
		if (cached != bitmaps.end())
			return cached->second;
		const auto image = decoded.find(key);
		if (image == decoded.end() || image->second.width <= 0 || image->second.height <= 0)
			return nullptr;

		const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
		ComPtr<ID2D1Bitmap> bmp;
		const D2D1_SIZE_U size =
			D2D1::SizeU((UINT32)image->second.width, (UINT32)image->second.height);
		if (FAILED(rt->CreateBitmap(size, image->second.bgra.data(),
					    (UINT32)image->second.width * 4, props, bmp.GetAddressOf())))
			return nullptr;
		bitmaps[key] = bmp;
		return bmp;
	}

	void draw_text(const Op &op, float x, float y, float alpha)
	{
		if (op.text.empty())
			return;
		ComPtr<IDWriteTextLayout> layout;
		if (FAILED(dwrite->CreateTextLayout(op.text.c_str(), (UINT32)op.text.size(), format.Get(),
						    400000.0f, 400000.0f, layout.GetAddressOf())))
			return;

		if (cfg.outline_size > 0) {
			brush->SetColor(color_from(cfg.outline_color, alpha));
			const float step = (float)cfg.outline_size;
			static const float kOffsets[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0},
							     {1, 0},   {-1, 1}, {0, 1},  {1, 1}};
			for (const auto &offset : kOffsets) {
				rt->DrawTextLayout(D2D1::Point2F(x + offset[0] * step, y + offset[1] * step),
						   layout.Get(), brush.Get(),
						   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
			}
		}

		brush->SetColor(color_from(op.color, alpha));
		rt->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush.Get(),
				   D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
	}

	void draw_image(const Op &op, float x, float y, float alpha)
	{
		ComPtr<ID2D1Bitmap> bmp = bitmap_for(op.image_key);
		if (!bmp)
			return;
		rt->DrawBitmap(bmp.Get(), D2D1::RectF(x, y, x + op.w, y + op.h), alpha,
			       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, nullptr);
	}

	bool render_panel(RenderedPanel &panel)
	{
		const float pad = (float)cfg.bg_padding;
		if (!ensure_target(panel_w, panel_h))
			return false;

		rt->BeginDraw();
		rt->SetTransform(D2D1::Matrix3x2F::Identity());
		rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

		if (((cfg.bg_color >> 24) & 0xFF) > 0) {
			brush->SetColor(color_from(cfg.bg_color, 1.0f));
			const D2D1_ROUNDED_RECT rounded =
				D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, (float)panel_w - 0.5f,
							      (float)panel_h - 0.5f),
						  (float)cfg.bg_radius, (float)cfg.bg_radius);
			rt->FillRoundedRectangle(rounded, brush.Get());
		}

		const float line_height = line_advance();
		const float spacing = (float)cfg.line_spacing;
		float y = pad;
		for (size_t i = 0; i < messages.size(); ++i) {
			const Message &item = messages[i];
			float line_y = y;
			for (const Line &line : item.lines) {
				for (const Op &op : line.ops) {
					if (op.kind == Op::Kind::Text)
						draw_text(op, pad + op.x, line_y, item.alpha);
					else
						draw_image(op, pad + op.x,
							   line_y + (line_height - op.h) * 0.5f,
							   item.alpha);
				}
				line_y += line_height;
			}
			y += item.height;
			if (i + 1 < messages.size())
				y += spacing;
		}

		const HRESULT result = rt->EndDraw();
		if (result == D2DERR_RECREATE_TARGET) {
			release_target();
			return false;
		}
		if (FAILED(result))
			return false;

		WICRect rect = {0, 0, panel_w, panel_h};
		ComPtr<IWICBitmapLock> lock;
		if (FAILED(bitmap->Lock(&rect, WICBitmapLockRead, lock.GetAddressOf())))
			return false;
		UINT stride = 0;
		UINT bytes = 0;
		BYTE *data = nullptr;
		if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &data)))
			return false;

		panel.width = panel_w;
		panel.height = panel_h;
		panel.rgba.resize((size_t)panel_w * (size_t)panel_h * 4);
		for (int row = 0; row < panel_h; ++row) {
			const BYTE *src = data + (size_t)row * stride;
			uint8_t *dst = panel.rgba.data() + (size_t)row * (size_t)panel_w * 4;
			for (int col = 0; col < panel_w; ++col) {
				dst[0] = src[2];
				dst[1] = src[1];
				dst[2] = src[0];
				dst[3] = src[3];
				src += 4;
				dst += 4;
			}
		}

		const bool is_left = cfg.anchor == Anchor::BottomLeft || cfg.anchor == Anchor::TopLeft;
		const bool is_top = cfg.anchor == Anchor::TopLeft || cfg.anchor == Anchor::TopRight;
		const int left = is_left ? cfg.margin_x : (int)cfg.width - panel_w - cfg.margin_x;
		const int top = is_top ? cfg.margin_y : (int)cfg.height - panel_h - cfg.margin_y;
		panel.offset_x = left < 0 ? 0 : left;
		panel.offset_y = top < 0 ? 0 : top;
		return true;
	}

	bool drain_images()
	{
		bool added = false;

		// Promote keys whose URL became known after the previous layout pass
		// (for example when channel badges finish loading a few seconds later).
		for (auto it = waiting.begin(); it != waiting.end();) {
			if (images->failed(*it)) {
				missing.insert(*it);
				it = waiting.erase(it);
				added = true;
				continue;
			}
			if (images->has_url(*it)) {
				it = waiting.erase(it);
				added = true;
				continue;
			}
			++it;
		}

		for (auto it = pending.begin(); it != pending.end();) {
			DecodedImage image;
			if (images->take(*it, image)) {
				decoded[*it] = std::move(image);
				added = true;
				it = pending.erase(it);
				continue;
			}
			if (images->failed(*it)) {
				// The asset will never arrive; relayout so its placeholder
				// slot is removed instead of leaving a gap.
				missing.insert(*it);
				added = true;
				it = pending.erase(it);
				continue;
			}
			++it;
		}
		return added;
	}
};

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

ChatRenderer::ChatRenderer(const Config &cfg, EmoteRegistry &emotes,
			   std::shared_ptr<ImageCache> images)
	: impl_(new Impl(cfg, emotes, std::move(images)))
{
}

ChatRenderer::~ChatRenderer() = default;

void ChatRenderer::set_images(std::shared_ptr<ImageCache> images)
{
	impl_->images = std::move(images);
	impl_->pending.clear();
	impl_->missing.clear();
	impl_->waiting.clear();
	impl_->layout_dirty = true;
	impl_->draw_dirty = true;
}

void ChatRenderer::configure(const Config &cfg)
{
	const Config &old = impl_->cfg;
	const bool font_changed = cfg.font_face != old.font_face || cfg.font_size != old.font_size ||
				  cfg.bold != old.bold || cfg.italic != old.italic;

	const bool layout_affecting = font_changed || cfg.max_width != old.max_width ||
				      cfg.width != old.width || cfg.height != old.height ||
				      cfg.emote_scale != old.emote_scale ||
				      cfg.show_badges != old.show_badges ||
				      cfg.show_nick != old.show_nick ||
				      cfg.text_align != old.text_align ||
				      cfg.bg_padding != old.bg_padding ||
				      cfg.line_spacing != old.line_spacing ||
				      cfg.twitch_emotes != old.twitch_emotes ||
				      cfg.bttv_emotes != old.bttv_emotes ||
				      cfg.seventv_emotes != old.seventv_emotes ||
				      cfg.margin_x != old.margin_x || cfg.margin_y != old.margin_y ||
				      cfg.anchor != old.anchor;

	impl_->cfg = cfg;

	if (font_changed) {
		impl_->format.Reset();
		impl_->measure_cache.clear();
		impl_->line_h = 0.0f;
		impl_->space_w = 0.0f;
	}
	if (layout_affecting) {
		impl_->layout_dirty = true;
		impl_->draw_dirty = true;
	}

	// Colours, outline width/colour and the background radius do not affect the
	// layout, but they must repaint the cached panel.
	impl_->draw_dirty = true;
}

void ChatRenderer::add(const ChatMessage &msg)
{
	impl_->messages.push_back(Impl::Message());
	Impl::Message &item = impl_->messages.back();
	item.msg = msg;
	item.recv = msg.recv_time;
	item.alpha = 1.0f;
	impl_->layout_dirty = true;
	impl_->draw_dirty = true;
}

void ChatRenderer::tick(double now)
{
	bool changed = false;
	const double fade = (double)impl_->cfg.fade_ms / 1000.0;

	for (auto it = impl_->messages.begin(); it != impl_->messages.end();) {
		const double age = now - it->recv;
		if (impl_->cfg.ttl_seconds > 0) {
			const double ttl = (double)impl_->cfg.ttl_seconds;
			if (age >= ttl) {
				it = impl_->messages.erase(it);
				changed = true;
				continue;
			}
			float alpha = 1.0f;
			if (fade > 0.0 && age > ttl - fade)
				alpha = (float)((ttl - age) / fade);
			alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
			if (std::fabs(alpha - it->alpha) > 0.001f) {
				it->alpha = alpha;
				changed = true;
			}
		}
		++it;
	}

	if ((int)impl_->messages.size() > impl_->cfg.max_messages) {
		while ((int)impl_->messages.size() > impl_->cfg.max_messages) {
			impl_->messages.erase(impl_->messages.begin());
			changed = true;
		}
	}

	if (changed) {
		impl_->layout_dirty = true;
		impl_->draw_dirty = true;
	}
}

bool ChatRenderer::empty() const
{
	return impl_->messages.empty();
}

void ChatRenderer::clear()
{
	impl_->messages.clear();
	impl_->layout_dirty = true;
	impl_->draw_dirty = true;
}

bool ChatRenderer::build(double now, RenderedPanel &panel)
{
	(void)now;
	if (!impl_->init_gfx())
		return false;
	// The text format is reset when font settings change, so make sure it is
	// (re)created before laying anything out.
	if (!impl_->ensure_format())
		return false;

	if (impl_->drain_images())
		impl_->layout_dirty = true;

	if (impl_->messages.empty())
		return false;

	if (impl_->layout_dirty) {
		impl_->layout_all();
		impl_->layout_dirty = false;
		impl_->draw_dirty = true;
	}

	if (!impl_->draw_dirty)
		return false;

	if (!impl_->render_panel(panel)) {
		impl_->draw_dirty = true;
		return false;
	}

	impl_->draw_dirty = false;
	return true;
}

std::vector<std::string> ChatRenderer::font_families()
{
	std::vector<std::string> result;
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

	ComPtr<IDWriteFactory> factory;
	if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
				       reinterpret_cast<IUnknown **>(factory.GetAddressOf()))))
		return result;

	ComPtr<IDWriteFontCollection> collection;
	if (FAILED(factory->GetSystemFontCollection(collection.GetAddressOf())))
		return result;

	const UINT32 count = collection->GetFontFamilyCount();
	for (UINT32 i = 0; i < count && i < 400; ++i) {
		ComPtr<IDWriteFontFamily> family;
		if (FAILED(collection->GetFontFamily(i, family.GetAddressOf())))
			continue;
		ComPtr<IDWriteLocalizedStrings> names;
		if (FAILED(family->GetFamilyNames(names.GetAddressOf())))
			continue;
		UINT32 index = 0;
		BOOL exists = FALSE;
		names->FindLocaleName(L"en-us", &index, &exists);
		if (!exists)
			index = 0;
		UINT32 length = 0;
		if (FAILED(names->GetStringLength(index, &length)) || length == 0)
			continue;
		std::vector<wchar_t> buffer((size_t)length + 1, L'\0');
		if (FAILED(names->GetString(index, buffer.data(), length + 1)))
			continue;
		result.push_back(wide_to_utf8(std::wstring(buffer.data(), length)));
	}
	return result;
}

#pragma once

#include "chat.hpp"
#include "images.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct RenderedPanel {
	int width = 0;
	int height = 0;
	int offset_x = 0;
	int offset_y = 0;
	std::vector<uint8_t> rgba; // premultiplied RGBA, width * height * 4
};

// Lays out and rasterises the chat panel on the CPU (Direct2D software render
// target) and hands the result over as premultiplied RGBA pixels.
class ChatRenderer {
public:
	ChatRenderer(const Config &cfg, EmoteRegistry &emotes, std::shared_ptr<ImageCache> images);
	~ChatRenderer();

	ChatRenderer(const ChatRenderer &) = delete;
	ChatRenderer &operator=(const ChatRenderer &) = delete;

	void set_images(std::shared_ptr<ImageCache> images);
	void configure(const Config &cfg);
	void add(const ChatMessage &msg);
	void tick(double now);
	bool empty() const;
	void clear();

	// Rebuilds the panel when something changed. Returns true when `panel` was
	// refreshed and must be uploaded to the GPU.
	bool build(double now, RenderedPanel &panel);

	static std::vector<std::string> font_families();

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

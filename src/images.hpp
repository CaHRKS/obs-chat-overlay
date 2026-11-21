#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct DecodedImage {
	int width = 0;
	int height = 0;
	std::vector<uint8_t> bgra; // premultiplied BGRA, width * height * 4 bytes
};

// Decodes PNG/JPEG/GIF (first frame) through WIC. Safe to call from any thread
// that initialised COM.
bool decode_image_bytes(const std::vector<uint8_t> &bytes, DecodedImage &out);

// Downloads images on worker threads and hands the decoded result to the
// render thread. Keys are stable strings such as "tw:25" or "badge:subscriber/12".
//
// Worker threads only reference an internal shared state, never the ImageCache
// object itself, so a slow download can never touch freed memory during
// shutdown.
class ImageCache {
public:
	explicit ImageCache(int workers = 2, std::string proxy = std::string());
	~ImageCache();

	ImageCache(const ImageCache &) = delete;
	ImageCache &operator=(const ImageCache &) = delete;

	// Registers where a key can be downloaded from. Safe to call repeatedly.
	void register_image(const std::string &key, const std::string &url);
	bool has_url(const std::string &key) const;

	// Queues a download for a registered key (no-op when already queued/done).
	void request(const std::string &key);

	// Pops a finished download. Returns false when nothing is ready.
	bool take(const std::string &key, DecodedImage &out);
	// True when the key failed permanently (404, decode error, ...).
	bool failed(const std::string &key) const;

	void stop();

private:
	struct State;
	static void worker_main(std::shared_ptr<State> state);

	std::shared_ptr<State> state_;
	std::vector<std::thread> threads_;
};

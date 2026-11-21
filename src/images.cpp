#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "images.hpp"
#include "net.hpp"

#include <obs-module.h>

#include <windows.h>
#include <objbase.h>
#include <wincodec.h>

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>

namespace {

void release(IUnknown *object)
{
	if (object)
		object->Release();
}

bool wic_decode(const std::vector<uint8_t> &bytes, DecodedImage &out)
{
	if (bytes.empty())
		return false;

	IWICImagingFactory *factory = nullptr;
	if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
				    IID_PPV_ARGS(&factory))))
		return false;

	bool ok = false;
	IStream *stream = nullptr;
	IWICBitmapDecoder *decoder = nullptr;
	IWICBitmapFrameDecode *frame = nullptr;
	IWICFormatConverter *converter = nullptr;

	do {
		if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)))
			break;
		ULONG written = 0;
		if (FAILED(stream->Write(bytes.data(), (ULONG)bytes.size(), &written)))
			break;
		LARGE_INTEGER zero;
		zero.QuadPart = 0;
		if (FAILED(stream->Seek(zero, STREAM_SEEK_SET, nullptr)))
			break;

		if (FAILED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand,
							    &decoder)))
			break;
		if (FAILED(decoder->GetFrame(0, &frame)))
			break;
		if (FAILED(factory->CreateFormatConverter(&converter)))
			break;
		if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
						 WICBitmapDitherTypeNone, nullptr, 0.0,
						 WICBitmapPaletteTypeCustom)))
			break;

		UINT width = 0;
		UINT height = 0;
		if (FAILED(converter->GetSize(&width, &height)))
			break;
		if (width == 0 || height == 0 || width > 4096 || height > 4096)
			break;

		out.width = (int)width;
		out.height = (int)height;
		out.bgra.resize((size_t)width * height * 4);
		if (FAILED(converter->CopyPixels(nullptr, width * 4, (UINT)out.bgra.size(),
						 out.bgra.data())))
			break;
		ok = true;
	} while (false);

	release(converter);
	release(frame);
	release(decoder);
	release(stream);
	release(factory);
	return ok;
}

} // namespace

bool decode_image_bytes(const std::vector<uint8_t> &bytes, DecodedImage &out)
{
	return wic_decode(bytes, out);
}

// ---------------------------------------------------------------------------

struct ImageCache::State {
	struct Entry {
		std::string url;
		bool queued = false;
		bool in_flight = false;
		bool done = false;
		bool error = false;
	};

	std::mutex mutex;
	std::map<std::string, Entry> entries;
	std::map<std::string, DecodedImage> ready;
	std::string proxy;
	bool stopping = false;
	std::atomic<bool> debug{false};
	std::atomic<int> active{0};
};

ImageCache::ImageCache(int workers, std::string proxy) : state_(std::make_shared<State>())
{
	state_->proxy = std::move(proxy);
	const int count = workers < 1 ? 1 : workers;
	for (int i = 0; i < count; ++i) {
		std::shared_ptr<State> state = state_;
		threads_.emplace_back([state]() { ImageCache::worker_main(state); });
	}
}

ImageCache::~ImageCache()
{
	stop();
}

void ImageCache::stop()
{
	if (threads_.empty())
		return;
	{
		std::lock_guard<std::mutex> lock(state_->mutex);
		state_->stopping = true;
	}

	const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1200);
	while (std::chrono::steady_clock::now() < deadline) {
		if (state_->active.load() == 0)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}

	const bool finished = state_->active.load() == 0;
	for (std::thread &thread : threads_) {
		if (!thread.joinable())
			continue;
		if (finished)
			thread.join();
		else
			thread.detach();
	}
	threads_.clear();
}

void ImageCache::register_image(const std::string &key, const std::string &url)
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	State::Entry &entry = state_->entries[key];
	if (entry.url.empty())
		entry.url = url;
}

bool ImageCache::has_url(const std::string &key) const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	const auto it = state_->entries.find(key);
	return it != state_->entries.end() && !it->second.url.empty();
}

bool ImageCache::url_for(const std::string &key, std::string &out) const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	const auto it = state_->entries.find(key);
	if (it == state_->entries.end() || it->second.url.empty())
		return false;
	out = it->second.url;
	return true;
}

void ImageCache::request(const std::string &key)
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	const auto it = state_->entries.find(key);
	if (it == state_->entries.end() || it->second.url.empty())
		return;
	if (it->second.queued || it->second.in_flight || it->second.done || it->second.error)
		return;
	it->second.queued = true;
}

bool ImageCache::take(const std::string &key, DecodedImage &out)
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	const auto it = state_->ready.find(key);
	if (it == state_->ready.end())
		return false;
	out = std::move(it->second);
	state_->ready.erase(it);
	return true;
}

bool ImageCache::failed(const std::string &key) const
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	const auto it = state_->entries.find(key);
	return it != state_->entries.end() && it->second.error;
}

void ImageCache::set_debug(bool enabled)
{
	state_->debug = enabled;
}

void ImageCache::clear()
{
	std::lock_guard<std::mutex> lock(state_->mutex);
	state_->entries.clear();
	state_->ready.clear();
}

void ImageCache::worker_main(std::shared_ptr<State> state)
{
	state->active.fetch_add(1);
	CoInitializeEx(nullptr, COINIT_MULTITHREADED);

	for (;;) {
		std::string key;
		std::string url;
		{
			std::unique_lock<std::mutex> lock(state->mutex);
			if (state->stopping)
				break;
			for (auto &entry : state->entries) {
				if (entry.second.queued && !entry.second.in_flight) {
					entry.second.queued = false;
					entry.second.in_flight = true;
					key = entry.first;
					url = entry.second.url;
					break;
				}
			}
		}

		if (key.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(40));
			continue;
		}

		HttpResult response = http_get(url, {}, state->proxy);
		DecodedImage image;
		bool ok = false;
		if (response.ok && !response.body.empty()) {
			const std::vector<uint8_t> bytes(response.body.begin(), response.body.end());
			ok = decode_image_bytes(bytes, image);
		}

		if (!ok && state->debug.load()) {
			blog(LOG_WARNING, "[chat-overlay] image failed: %s (%s) status=%ld", key.c_str(),
			     url.c_str(), response.status);
		}

		{
			std::lock_guard<std::mutex> lock(state->mutex);
			auto it = state->entries.find(key);
			if (it != state->entries.end()) {
				it->second.in_flight = false;
				it->second.done = ok;
				it->second.error = !ok;
			}
			if (ok)
				state->ready[key] = std::move(image);
		}
	}

	CoUninitialize();
	state->active.fetch_sub(1);
}

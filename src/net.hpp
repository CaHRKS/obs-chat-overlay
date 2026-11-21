#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct HttpResult {
	bool ok = false;
	long status = 0;
	std::string body;
	std::string error;
};

HttpResult http_get(const std::string &url,
		    const std::vector<std::pair<std::string, std::string>> &headers = {},
		    const std::string &proxy = std::string());

// Minimal WebSocket client on top of WinHTTP, used for Twitch IRC over
// websockets. All methods are safe to call from a single worker thread;
// close() may be called from another thread to unblock receive_text().
class WebSocket {
public:
	WebSocket() = default;
	~WebSocket();

	WebSocket(const WebSocket &) = delete;
	WebSocket &operator=(const WebSocket &) = delete;

	bool connect(const std::string &url,
		     const std::vector<std::pair<std::string, std::string>> &headers,
		     const std::string &proxy, std::string *error);
	void close();
	// Closes the underlying handles without a websocket close handshake. Safe to
	// call from another thread to cancel a blocking receive_text().
	void abort();
	bool is_open() const { return socket_ != nullptr; }

	bool send_text(const std::string &text);
	// Blocks until a full text message is received. Returns false when the
	// connection is closed, fails, or close() is called from another thread.
	bool receive_text(std::string &out, std::string *error);

private:
	void *session_ = nullptr;
	void *connection_ = nullptr;
	void *socket_ = nullptr;
};

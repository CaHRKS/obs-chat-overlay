#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Minimal HTTP server bound to 127.0.0.1. Serves one HTML page at "/" and an
// SSE stream at "/events". Used to feed the embedded browser source with chat
// events so third-party widgets (StreamElements HTML/CSS/JS) can run unmodified.
class HttpServer {
public:
	HttpServer() = default;
	~HttpServer();

	HttpServer(const HttpServer &) = delete;
	HttpServer &operator=(const HttpServer &) = delete;

	bool start(std::string *error);
	void stop();
	bool running() const { return running_.load(); }
	int port() const { return port_; }

	void set_page(std::string html);
	void broadcast(const std::string &json);
	size_t client_count() const;
	// Receives "/log?m=..." requests from the widget page (console.errors etc.).
	void set_logger(std::function<void(const std::string &)> logger);

private:
	void accept_loop();
	void handle_connection(unsigned long long socket);

	std::atomic<bool> running_{false};
	int port_ = 0;
	unsigned long long listen_socket_ = 0;

	mutable std::mutex mutex_;
	std::string page_;
	std::vector<unsigned long long> clients_;
	std::function<void(const std::string &)> logger_;

	std::thread thread_;
};

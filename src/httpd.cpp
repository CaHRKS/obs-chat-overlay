#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "httpd.hpp"

// winsock2.h must be included before windows.h (which obs-module.h pulls in),
// otherwise the old winsock.h wins and the build fails on conflicting types.
#include <winsock2.h>
#include <ws2tcpip.h>

#include <obs-module.h>
#include <windows.h>

#include <cstddef>
#include <cstring>

namespace {

constexpr int kMaxRequest = 8192;

void close_socket(unsigned long long handle)
{
	if (handle)
		closesocket((SOCKET)handle);
}

bool send_all(SOCKET socket, const char *data, size_t length)
{
	size_t sent = 0;
	while (sent < length) {
		const int result = ::send(socket, data + sent, (int)(length - sent), 0);
		if (result <= 0)
			return false;
		sent += (size_t)result;
	}
	return true;
}

bool send_string(SOCKET socket, const std::string &text)
{
	return send_all(socket, text.c_str(), text.size());
}

} // namespace

HttpServer::~HttpServer()
{
	stop();
}

bool HttpServer::start(std::string *error)
{
	if (running_.load())
		return true;

	WSADATA data;
	if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
		if (error)
			*error = "WSAStartup failed";
		return false;
	}

	const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) {
		if (error)
			*error = "socket() failed";
		return false;
	}

	BOOL reuse = TRUE;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&reuse, sizeof(reuse));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = 0; // ephemeral port
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (bind(listener, (const sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		if (error)
			*error = "bind() failed";
		closesocket(listener);
		return false;
	}
	if (listen(listener, 8) == SOCKET_ERROR) {
		if (error)
			*error = "listen() failed";
		closesocket(listener);
		return false;
	}

	sockaddr_in bound = {};
	int bound_length = sizeof(bound);
	if (getsockname(listener, (sockaddr *)&bound, &bound_length) == SOCKET_ERROR) {
		if (error)
			*error = "getsockname() failed";
		closesocket(listener);
		return false;
	}

	port_ = (int)ntohs(bound.sin_port);
	listen_socket_ = (unsigned long long)listener;
	running_ = true;
	thread_ = std::thread([this]() { accept_loop(); });
	return true;
}

void HttpServer::stop()
{
	if (!running_.exchange(false))
		return;

	// Closing the listener unblocks accept().
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (unsigned long long client : clients_)
			close_socket(client);
		clients_.clear();
	}
	close_socket(listen_socket_);
	listen_socket_ = 0;

	if (thread_.joinable())
		thread_.join();
}

void HttpServer::set_page(std::string html)
{
	std::lock_guard<std::mutex> lock(mutex_);
	page_ = std::move(html);
}

void HttpServer::broadcast(const std::string &json)
{
	const std::string frame = "data: " + json + "\n\n";

	std::lock_guard<std::mutex> lock(mutex_);
	for (size_t i = 0; i < clients_.size();) {
		if (send_string((SOCKET)clients_[i], frame)) {
			++i;
			continue;
		}
		close_socket(clients_[i]);
		clients_.erase(clients_.begin() + (ptrdiff_t)i);
	}
}

size_t HttpServer::client_count() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return clients_.size();
}

void HttpServer::set_logger(std::function<void(const std::string &)> logger)
{
	std::lock_guard<std::mutex> lock(mutex_);
	logger_ = std::move(logger);
}

bool HttpServer::self_test(std::string *detail)
{
	if (!running_ || !port_) {
		if (detail)
			*detail = "server is not running";
		return false;
	}

	const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (probe == INVALID_SOCKET) {
		if (detail)
			*detail = "socket() failed";
		return false;
	}

	DWORD timeout = 2500;
	setsockopt(probe, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
	setsockopt(probe, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

	sockaddr_in address = {};
	address.sin_family = AF_INET;
	address.sin_port = htons((unsigned short)port_);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(probe, (const sockaddr *)&address, sizeof(address)) == SOCKET_ERROR) {
		if (detail)
			*detail = "connect failed, error " + std::to_string(WSAGetLastError());
		closesocket(probe);
		return false;
	}

	const char request[] = "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
	send(probe, request, (int)(sizeof(request) - 1), 0);

	char buffer[512];
	const int received = recv(probe, buffer, sizeof(buffer), 0);
	closesocket(probe);

	if (received <= 0) {
		if (detail)
			*detail = "no response from server";
		return false;
	}

	const std::string response(buffer, (size_t)received);
	if (response.rfind("HTTP/1.1 200", 0) != 0) {
		if (detail)
			*detail = "unexpected response: " + response.substr(0, 48);
		return false;
	}
	if (response.find("<!DOCTYPE html") == std::string::npos) {
		if (detail)
			*detail = "page body is missing";
		return false;
	}
	return true;
}

void HttpServer::accept_loop()
{
	while (running_.load()) {
		const SOCKET client = accept((SOCKET)listen_socket_, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (!running_.load())
				break;
			continue;
		}

		// Browsers open speculative connections that never send a request. Without
		// a timeout the accept loop would block in recv() forever and the server
		// would stop answering real requests.
		DWORD timeout = 1500;
		setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
		setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));

		handle_connection((unsigned long long)client);
	}
}

void HttpServer::handle_connection(unsigned long long handle)
{
	const SOCKET client = (SOCKET)handle;

	// Read the whole request head. Closing a socket while headers are still
	// unread makes Windows send RST instead of FIN, and the browser reports
	// ERR_CONNECTION_RESET even though a response was written.
	std::string request;
	char buffer[1024];
	while (request.size() < kMaxRequest) {
		if (request.find("\r\n\r\n") != std::string::npos)
			break;
		const int received = recv(client, buffer, sizeof(buffer), 0);
		if (received <= 0)
			break;
		request.append(buffer, (size_t)received);
	}

	if (request.empty()) {
		// Speculative connection or a client that hung up: nothing to answer.
		close_socket(handle);
		return;
	}

	const size_t line_end = request.find("\r\n");
	const std::string request_line = request.substr(0, line_end);
	const size_t first_space = request_line.find(' ');
	const size_t second_space =
		first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
	std::string raw_path = (first_space == std::string::npos || second_space == std::string::npos)
				       ? std::string("/")
				       : request_line.substr(first_space + 1,
							     second_space - first_space - 1);
	// The page is requested as "/?v=<timestamp>", so routing uses the path
	// without the query; "/log" still needs the raw value to read ?m=...
	std::string path = raw_path;
	const size_t question = path.find('?');
	if (question != std::string::npos)
		path.resize(question);

	if (path == "/log") {
		const size_t marker = raw_path.find("?m=");
		if (marker != std::string::npos) {
			std::string message = raw_path.substr(marker + 3);
			// Minimal percent-decoding for the values we send ourselves.
			std::string decoded;
			decoded.reserve(message.size());
			for (size_t i = 0; i < message.size(); ++i) {
				if (message[i] == '%' && i + 2 < message.size()) {
					const auto hex = [](char c) -> int {
						if (c >= '0' && c <= '9')
							return c - '0';
						if (c >= 'a' && c <= 'f')
							return c - 'a' + 10;
						if (c >= 'A' && c <= 'F')
							return c - 'A' + 10;
						return -1;
					};
					const int hi = hex(message[i + 1]);
					const int lo = hex(message[i + 2]);
					if (hi >= 0 && lo >= 0) {
						decoded.push_back((char)((hi << 4) | lo));
						i += 2;
						continue;
					}
				}
				decoded.push_back(message[i] == '+' ? ' ' : message[i]);
			}
			if (decoded.size() > 800)
				decoded.resize(800);
			std::function<void(const std::string &)> logger;
			{
				std::lock_guard<std::mutex> lock(mutex_);
				logger = logger_;
			}
			if (logger)
				logger(decoded);
		}
		const std::string response = "HTTP/1.1 204 No Content\r\n"
					     "Access-Control-Allow-Origin: *\r\n"
					     "Connection: close\r\n\r\n";
		send_string(client, response);
		close_socket(handle);
		return;
	}

	if (path == "/events") {
		const std::string headers =
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/event-stream; charset=utf-8\r\n"
			"Cache-Control: no-cache, no-store\r\n"
			"Connection: keep-alive\r\n"
			"Access-Control-Allow-Origin: *\r\n"
			"\r\n"
			": connected\n\n";
		if (!send_string(client, headers)) {
			close_socket(handle);
			return;
		}
		std::lock_guard<std::mutex> lock(mutex_);
		clients_.push_back(handle);
		blog(LOG_DEBUG, "[chat-overlay] httpd: event stream client connected (~%zu total)",
		     clients_.size());
		return;
	}

	const bool is_page = path == "/" || path == "/index.html";
	std::string body;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		body = page_;
	}
	if (!is_page)
		body = "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>";

	const std::string response =
		std::string(is_page ? "HTTP/1.1 200 OK" : "HTTP/1.1 404 Not Found") +
		"\r\n"
		"Content-Type: text/html; charset=utf-8\r\n"
		"Cache-Control: no-cache, no-store\r\n"
		"Connection: close\r\n"
		"Content-Length: " +
		std::to_string(body.size()) + "\r\n\r\n" + body;
	send_string(client, response);
	shutdown(client, SD_SEND);
	close_socket(handle);
	blog(LOG_DEBUG, "[chat-overlay] httpd: served %s (%zu bytes)", path.c_str(), body.size());
}

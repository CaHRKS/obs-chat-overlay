#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "httpd.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

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

void HttpServer::accept_loop()
{
	while (running_.load()) {
		const SOCKET client = accept((SOCKET)listen_socket_, nullptr, nullptr);
		if (client == INVALID_SOCKET) {
			if (!running_.load())
				break;
			continue;
		}
		handle_connection((unsigned long long)client);
	}
}

void HttpServer::handle_connection(unsigned long long handle)
{
	const SOCKET client = (SOCKET)handle;

	// Read the request head (we only need the request line).
	std::string request;
	char buffer[512];
	while (request.size() < kMaxRequest) {
		const int received = recv(client, buffer, sizeof(buffer), 0);
		if (received <= 0)
			break;
		request.append(buffer, (size_t)received);
		if (request.find("\r\n") != std::string::npos)
			break;
	}

	const size_t line_end = request.find("\r\n");
	const std::string request_line = request.substr(0, line_end);
	const size_t first_space = request_line.find(' ');
	const size_t second_space =
		first_space == std::string::npos ? std::string::npos : request_line.find(' ', first_space + 1);
	const std::string path = (first_space == std::string::npos || second_space == std::string::npos)
					 ? std::string("/")
					 : request_line.substr(first_space + 1,
							       second_space - first_space - 1);

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
		return;
	}

	std::string body;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		body = page_;
	}
	if (path != "/" && path != "/index.html") {
		body = "<!DOCTYPE html><html><body>404</body></html>";
	}

	const std::string response = "HTTP/1.1 200 OK\r\n"
				     "Content-Type: text/html; charset=utf-8\r\n"
				     "Cache-Control: no-cache, no-store\r\n"
				     "Connection: close\r\n"
				     "Content-Length: " +
				     std::to_string(body.size()) + "\r\n\r\n" + body;
	send_string(client, response);
	close_socket(handle);
}

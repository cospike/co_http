#include "file_utils.hpp"
#include "http_server.hpp"
#include "io_context.hpp"
#include "reflect.hpp"
#include <cassert>
#include <cstdio>
#include <iostream>
#include <unistd.h>

using namespace std::chrono_literals;

// C++ 全栈，聊天服务器
// 1. AJAX，轮询与长轮询 (OK)
// 2. WebSocket，JSON 消息

struct Message {
	std::string user;
	std::string type;
	std::string content;

	REFLECT(user, type, content);
};

struct RecvParams {
	uint32_t first;

	REFLECT(first);
};

std::vector<Message> messages;
stop_source recv_timeout_stop = stop_source::make();

void server() {
	io_context ctx;
	// chdir("../static");
	chdir("..");
	messages.push_back({"系统", "text", "你好，欢迎来到聊天室"});
	auto server = http_server::make();
	server->get_router().route("/", [](http_server::http_request &request) {
		std::string response = file_get_content("./static/index.html");
		request.write_response(200, response, "text/html");
	});
	server->get_router().route(
		"/favicon.ico", [](http_server::http_request &request) {
			std::string response = file_get_content("./static/favicon.ico");
			request.write_response(200, response, "image/x-icon");
		});
	server->get_router().route("/send", [](http_server::http_request &request) {
		// fmt::println("/send 收到了 {}", request.body);
		messages.push_back(reflect::json_decode<Message>(request.body));
		recv_timeout_stop.request_stop();
		recv_timeout_stop = stop_source::make();
		request.write_response(200, "OK");
	});
	server->get_router().route(
		"/upload", [&server](http_server::http_request &request) {
			// fmt::println("/send 收到了 {}", request.body);
			auto multipart = request.parts;
			assert(!multipart.empty());
			std::string user;
			std::string type;
			std::string filename;
			std::string file_type;
			std::string filepath_to_save;
			std::string filepath;
			for (auto &part : multipart) {
				if (part.name == "user" && !part.filename) {
					user = std::string(part.data);
				} else if (part.name == "type" && !part.filename) {
					type = std::string(part.data);
				} else if (part.name == "image" && part.filename) {
					const std::string &original_filename = *part.filename;
					file_type = part.content_type;
					filepath_to_save = "./uploads/" + original_filename;
					filepath = "/uploads/" + original_filename;

					std::ofstream output_file(filepath_to_save,
											  std::ios::binary);

					if (!output_file) {
						std::cerr << "Error: Could not open file for writing: "
								  << filepath_to_save << std::endl;
						continue; // 继续处理下一个 part
					}

					output_file.write(part.data.data(), part.data.size());

					std::cout << "Saved file '" << original_filename << "' to '"
							  << filepath_to_save << "' (" << part.data.size()
							  << " bytes)." << std::endl;
					server->get_router().route(
						filepath, [filepath_to_save, file_type](
									  http_server::http_request &request) {
							std::string response =
								file_get_content(filepath_to_save);
							request.write_response(200, response, file_type);
						});
				}
			}
			messages.push_back({user, type, filepath});
			recv_timeout_stop.request_stop();
			recv_timeout_stop = stop_source::make();
			request.write_response(200, "OK");
		});
	server->get_router().route("/recv", [](http_server::http_request &request) {
		auto params = reflect::json_decode<RecvParams>(request.body);
		if (messages.size() > params.first) {
			std::vector<Message> submessages(messages.begin() + params.first,
											 messages.end());
			std::string response = reflect::json_encode(submessages);
			// fmt::println("/recv 立即返回 {}", response);
			request.write_response(200, response);
		} else {
			io_context::get().set_timeout(
				3s,
				[&request, params] {
					std::vector<Message> submessages;
					if (messages.size() > params.first) {
						submessages.assign(messages.begin() + params.first,
										   messages.end());
					}
					std::string response = reflect::json_encode(submessages);
					// fmt::println("/recv 延迟返回 {}", response);
					request.write_response(200, response);
				},
				recv_timeout_stop);
		}
	});
	// fmt::println("正在监听：http://0.0.0.0:8080");
	server->do_start("0.0.0.0", "28080");

	ctx.join();
}

int main() {
	// try {
	server();
	// } catch (std::system_error const &e) {
	// fmt::println("{} ({}/{})", e.what(), e.code().category().name(),
	// 			 e.code().value());
	// std::cout << e.what() << " " << e.code().category().name() << ":"
	// 		  << e.code().value() << "\n";
	// }
	return 0;
}

#pragma once

#include "bytes_buffer.hpp"
#include "enum_magic.hpp"
#include "string_map.hpp"
#include <cassert>
#include <optional>

struct http11_header_parser {
	bytes_buffer m_header;	  // "GET / HTTP/1.1\nHost: 142857.red\r\nAccept:
							  // */*\r\nConnection: close"
	std::string m_headline;	  // "GET / HTTP/1.1"
	string_map m_header_keys; // {"Host": "142857.red", "Accept": "*/*",
							  // "Connection: close"}
	std::string m_body;		  // 不小心超量读取的正文（如果有的话）
	bool m_header_finished = false;

	void reset_state() {
		m_header.clear();
		m_headline.clear();
		m_header_keys.clear();
		m_body.clear();
		m_header_finished = 0;
	}

	[[nodiscard]] bool header_finished() {
		return m_header_finished; // 如果正文都结束了，就不再需要更多数据
	}

	void _extract_headers() {
		std::string_view header = m_header;
		size_t pos = header.find("\r\n", 0, 2);
		m_headline = std::string(header.substr(0, pos));
		while (pos != std::string::npos) {
			// 跳过 "\r\n"
			pos += 2;
			// 从当前位置开始找，先找到下一行位置（可能为 npos）
			size_t next_pos = header.find("\r\n", pos, 2);
			size_t line_len = std::string::npos;
			if (next_pos != std::string::npos) {
				// 如果下一行还不是结束，那么 line_len
				// 设为本行开始到下一行之间的距离
				line_len = next_pos - pos;
			}
			// 就能切下本行
			std::string_view line = header.substr(pos, line_len);
			size_t colon = line.find(": ", 0, 2);
			if (colon != std::string::npos) {
				// 每一行都是 "键: 值"
				std::string key = std::string(line.substr(0, colon));
				std::string_view value = line.substr(colon + 2);
				// 键统一转成小写，实现大小写不敏感的判断
				for (char &c : key) {
					if ('A' <= c && c <= 'Z') {
						c += 'a' - 'A';
					}
				}
				// 古代 C++ 过时的写法：m_header_keys[key] = value;
				// 现代 C++17 的高效写法：
				m_header_keys.insert_or_assign(std::move(key), value);
			}
			pos = next_pos;
		}
	}

	void push_chunk(bytes_const_view chunk) {
		assert(!m_header_finished);
		size_t old_size = m_header.size();
		m_header.append(chunk);
		std::string_view header = m_header;
		// 如果还在解析头部的话，尝试判断头部是否结束
		if (old_size < 4) {
			old_size = 4;
		}
		old_size -= 4;
		size_t header_len = header.find("\r\n\r\n", old_size, 4);
		if (header_len != std::string::npos) {
			// 头部已经结束
			m_header_finished = true;
			// 把不小心多读取的正文留下
			m_body = header.substr(header_len + 4);
			m_header.resize(header_len);
			// 开始分析头部，尝试提取 Content-length 字段
			_extract_headers();
		}
	}

	std::string &headline() { return m_headline; }

	string_map &headers() { return m_header_keys; }

	bytes_buffer &headers_raw() { return m_header; }

	std::string &extra_body() { return m_body; }
};

template <class HeaderParser = http11_header_parser> struct _http_base_parser {
	HeaderParser m_header_parser;
	size_t m_content_length = 0;
	size_t body_accumulated_size = 0;
	bool m_body_finished = false;

	virtual ~_http_base_parser() = default;
	_http_base_parser &operator=(_http_base_parser &&that) = default;

	void reset_state() {
		m_header_parser.reset_state();
		m_content_length = 0;
		body_accumulated_size = 0;
		m_body_finished = false;
	}

	[[nodiscard]] bool header_finished() {
		return m_header_parser.header_finished();
	}

	[[nodiscard]] bool request_finished() { return m_body_finished; }

	std::string &headers_raw() { return m_header_parser.headers_raw(); }

	std::string &headline() { return m_header_parser.headline(); }

	string_map &headers() { return m_header_parser.headers(); }

	std::string _headline_first() {
		// "GET / HTTP/1.1" request
		// "HTTP/1.1 200 OK" response
		auto &line = headline();
		size_t space = line.find(' ');
		if (space == std::string::npos) {
			return "";
		}
		return line.substr(0, space);
	}

	std::string _headline_second() {
		// "GET / HTTP/1.1"
		auto &line = headline();
		size_t space1 = line.find(' ');
		if (space1 == std::string::npos) {
			return "";
		}
		size_t space2 = line.find(' ', space1 + 1);
		if (space2 == std::string::npos) {
			return "";
		}
		return line.substr(space1 + 1, space2 - (space1 + 1));
	}

	std::string _headline_third() {
		// "GET / HTTP/1.1"
		auto &line = headline();
		size_t space1 = line.find(' ');
		if (space1 == std::string::npos) {
			return "";
		}
		size_t space2 = line.find(' ', space1 + 1);
		if (space2 == std::string::npos) {
			return "";
		}
		return line.substr(space2);
	}

	std::string &body() { return m_header_parser.extra_body(); }

	size_t _extract_content_length() {
		auto &headers = m_header_parser.headers();
		auto it = headers.find("content-length");
		if (it == headers.end()) {
			return 0;
		}
		try {
			return std::stoi(it->second);
		} catch (std::logic_error const &) {
			return 0;
		}
	}

	void virtual push_chunk(bytes_const_view chunk) {
		assert(!m_body_finished);
		if (!m_header_parser.header_finished()) {
			m_header_parser.push_chunk(chunk);
			if (m_header_parser.header_finished()) {
				body_accumulated_size = body().size();
				m_content_length = _extract_content_length();
				if (body_accumulated_size >= m_content_length) {
					m_body_finished = true;
				}
			}
		} else {
			body().append(chunk);
			body_accumulated_size += chunk.size();
			if (body_accumulated_size >= m_content_length) {
				m_body_finished = true;
			}
		}
	}

	std::string read_some_body() { return std::move(body()); }
};

template <class HeaderParser = http11_header_parser>
struct HttpMultipartParser : public _http_base_parser<HeaderParser> {
  public:
	using Base = _http_base_parser<HeaderParser>;
	using Base::body;
	using Base::headers;

	struct MultipartPart {
		std::string name;
		std::optional<std::string> filename;
		std::string content_type;
		bytes_const_view data; // 使用您自定义的视图类型，实现零拷贝
	};

  private:
	std::vector<MultipartPart> m_parts;
	bool m_multipart_parsed = false;

	// 解析 part 内部的头部 (Content-Disposition, Content-Type)
	void _parse_part_headers(bytes_const_view part_header,
							 MultipartPart &part) {
		std::string_view header_sv = part_header;
		size_t pos = 0;
		while (pos < header_sv.size()) {
			size_t next_pos = header_sv.find("\r\n", pos);
			std::string_view line = header_sv.substr(
				pos, (next_pos == std::string::npos) ? std::string::npos
													 : next_pos - pos);
			if (std::string_view(line).rfind("Content-Disposition:", 0) == 0) {
				std::string_view cd_value = line.substr(line.find(':') + 1);
				cd_value.remove_prefix(cd_value.find_first_not_of(" \t"));
				size_t start = 0;
				while (start < cd_value.size()) {
					size_t end = cd_value.find(';', start);
					std::string_view param_part =
						cd_value.substr(start, (end == std::string_view::npos)
												   ? std::string_view::npos
												   : end - start);
					param_part.remove_prefix(
						std::min(param_part.find_first_not_of(" \t"),
								 param_part.size()));
					size_t eq_pos = param_part.find('=');
					if (eq_pos != std::string_view::npos) {
						std::string_view key = param_part.substr(0, eq_pos);
						std::string_view value = param_part.substr(eq_pos + 1);
						if (value.size() >= 2 && value.front() == '"' &&
							value.back() == '"')
							value = value.substr(1, value.size() - 2);
						if (key == "name")
							part.name = std::string(value);
						else if (key == "filename")
							part.filename = std::string(value);
					}
					if (end == std::string_view::npos)
						break;
					start = end + 1;
				}
			} else if (std::string_view(line).rfind("Content-Type:", 0) == 0) {
				std::string_view ct_value = line.substr(line.find(':') + 1);
				ct_value.remove_prefix(ct_value.find_first_not_of(" \t"));
				part.content_type = std::string(ct_value);
			}
			if (next_pos == std::string_view::npos)
				break;
			pos = next_pos + 2;
		}
	}

	void _parse_multipart_body() {
		auto it = headers().find("content-type");
		if (it == headers().end() ||
			std::string_view(it->second).rfind("multipart/form-data", 0) != 0)
			return;
		std::string_view content_type_val = it->second;
		size_t boundary_pos = content_type_val.find("boundary=");
		if (boundary_pos == std::string::npos)
			return;
		std::string_view boundary_sv =
			content_type_val.substr(boundary_pos + 9);
		if (!boundary_sv.empty() && boundary_sv.front() == '"')
			boundary_sv = boundary_sv.substr(1, boundary_sv.length() - 2);
		std::string boundary = "--" + std::string(boundary_sv);
		std::string_view body_view = this->body();
		size_t current_pos = 0;
		while (current_pos < body_view.size()) {
			size_t part_start_pos = body_view.find(boundary, current_pos);
			if (part_start_pos == std::string::npos)
				break;
			part_start_pos += boundary.length();
			if (body_view.substr(part_start_pos, 2) == "--")
				break;
			if (body_view.substr(part_start_pos, 2) == "\r\n")
				part_start_pos += 2;
			size_t part_end_pos = body_view.find(boundary, part_start_pos);
			if (part_end_pos == std::string::npos)
				break;
			std::string_view part_view =
				body_view.substr(part_start_pos, part_end_pos - part_start_pos);
			size_t header_data_separator = part_view.find("\r\n\r\n");
			if (header_data_separator == std::string::npos)
				continue;
			std::string_view part_header_view =
				part_view.substr(0, header_data_separator);
			std::string_view part_data_view =
				part_view.substr(header_data_separator + 4);
			if (_ends_with(part_data_view, "\r\n"))
				part_data_view.remove_suffix(2);
			MultipartPart part;
			part.data = {part_data_view.data(), part_data_view.size()};
			_parse_part_headers(
				{part_header_view.data(), part_header_view.size()}, part);
			m_parts.push_back(std::move(part));
			current_pos = part_end_pos;
		}
	}

	bool _ends_with(std::string_view str, std::string_view suffix) {
		return str.size() >= suffix.size() &&
			   str.compare(str.size() - suffix.size(), suffix.size(), suffix) ==
				   0;
	}

  public:
	// 从另一个解析器接管状态
	void take_state(_http_base_parser<HeaderParser> &&old_parser) {
		static_cast<Base &>(*this) = std::move(old_parser);
	}

	void reset_state() {
		Base::reset_state();
		m_parts.clear();
		m_multipart_parsed = false;
	}

	void push_chunk(bytes_const_view chunk) override {
		Base::push_chunk(chunk);
		if (this->request_finished() && !m_multipart_parsed) {
			_parse_multipart_body();
			m_multipart_parsed = true;
		}
	}

	const std::vector<MultipartPart> &get_parts() const { return m_parts; }
};

enum class http_method {
	UNKNOWN = -1,
	GET,
	POST,
	PUT,
	DELETE,
	HEAD,
	OPTIONS,
	PATCH,
	TRACE,
	CONNECT,
};

template <class HeaderParser = http11_header_parser>
struct http_request_parser : _http_base_parser<HeaderParser> {
	http_method method() {
		return parse_enum<http_method>(this->_headline_first());
	}

	std::string url() { return this->_headline_second(); }
};

template <class HeaderParser = http11_header_parser>
struct http_response_parser : _http_base_parser<HeaderParser> {
	int status() {
		auto s = this->_headline_second();
		try {
			return std::stoi(s);
		} catch (std::logic_error const &) {
			return -1;
		}
	}
};

struct http11_header_writer {
	bytes_buffer m_buffer;

	void reset_state() { m_buffer.clear(); }

	bytes_buffer &buffer() { return m_buffer; }

	void begin_header(std::string_view first, std::string_view second,
					  std::string_view third) {
		m_buffer.append(first);
		m_buffer.append_literial(" ");
		m_buffer.append(second);
		m_buffer.append_literial(" ");
		m_buffer.append(third);
	}

	void write_header(std::string_view key, std::string_view value) {
		m_buffer.append_literial("\r\n");
		m_buffer.append(key);
		m_buffer.append_literial(": ");
		m_buffer.append(value);
	}

	void end_header() { m_buffer.append_literial("\r\n\r\n"); }
};

template <class HeaderWriter = http11_header_writer> struct _http_base_writer {
	HeaderWriter m_header_writer;

	void _begin_header(std::string_view first, std::string_view second,
					   std::string_view third) {
		m_header_writer.begin_header(first, second, third);
	}

	void reset_state() { m_header_writer.reset_state(); }

	bytes_buffer &buffer() { return m_header_writer.buffer(); }

	void write_header(std::string_view key, std::string_view value) {
		m_header_writer.write_header(key, value);
	}

	void end_header() { m_header_writer.end_header(); }

	void write_body(std::string_view body) {
		m_header_writer.buffer().append(body);
	}
};

template <class HeaderWriter = http11_header_writer>
struct http_request_writer : _http_base_writer<HeaderWriter> {
	void begin_header(std::string_view method, std::string_view url) {
		this->_begin_header(method, url, "HTTP/1.1");
	}
};

template <class HeaderWriter = http11_header_writer>
struct http_response_writer : _http_base_writer<HeaderWriter> {
	void begin_header(int status) {
		this->_begin_header("HTTP/1.1", std::to_string(status), "OK");
	}
};

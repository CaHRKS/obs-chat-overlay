#include "json.hpp"

#include <cctype>
#include <cstdlib>

namespace json {

namespace {

const Value &empty_value()
{
	static const Value empty;
	return empty;
}

class Parser {
public:
	Parser(const std::string &text, std::string *error) : text_(text), error_(error) {}

	bool parse_document(Value &out)
	{
		skip_whitespace();
		if (!parse_value(out))
			return false;
		skip_whitespace();
		if (pos_ != text_.size()) {
			fail("trailing characters");
			return false;
		}
		return true;
	}

private:
	const std::string &text_;
	size_t pos_ = 0;
	std::string *error_;

	void fail(const char *message)
	{
		if (error_) {
			*error_ = std::string(message) + " at offset " + std::to_string(pos_);
		}
	}

	void skip_whitespace()
	{
		while (pos_ < text_.size()) {
			const char c = text_[pos_];
			if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
				++pos_;
			else
				break;
		}
	}

	bool consume(char expected)
	{
		if (pos_ < text_.size() && text_[pos_] == expected) {
			++pos_;
			return true;
		}
		return false;
	}

	bool parse_value(Value &out)
	{
		if (pos_ >= text_.size()) {
			fail("unexpected end of input");
			return false;
		}
		const char c = text_[pos_];
		switch (c) {
		case '{':
			return parse_object(out);
		case '[':
			return parse_array(out);
		case '"':
			out.type = Value::Type::String;
			return parse_string(out.string);
		case 't':
			return parse_literal("true", out, Value::Type::Bool, true);
		case 'f':
			return parse_literal("false", out, Value::Type::Bool, false);
		case 'n':
			return parse_literal("null", out, Value::Type::Null, false);
		default:
			return parse_number(out);
		}
	}

	bool parse_literal(const char *literal, Value &out, Value::Type type, bool bool_value)
	{
		const size_t length = std::char_traits<char>::length(literal);
		if (text_.compare(pos_, length, literal) != 0) {
			fail("invalid literal");
			return false;
		}
		pos_ += length;
		out.type = type;
		out.boolean = bool_value;
		return true;
	}

	bool parse_number(Value &out)
	{
		const size_t start = pos_;
		if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
			++pos_;
		bool any = false;
		while (pos_ < text_.size() && std::isdigit((unsigned char)text_[pos_])) {
			++pos_;
			any = true;
		}
		if (pos_ < text_.size() && text_[pos_] == '.') {
			++pos_;
			while (pos_ < text_.size() && std::isdigit((unsigned char)text_[pos_])) {
				++pos_;
				any = true;
			}
		}
		if (any && pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
			++pos_;
			if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+'))
				++pos_;
			while (pos_ < text_.size() && std::isdigit((unsigned char)text_[pos_]))
				++pos_;
		}
		if (!any) {
			fail("invalid number");
			return false;
		}
		out.type = Value::Type::Number;
		out.number = std::strtod(text_.substr(start, pos_ - start).c_str(), nullptr);
		return true;
	}

	static void append_utf8(std::string &out, unsigned int cp)
	{
		if (cp < 0x80) {
			out.push_back((char)cp);
		} else if (cp < 0x800) {
			out.push_back((char)(0xC0 | (cp >> 6)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		} else if (cp < 0x10000) {
			out.push_back((char)(0xE0 | (cp >> 12)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		} else {
			out.push_back((char)(0xF0 | (cp >> 18)));
			out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
			out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
			out.push_back((char)(0x80 | (cp & 0x3F)));
		}
	}

	bool parse_hex4(unsigned int &value)
	{
		if (pos_ + 4 > text_.size()) {
			fail("truncated unicode escape");
			return false;
		}
		value = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = text_[pos_ + i];
			unsigned int digit = 0;
			if (c >= '0' && c <= '9')
				digit = (unsigned int)(c - '0');
			else if (c >= 'a' && c <= 'f')
				digit = (unsigned int)(c - 'a' + 10);
			else if (c >= 'A' && c <= 'F')
				digit = (unsigned int)(c - 'A' + 10);
			else {
				fail("invalid unicode escape");
				return false;
			}
			value = (value << 4) | digit;
		}
		pos_ += 4;
		return true;
	}

	bool parse_string(std::string &out)
	{
		if (!consume('"')) {
			fail("expected string");
			return false;
		}
		out.clear();
		while (pos_ < text_.size()) {
			const char c = text_[pos_++];
			if (c == '"')
				return true;
			if (c != '\\') {
				out.push_back(c);
				continue;
			}
			if (pos_ >= text_.size()) {
				fail("truncated escape");
				return false;
			}
			const char esc = text_[pos_++];
			switch (esc) {
			case '"':
				out.push_back('"');
				break;
			case '\\':
				out.push_back('\\');
				break;
			case '/':
				out.push_back('/');
				break;
			case 'b':
				out.push_back('\b');
				break;
			case 'f':
				out.push_back('\f');
				break;
			case 'n':
				out.push_back('\n');
				break;
			case 'r':
				out.push_back('\r');
				break;
			case 't':
				out.push_back('\t');
				break;
			case 'u': {
				unsigned int cp = 0;
				if (!parse_hex4(cp))
					return false;
				if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() &&
				    text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
					pos_ += 2;
					unsigned int low = 0;
					if (!parse_hex4(low))
						return false;
					if (low >= 0xDC00 && low <= 0xDFFF)
						cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
					else
						cp = 0xFFFD; // lone high surrogate
				} else if (cp >= 0xD800 && cp <= 0xDFFF) {
					cp = 0xFFFD; // lone surrogate
				}
				append_utf8(out, cp);
				break;
			}
			default:
				fail("unknown escape");
				return false;
			}
		}
		fail("unterminated string");
		return false;
	}

	bool parse_array(Value &out)
	{
		++pos_; // '['
		out.type = Value::Type::Array;
		skip_whitespace();
		if (consume(']'))
			return true;
		while (true) {
			Value item;
			skip_whitespace();
			if (!parse_value(item))
				return false;
			out.array.push_back(std::move(item));
			skip_whitespace();
			if (consume(','))
				continue;
			if (consume(']'))
				return true;
			fail("expected ',' or ']'");
			return false;
		}
	}

	bool parse_object(Value &out)
	{
		++pos_; // '{'
		out.type = Value::Type::Object;
		skip_whitespace();
		if (consume('}'))
			return true;
		while (true) {
			skip_whitespace();
			std::string key;
			if (!parse_string(key))
				return false;
			skip_whitespace();
			if (!consume(':')) {
				fail("expected ':'");
				return false;
			}
			skip_whitespace();
			Value item;
			if (!parse_value(item))
				return false;
			out.object.emplace_back(std::move(key), std::move(item));
			skip_whitespace();
			if (consume(','))
				continue;
			if (consume('}'))
				return true;
			fail("expected ',' or '}'");
			return false;
		}
	}
};

} // namespace

const Value *Value::find(const std::string &key) const
{
	if (type != Type::Object)
		return nullptr;
	for (const auto &kv : object) {
		if (kv.first == key)
			return &kv.second;
	}
	return nullptr;
}

const Value &Value::operator[](const std::string &key) const
{
	const Value *found = find(key);
	return found ? *found : empty_value();
}

const Value &Value::at(size_t index) const
{
	if (type == Type::Array && index < array.size())
		return array[index];
	return empty_value();
}

std::string Value::get_string(const std::string &key, const std::string &fallback) const
{
	const Value *found = find(key);
	if (!found || found->type != Type::String)
		return fallback;
	return found->string;
}

double Value::get_number(const std::string &key, double fallback) const
{
	const Value *found = find(key);
	if (!found || found->type != Type::Number)
		return fallback;
	return found->number;
}

bool Value::get_bool(const std::string &key, bool fallback) const
{
	const Value *found = find(key);
	if (!found || found->type != Type::Bool)
		return fallback;
	return found->boolean;
}

size_t Value::size() const
{
	if (type == Type::Array)
		return array.size();
	if (type == Type::Object)
		return object.size();
	return 0;
}

bool parse(const std::string &text, Value &out, std::string *error)
{
	out = Value();
	Parser parser(text, error);
	return parser.parse_document(out);
}

} // namespace json

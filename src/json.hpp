#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace json {

class Value {
public:
	enum class Type { Null, Bool, Number, String, Array, Object };

	Type type = Type::Null;
	bool boolean = false;
	double number = 0.0;
	std::string string;
	std::vector<Value> array;
	std::vector<std::pair<std::string, Value>> object;

	bool is_array() const { return type == Type::Array; }
	bool is_object() const { return type == Type::Object; }

	const Value *find(const std::string &key) const;
	const Value &operator[](const std::string &key) const;
	const Value &at(size_t index) const;

	std::string get_string(const std::string &key, const std::string &fallback = std::string()) const;
	double get_number(const std::string &key, double fallback = 0.0) const;
	bool get_bool(const std::string &key, bool fallback = false) const;

	size_t size() const;
};

bool parse(const std::string &text, Value &out, std::string *error = nullptr);

} // namespace json

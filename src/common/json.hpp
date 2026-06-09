#pragma once

#include <map>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

class Json {
public:
    enum class Type {
        Null,
        Bool,
        Number,
        String,
        Object,
        Array
    };

    Type type = Type::Null;
    bool bool_value = false;
    double number_value = 0;
    std::string string_value;
    std::map<std::string, Json, std::less<>> object_value;
    std::vector<Json> array_value;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    const Json *find(std::string_view key) const;
    std::string string_or(std::string_view key, std::string_view fallback = "") const;
    int int_or(std::string_view key, int fallback = 0) const;
    bool bool_or(std::string_view key, bool fallback = false) const;
};

Json parse_json(std::string_view text);
std::string dump_json(const Json &json);

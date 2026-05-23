#pragma once

#include <map>
#include <stdexcept>
#include <string>
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
    std::map<std::string, Json> object_value;
    std::vector<Json> array_value;

    bool is_null() const { return type == Type::Null; }
    bool is_bool() const { return type == Type::Bool; }
    bool is_number() const { return type == Type::Number; }
    bool is_string() const { return type == Type::String; }
    bool is_object() const { return type == Type::Object; }
    bool is_array() const { return type == Type::Array; }

    const Json *find(const std::string &key) const;
    std::string string_or(const std::string &key, const std::string &fallback = "") const;
    int int_or(const std::string &key, int fallback = 0) const;
    bool bool_or(const std::string &key, bool fallback = false) const;
};

Json parse_json(const std::string &text);
std::string dump_json(const Json &json);


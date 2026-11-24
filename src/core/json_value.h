#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Minimal JSON value shared by the native backend. The app's existing parsers
// are point lookups (`json_get_string`) which are fine for flat replies but
// useless for InnerTube's nested responses, so this is a real tree.
namespace jsonv
{
class Value
{
public:
    enum class Type
    {
        Null,
        Bool,
        Number,
        String,
        Array,
        Object,
    };

    Value() = default;

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_string() const { return type_ == Type::String; }
    bool is_number() const { return type_ == Type::Number; }
    bool is_array() const { return type_ == Type::Array; }
    bool is_object() const { return type_ == Type::Object; }

    bool as_bool(bool fallback = false) const;
    double as_double(double fallback = 0.0) const;
    long long as_int(long long fallback = 0) const;
    const std::string &as_string() const;

    // Object lookup; returns the null singleton when missing.
    const Value &operator[](std::string_view key) const;
    // Array element; returns the null singleton when out of range.
    const Value &at(std::size_t index) const;
    std::size_t size() const;

    // Raw containers, for serialisation and iteration.
    const std::vector<Value> &items() const { return array_; }
    const std::vector<std::pair<std::string, Value>> &members() const { return object_; }

    // Convenience readers that never throw.
    std::string str(std::string_view key, const std::string &fallback = "") const;
    double num(std::string_view key, double fallback = 0.0) const;
    bool boolean(std::string_view key, bool fallback = false) const;
    const Value &get(std::string_view key) const { return (*this)[key]; }

    static const Value &null();
    static bool parse(std::string_view text, Value &out, std::string &error);

private:
    friend class Parser;
    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::vector<Value> array_;
    std::vector<std::pair<std::string, Value>> object_;
};

// Serialise with optional pretty indentation (2 spaces when pretty).
std::string dump(const Value &value, bool pretty = false);
} // namespace jsonv

#include "json_value.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace jsonv
{
namespace
{
const Value kNull;

constexpr int kMaxDepth = 256;

void append_utf8(std::string &out, std::uint32_t cp)
{
    if (cp <= 0x7F)
        out += static_cast<char>(cp);
    else if (cp <= 0x7FF)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp <= 0xFFFF)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

void escape_into(std::string &out, const std::string &s)
{
    out += '"';
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else
            {
                out += static_cast<char>(c);
            }
        }
    }
    out += '"';
}
} // namespace

const Value &Value::null()
{
    return kNull;
}

bool Value::as_bool(bool fallback) const
{
    return type_ == Type::Bool ? bool_ : fallback;
}

double Value::as_double(double fallback) const
{
    return type_ == Type::Number ? num_ : fallback;
}

long long Value::as_int(long long fallback) const
{
    return type_ == Type::Number ? static_cast<long long>(num_) : fallback;
}

const std::string &Value::as_string() const
{
    return type_ == Type::String ? str_ : kNull.str_;
}

const Value &Value::operator[](std::string_view key) const
{
    if (type_ != Type::Object)
        return kNull;
    for (const auto &kv : object_)
        if (kv.first == key)
            return kv.second;
    return kNull;
}

const Value &Value::at(std::size_t index) const
{
    if (type_ != Type::Array || index >= array_.size())
        return kNull;
    return array_[index];
}

std::size_t Value::size() const
{
    if (type_ == Type::Array)
        return array_.size();
    if (type_ == Type::Object)
        return object_.size();
    return 0;
}

std::string Value::str(std::string_view key, const std::string &fallback) const
{
    const Value &v = (*this)[key];
    return v.is_string() ? v.as_string() : fallback;
}

double Value::num(std::string_view key, double fallback) const
{
    const Value &v = (*this)[key];
    return v.is_number() ? v.as_double() : fallback;
}

bool Value::boolean(std::string_view key, bool fallback) const
{
    const Value &v = (*this)[key];
    return v.type() == Type::Bool ? v.as_bool() : fallback;
}

// ---------------------------------------------------------------------------
class Parser
{
public:
    Parser(std::string_view text, std::string &error) : text_(text), error_(error) {}

    bool run(Value &out)
    {
        skip_ws();
        if (!value(out, 0))
            return false;
        skip_ws();
        if (pos_ != text_.size())
            return fail("trailing characters");
        return true;
    }

private:
    std::string_view text_;
    std::string &error_;
    std::size_t pos_ = 0;

    bool fail(const char *what)
    {
        if (error_.empty())
            error_ = std::string(what) + " at offset " + std::to_string(pos_);
        return false;
    }

    void skip_ws()
    {
        while (pos_ < text_.size())
        {
            const char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++pos_;
            else
                break;
        }
    }

    bool literal(std::string_view lit)
    {
        if (text_.substr(pos_, lit.size()) != lit)
            return fail("invalid literal");
        pos_ += lit.size();
        return true;
    }

    bool value(Value &out, int depth)
    {
        if (depth > kMaxDepth)
            return fail("nesting too deep");
        if (pos_ >= text_.size())
            return fail("unexpected end of input");
        const char c = text_[pos_];
        switch (c)
        {
        case 'n':
            if (!literal("null"))
                return false;
            out = Value();
            return true;
        case 't':
            if (!literal("true"))
                return false;
            out.type_ = Value::Type::Bool;
            out.bool_ = true;
            return true;
        case 'f':
            if (!literal("false"))
                return false;
            out.type_ = Value::Type::Bool;
            out.bool_ = false;
            return true;
        case '"':
        {
            std::string s;
            if (!string(s))
                return false;
            out.type_ = Value::Type::String;
            out.str_ = std::move(s);
            return true;
        }
        case '[':
            return array(out, depth);
        case '{':
            return object(out, depth);
        default:
            if (c == '-' || (c >= '0' && c <= '9'))
                return number(out);
            return fail("unexpected character");
        }
    }

    bool number(Value &out)
    {
        const char *begin = text_.data() + pos_;
        char *end = nullptr;
        const double v = std::strtod(begin, &end);
        if (end == begin)
            return fail("invalid number");
        pos_ += static_cast<std::size_t>(end - begin);
        out.type_ = Value::Type::Number;
        out.num_ = v;
        return true;
    }

    bool hex4(std::uint32_t &cp)
    {
        if (pos_ + 4 > text_.size())
            return fail("truncated \\u escape");
        cp = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = text_[pos_++];
            cp <<= 4;
            if (c >= '0' && c <= '9')
                cp |= (std::uint32_t)(c - '0');
            else if (c >= 'a' && c <= 'f')
                cp |= (std::uint32_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                cp |= (std::uint32_t)(c - 'A' + 10);
            else
                return fail("invalid \\u escape");
        }
        return true;
    }

    bool string(std::string &out)
    {
        ++pos_; // opening quote
        out.clear();
        while (pos_ < text_.size())
        {
            const unsigned char c = static_cast<unsigned char>(text_[pos_++]);
            if (c == '"')
                return true;
            if (c != '\\')
            {
                if (c < 0x20)
                    return fail("control character in string");
                out += static_cast<char>(c);
                continue;
            }
            if (pos_ >= text_.size())
                return fail("truncated escape");
            const char e = text_[pos_++];
            switch (e)
            {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u':
            {
                std::uint32_t cp = 0;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp <= 0xDBFF)
                {
                    // High surrogate: needs a following low surrogate.
                    if (pos_ + 6 > text_.size() || text_[pos_] != '\\' ||
                        text_[pos_ + 1] != 'u')
                        return fail("lone surrogate");
                    pos_ += 2;
                    std::uint32_t lo = 0;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return fail("invalid low surrogate");
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                else if (cp >= 0xDC00 && cp <= 0xDFFF)
                {
                    return fail("lone low surrogate");
                }
                append_utf8(out, cp);
                break;
            }
            default:
                return fail("invalid escape");
            }
        }
        return fail("unterminated string");
    }

    bool array(Value &out, int depth)
    {
        ++pos_; // [
        out.type_ = Value::Type::Array;
        out.array_.clear();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == ']')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            skip_ws();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.array_.push_back(std::move(item));
            skip_ws();
            if (pos_ >= text_.size())
                return fail("unterminated array");
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == ']')
            {
                ++pos_;
                return true;
            }
            return fail("expected ',' or ']'");
        }
    }

    bool object(Value &out, int depth)
    {
        ++pos_; // {
        out.type_ = Value::Type::Object;
        out.object_.clear();
        skip_ws();
        if (pos_ < text_.size() && text_[pos_] == '}')
        {
            ++pos_;
            return true;
        }
        for (;;)
        {
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != '"')
                return fail("expected object key");
            std::string key;
            if (!string(key))
                return false;
            skip_ws();
            if (pos_ >= text_.size() || text_[pos_] != ':')
                return fail("expected ':'");
            ++pos_;
            skip_ws();
            Value item;
            if (!value(item, depth + 1))
                return false;
            out.object_.emplace_back(std::move(key), std::move(item));
            skip_ws();
            if (pos_ >= text_.size())
                return fail("unterminated object");
            if (text_[pos_] == ',')
            {
                ++pos_;
                continue;
            }
            if (text_[pos_] == '}')
            {
                ++pos_;
                return true;
            }
            return fail("expected ',' or '}'");
        }
    }
};

bool Value::parse(std::string_view text, Value &out, std::string &error)
{
    error.clear();
    out = Value();
    Parser p(text, error);
    return p.run(out);
}

namespace
{
void dump_into(std::string &out, const Value &v, bool pretty, int depth)
{
    auto newline = [&](int d) {
        if (!pretty)
            return;
        out += '\n';
        out.append(static_cast<std::size_t>(d) * 2, ' ');
    };
    switch (v.type())
    {
    case Value::Type::Null: out += "null"; break;
    case Value::Type::Bool: out += v.as_bool() ? "true" : "false"; break;
    case Value::Type::Number:
    {
        char buf[40];
        const double d = v.as_double();
        if (d == std::floor(d) && std::fabs(d) < 1e15)
            std::snprintf(buf, sizeof(buf), "%lld", (long long)d);
        else
            std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += buf;
        break;
    }
    case Value::Type::String: escape_into(out, v.as_string()); break;
    case Value::Type::Array:
    {
        out += '[';
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                out += ',';
            newline(depth + 1);
            dump_into(out, v.items()[i], pretty, depth + 1);
        }
        if (v.size())
            newline(depth);
        out += ']';
        break;
    }
    case Value::Type::Object:
    {
        out += '{';
        for (std::size_t i = 0; i < v.size(); ++i)
        {
            if (i)
                out += ',';
            newline(depth + 1);
            escape_into(out, v.members()[i].first);
            out += pretty ? ": " : ":";
            dump_into(out, v.members()[i].second, pretty, depth + 1);
        }
        if (v.size())
            newline(depth);
        out += '}';
        break;
    }
    }
}
} // namespace

std::string dump(const Value &value, bool pretty)
{
    std::string out;
    dump_into(out, value, pretty, 0);
    return out;
}
} // namespace jsonv

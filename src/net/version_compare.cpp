#include "version_compare.h"

#include <algorithm>
#include <cctype>

namespace version_compare
{

std::string trim(std::string s)
{
    auto not_space = [](unsigned char c) { return !std::isspace(c) && c != '"'; };
    while (!s.empty() && !not_space((unsigned char)s.front()))
        s.erase(s.begin());
    while (!s.empty() && !not_space((unsigned char)s.back()))
        s.pop_back();
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V'))
        s.erase(s.begin());
    return s;
}

std::vector<int> parse(const std::string &v)
{
    std::vector<int> out;
    int n = 0;
    bool have = false;
    for (char c : v)
    {
        if (c >= '0' && c <= '9')
        {
            // Guard against a malformed/huge component in version.txt.
            if (n < 100000000)
                n = n * 10 + (c - '0');
            have = true;
        }
        else if (c == '.')
        {
            out.push_back(have ? n : 0);
            n = 0;
            have = false;
        }
        else
        {
            break;
        }
    }
    if (have)
        out.push_back(n);
    return out;
}

std::string canonical(const std::string &v)
{
    const auto parts = parse(trim(v));
    if (parts.empty())
        return {};
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i)
            out += '.';
        out += std::to_string(parts[i]);
    }
    return out;
}

bool newer(const std::string &latest, const std::string &current)
{
    const auto a = parse(latest);
    const auto b = parse(current);
    if (a.empty() || b.empty())
        return false;
    const size_t n = std::max(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
    {
        const int av = i < a.size() ? a[i] : 0;
        const int bv = i < b.size() ? b[i] : 0;
        if (av != bv)
            return av > bv;
    }
    return false;
}

} // namespace version_compare

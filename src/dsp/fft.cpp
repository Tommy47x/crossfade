#include "fft.h"

#include <cmath>

namespace dsp
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
}

std::size_t next_pow2(std::size_t n)
{
    std::size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

void fft(std::vector<cplx> &a, bool inverse)
{
    const std::size_t n = a.size();
    if (n < 2)
        return;

    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i)
    {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }

    for (std::size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPi / (double)len * (inverse ? 1.0 : -1.0);
        const cplx wlen(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len)
        {
            cplx w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k)
            {
                const cplx u = a[i + k];
                const cplx v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }

    if (inverse)
    {
        const double inv = 1.0 / (double)n;
        for (cplx &v : a)
            v *= inv;
    }
}
} // namespace dsp

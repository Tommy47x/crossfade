#pragma once

#include <complex>
#include <cstddef>
#include <vector>

// Minimal in-place radix-2 FFT. Room correction is a one-shot offline
// calculation, so clarity and exactness beat speed here; nothing on the audio
// path depends on this file.
namespace dsp
{
using cplx = std::complex<double>;

// Smallest power of two >= n (n >= 1).
std::size_t next_pow2(std::size_t n);

// In-place forward (inverse=false) or inverse (inverse=true, already scaled by
// 1/N) transform. `a.size()` must be a power of two.
void fft(std::vector<cplx> &a, bool inverse);
} // namespace dsp

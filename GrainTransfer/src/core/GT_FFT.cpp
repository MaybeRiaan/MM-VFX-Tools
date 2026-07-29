/*  GT_FFT.cpp — see GT_FFT.h.  */

#include "GT_FFT.h"
#include "kiss_fftnd.h"

#include <cstring>

namespace gt {

Fft2D::Fft2D(int n)
    : n_(n)
{
    const int dims[2] = { n, n };
    fwd_ = kiss_fftnd_alloc(dims, 2, 0, nullptr, nullptr);
    inv_ = kiss_fftnd_alloc(dims, 2, 1, nullptr, nullptr);
    tmp_.resize((size_t)n * n);
}

Fft2D::~Fft2D()
{
    kiss_fft_free(fwd_);
    kiss_fft_free(inv_);
}

void Fft2D::forward(const float *realIn, kiss_fft_cpx *spec)
{
    const size_t N = (size_t)n_ * n_;
    for (size_t i = 0; i < N; ++i) {
        tmp_[i].r = realIn[i];
        tmp_[i].i = 0.f;
    }
    kiss_fftnd((kiss_fftnd_cfg)fwd_, tmp_.data(), spec);
}

void Fft2D::inverse(const kiss_fft_cpx *spec, float *realOut)
{
    const size_t N = (size_t)n_ * n_;
    kiss_fftnd((kiss_fftnd_cfg)inv_, spec, tmp_.data());
    const float s = 1.f / (float)N;
    for (size_t i = 0; i < N; ++i)
        realOut[i] = tmp_[i].r * s;
}

} // namespace gt

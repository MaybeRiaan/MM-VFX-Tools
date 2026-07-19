/*  GT_FFT.h — 2D FFT for Grain Transfer, thin wrapper over vendored kissfft
    (third_party/kissfft, BSD-3-Clause; float scalar).

    N×N complex transform via kiss_fftnd. Only the ANALYZE-ONCE / PRECOMPUTE-
    ONCE paths touch the FFT — nothing per-frame — so simplicity beats raw
    speed here. Forward takes a real plane (imag = 0); inverse returns the
    real part scaled by 1/(N·N) so forward→inverse is the identity.

    AE-free: unit-testable headlessly.
*/

#pragma once

#include <vector>
#include "kiss_fft.h"

namespace gt {

class Fft2D {
public:
    explicit Fft2D(int n);
    ~Fft2D();
    Fft2D(const Fft2D &) = delete;
    Fft2D &operator=(const Fft2D &) = delete;

    int n() const { return n_; }

    // realIn: n*n floats (row-major). spec: n*n bins.
    void forward(const float *realIn, kiss_fft_cpx *spec);

    // spec: n*n bins. realOut: n*n floats — real part of the inverse,
    // normalized by 1/(n*n). Imaginary residue is discarded (for Hermitian
    // spectra it is float noise).
    void inverse(const kiss_fft_cpx *spec, float *realOut);

private:
    int n_;
    void *fwd_;   // kiss_fftnd_cfg
    void *inv_;   // kiss_fftnd_cfg
    std::vector<kiss_fft_cpx> tmp_;
};

} // namespace gt

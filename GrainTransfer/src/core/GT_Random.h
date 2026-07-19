/*  GT_Random.h — deterministic PRNG for Grain Transfer.

    SplitMix64 everywhere: tiny, fast, high-quality, and — critically —
    bit-identical across platforms and compilers, so a given Seed always
    produces the same grain. No std::random in any render path (its
    distributions are implementation-defined).
*/

#pragma once

#include <cstdint>

namespace gt {

inline uint64_t SplitMix64(uint64_t &state)
{
    state += 0x9E3779B97F4A7C15ull;
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// Stable seed derivation from up to three components (user seed, frame
// number, salt). Chained SplitMix64 — order-sensitive, well mixed.
inline uint64_t HashSeed(uint64_t a, uint64_t b, uint64_t c = 0)
{
    uint64_t s = a;
    uint64_t h = SplitMix64(s);
    s = h ^ (b * 0xBF58476D1CE4E5B9ull);
    h = SplitMix64(s);
    s = h ^ (c * 0x94D049BB133111EBull);
    return SplitMix64(s);
}

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}

    uint64_t next() { return SplitMix64(s); }

    // Uniform in [0,1), 24-bit mantissa.
    float nextFloat() { return (float)(next() >> 40) * (1.0f / 16777216.0f); }

    // Uniform integer in [0,n). Modulo bias is irrelevant at our n (≤ tile size).
    uint32_t nextBelow(uint32_t n) { return n ? (uint32_t)(next() % n) : 0u; }

    bool nextBool() { return (next() & 1ull) != 0; }
};

} // namespace gt

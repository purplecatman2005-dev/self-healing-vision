#pragma once
// 基本データ構造・乱数・ユーティリティ
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <vector>
#include <string>
#include <algorithm>

struct Image {
    int w = 0, h = 0;
    std::vector<float> px;   // グレースケール 0.0〜1.0
    Image() = default;
    Image(int W, int H) : w(W), h(H), px((size_t)W * H, 0.f) {}
    float&       at(int x, int y)       { return px[(size_t)y * w + x]; }
    const float& at(int x, int y) const { return px[(size_t)y * w + x]; }
};

// xorshift64* — 再現性のある軽量PRNG(プロセス再起動をまたいでもフレーム単位で決定的)
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() { s ^= s >> 12; s ^= s << 25; s ^= s >> 27; return s * 0x2545F4914F6CDD1Dull; }
    double uni() { return (double)(next() >> 11) * (1.0 / 9007199254740992.0); }
    double range(double a, double b) { return a + (b - a) * uni(); }
    int irange(int a, int b) { return a + (int)(next() % (uint64_t)(b - a + 1)); }
    double gauss(double mu, double sig) {
        double u1 = std::max(1e-12, uni()), u2 = uni();
        return mu + sig * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
};

inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

#pragma once
// 合成画像ジェネレータ:
//   カメラの代わりに図形(○/□/△)を描画し、認識を壊す2種類の外乱を注入する。
//   - Glare   : 鏡面反射(飽和した輝点ブロブ + ハロー)
//   - Impulse : ごま塩ノイズ(画素が突発的に0.0/1.0へ飛ぶ)
#include "common.hpp"

enum class Shape   : int { Circle = 0, Square = 1, Triangle = 2 };
enum class Corrupt : int { None = 0, Glare = 1, Impulse = 2 };

inline const char* shapeName(int s) {
    static const char* n[] = { "Circle", "Square", "Triangle" };
    return (s >= 0 && s < 3) ? n[s] : "?";
}
inline const char* corruptName(Corrupt c) {
    switch (c) { case Corrupt::Glare: return "Glare"; case Corrupt::Impulse: return "Impulse"; default: return "Clean"; }
}

constexpr int   IMG = 32;
constexpr float BG  = 0.10f;   // 背景輝度
constexpr float FG  = 0.70f;   // 図形輝度

inline Image renderShape(Shape s, Rng& rng) {
    Image im(IMG, IMG);
    double cx = 16.0 + rng.range(-2.5, 2.5);
    double cy = 16.0 + rng.range(-2.5, 2.5);
    double r  = rng.range(8.0, 11.0);
    for (int y = 0; y < IMG; y++) {
        for (int x = 0; x < IMG; x++) {
            bool inside = false;
            double dx = x - cx, dy = y - cy;
            switch (s) {
            case Shape::Circle:
                inside = dx * dx + dy * dy <= r * r;
                break;
            case Shape::Square:
                inside = std::fabs(dx) <= 0.8 * r && std::fabs(dy) <= 0.8 * r;
                break;
            case Shape::Triangle: {
                double t = (y - (cy - r)) / (2.0 * r);   // 頂点0 → 底辺1
                inside = t >= 0.0 && t <= 1.0 && std::fabs(dx) <= r * t;
                break;
            }
            }
            float v = inside ? FG : BG;
            im.at(x, y) = clamp01(v + (float)rng.gauss(0.0, 0.02));
        }
    }
    return im;
}

// 鏡面反射: 中心が完全飽和(1.0)した輝点ブロブ2個 + 減衰ハロー
inline void addGlare(Image& im, Rng& rng) {
    for (int blob = 0; blob < 2; blob++) {
        double gx = rng.range(9.0, 23.0);
        double gy = rng.range(9.0, 23.0);
        double rr = rng.range(3.5, 5.5);
        for (int y = 0; y < im.h; y++) {
            for (int x = 0; x < im.w; x++) {
                double d = std::sqrt((x - gx) * (x - gx) + (y - gy) * (y - gy));
                if (d < rr) {
                    im.at(x, y) = 1.0f;
                } else if (d < rr * 1.8) {
                    float halo = (float)(0.25 * (1.0 - (d - rr) / (0.8 * rr)));
                    im.at(x, y) = clamp01(im.at(x, y) + halo);
                }
            }
        }
    }
}

// ごま塩ノイズ: 各画素が確率pで0.0または1.0へ
inline void addImpulse(Image& im, Rng& rng, double p = 0.10) {
    for (auto& v : im.px)
        if (rng.uni() < p) v = (rng.uni() < 0.5) ? 0.0f : 1.0f;
}

inline void applyCorrupt(Image& im, Corrupt c, Rng& rng) {
    if (c == Corrupt::Glare)   addGlare(im, rng);
    if (c == Corrupt::Impulse) addImpulse(im, rng);
}

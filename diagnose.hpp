#pragma once
// 数理診断モジュール:
//   失敗フレーム群の統計量からバグ(認識エラー)の根本原因を特定し、
//   修正後の計算グラフパラメータを最適化で求める。
//
// 使う統計量:
//   satRatio    : 飽和画素率 P(px >= 0.95)
//   satCluster  : 飽和画素の最大連結成分 / 全飽和画素数(空間的凝集度)
//                 → 鏡面反射は「1つの塊」、ごま塩は「散在」なのでこれで分離できる
//   impulseFrac : 極値画素(<=0.05 or >=0.95)のうち3x3中央値から0.4以上乖離する率
//                 (孤立インパルス性の検定)
//   klDiv       : 輝度16binヒストグラムの、クリーン校正分布に対するKLダイバージェンス
//                 (「校正時と入力分布が変わった」ことの定量的証拠)
#include "common.hpp"
#include "imagegen.hpp"
#include "pipeline.hpp"

enum class Cause { Glare, Impulse, Unknown };

inline const char* causeName(Cause c) {
    switch (c) {
    case Cause::Glare:   return "鏡面反射(飽和ブロブ)";
    case Cause::Impulse: return "インパルス性ノイズ(ごま塩)";
    default:             return "原因不明";
    }
}

struct Evidence {
    double satRatio = 0, satCluster = 0, impulseFrac = 0, klDiv = 0;
};

struct CalibStats {
    double satMean = 0;
    double hist[16] = {};
};

struct LabeledImage {
    Image img;
    int   label;
};

// --- フレーム単位の統計量 ---------------------------------------------------

inline double frameSatRatio(const Image& im) {
    int n = 0;
    for (float v : im.px) n += v >= 0.95f;
    return (double)n / im.px.size();
}

inline double frameSatCluster(const Image& im) {
    const int W = im.w, H = im.h;
    std::vector<char> sat((size_t)W * H, 0);
    int total = 0;
    for (int i = 0; i < W * H; i++) { sat[i] = im.px[i] >= 0.95f; total += sat[i]; }
    if (total == 0) return 0.0;
    std::vector<char> vis((size_t)W * H, 0);
    std::vector<int> stack;
    int best = 0;
    for (int i = 0; i < W * H; i++) {
        if (!sat[i] || vis[i]) continue;
        int area = 0;
        stack.clear(); stack.push_back(i); vis[i] = 1;
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back(); area++;
            int x = p % W, y = p / W;
            const int nx[4] = { x - 1, x + 1, x, x };
            const int ny[4] = { y, y, y - 1, y + 1 };
            for (int k = 0; k < 4; k++) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
                int q = ny[k] * W + nx[k];
                if (sat[q] && !vis[q]) { vis[q] = 1; stack.push_back(q); }
            }
        }
        best = std::max(best, area);
    }
    return (double)best / total;
}

inline double frameImpulseFrac(const Image& im) {
    int hit = 0;
    for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
        float v = im.at(x, y);
        if (v > 0.05f && v < 0.95f) continue;
        float nb[9]; int n = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = std::clamp(x + dx, 0, im.w - 1);
            int ny = std::clamp(y + dy, 0, im.h - 1);
            nb[n++] = im.at(nx, ny);
        }
        std::nth_element(nb, nb + 4, nb + 9);
        if (std::fabs(v - nb[4]) > 0.4f) hit++;
    }
    return (double)hit / im.px.size();
}

inline void frameHist(const Image& im, double h[16]) {
    for (int i = 0; i < 16; i++) h[i] = 0;
    for (float v : im.px) {
        int b = std::min(15, (int)(v * 16.0f));
        h[b] += 1.0 / im.px.size();
    }
}

// --- 校正統計(クリーン基準分布) --------------------------------------------

inline CalibStats computeCalibStats() {
    CalibStats cs;
    const int N = 60;
    for (int i = 0; i < N; i++) {
        Rng rng(424200ull + i);
        Image im = renderShape((Shape)(i % 3), rng);
        cs.satMean += frameSatRatio(im) / N;
        double h[16]; frameHist(im, h);
        for (int b = 0; b < 16; b++) cs.hist[b] += h[b] / N;
    }
    return cs;
}

// --- 原因特定 ---------------------------------------------------------------

inline Cause diagnose(const std::vector<LabeledImage>& fails,
                      const CalibStats& calib, Evidence& ev) {
    ev = Evidence{};
    if (fails.empty()) return Cause::Unknown;
    double hist[16] = {};
    for (auto& li : fails) {
        ev.satRatio    += frameSatRatio(li.img)    / fails.size();
        ev.satCluster  += frameSatCluster(li.img)  / fails.size();
        ev.impulseFrac += frameImpulseFrac(li.img) / fails.size();
        double h[16]; frameHist(li.img, h);
        for (int b = 0; b < 16; b++) hist[b] += h[b] / fails.size();
    }
    for (int b = 0; b < 16; b++) {
        double p = hist[b] + 1e-9, q = calib.hist[b] + 1e-9;
        ev.klDiv += p * std::log(p / q);
    }
    // 仮説判定: 飽和が校正時より有意に増え、かつ空間的に凝集 → 鏡面反射
    if (ev.satRatio > calib.satMean * 5.0 + 0.01 && ev.satCluster > 0.5)
        return Cause::Glare;
    // 極値が孤立して散在 → インパルスノイズ
    if (ev.impulseFrac > 0.02 && ev.satCluster < 0.5)
        return Cause::Impulse;
    return Cause::Unknown;
}

// --- 修正パラメータの最適化 -------------------------------------------------

struct FixResult {
    bool        ok = false;
    GraphParams gp{};
    double      acc = 0;        // 失敗フレーム+回帰セットでの複合精度
    std::string note;
};

// 回帰セット: 固定シードのクリーン画像(修正が正常系を壊していないかの担保)
inline std::vector<LabeledImage> makeRegressionSet() {
    std::vector<LabeledImage> v;
    for (int i = 0; i < 60; i++) {
        Rng rng(999000ull + i);
        int c = i % 3;
        v.push_back({ renderShape((Shape)c, rng), c });
    }
    return v;
}

inline double evalParams(const GraphParams& gp,
                         const std::vector<LabeledImage>& fails,
                         const std::vector<LabeledImage>& reg) {
    Pipeline p; p.gp = gp; p.calibrate();
    int ok = 0, n = 0;
    double conf;
    for (auto& li : fails) { ok += p.predict(li.img, conf) == li.label; n++; }
    for (auto& li : reg)   { ok += p.predict(li.img, conf) == li.label; n++; }
    return n ? (double)ok / n : 0.0;
}

// 鏡面反射: クリップ閾値 t を [0.50, 0.99] のグリッドで直接探索し、
// 最大精度が続く最長プラトーの中点を採用する(マージン最大化)。
// ※精度は t に対して区分定数なので、勾配法や黄金分割ではなく
//   グリッド+プラトー中点が数理的に適切。
inline FixResult solveGlareFix(const GraphParams& cur,
                               const std::vector<LabeledImage>& fails) {
    auto reg = makeRegressionSet();
    const double t0 = 0.50, t1 = 0.99, step = 0.01;
    std::vector<double> ts, accs;
    double best = 0;
    for (double t = t0; t <= t1 + 1e-9; t += step) {
        GraphParams gp = cur;
        gp.glareClip = true; gp.glareThresh = (float)t;
        double a = evalParams(gp, fails, reg);
        ts.push_back(t); accs.push_back(a);
        best = std::max(best, a);
    }
    // 最長最大プラトーを探す
    int runStart = -1, bestStart = 0, bestLen = 0;
    for (int i = 0; i <= (int)accs.size(); i++) {
        bool atMax = i < (int)accs.size() && accs[i] >= best - 1e-9;
        if (atMax && runStart < 0) runStart = i;
        if (!atMax && runStart >= 0) {
            if (i - runStart > bestLen) { bestLen = i - runStart; bestStart = runStart; }
            runStart = -1;
        }
    }
    double tStar = ts[bestStart + bestLen / 2];
    FixResult r;
    r.gp = cur; r.gp.glareClip = true; r.gp.glareThresh = (float)tStar;
    r.acc = best;
    r.ok = best >= 0.90;
    char buf[160];
    snprintf(buf, sizeof buf,
             "glare-clip段を有効化 thresh=%.2f (グリッド探索: 最大精度%.1f%%のプラトー[%.2f,%.2f]の中点)",
             tStar, best * 100.0, ts[bestStart], ts[bestStart + bestLen - 1]);
    r.note = buf;
    return r;
}

// インパルスノイズ: 離散的なグラフ編集(メディアンフィルタ段の追加)を検証
inline FixResult solveImpulseFix(const GraphParams& cur,
                                 const std::vector<LabeledImage>& fails) {
    auto reg = makeRegressionSet();
    GraphParams gp = cur;
    gp.medianFilter = true;
    FixResult r;
    r.gp = gp;
    r.acc = evalParams(gp, fails, reg);
    r.ok = r.acc >= 0.90;
    char buf[120];
    snprintf(buf, sizeof buf,
             "median3x3段をグラフに追加 (失敗フレーム+回帰セットで精度%.1f%%を確認)", r.acc * 100.0);
    r.note = buf;
    return r;
}

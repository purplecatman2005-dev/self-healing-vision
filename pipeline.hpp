#pragma once
// 認識パイプライン(計算グラフ実行器):
//   input → [glare_clip?] → [median3x3?] → binarize → largest-component
//         → features(充填率, コンパクト度) → nearest-centroid 分類
//
// グラフの構成(段の有効/無効・閾値)は generated_params.hpp が定義し、
// 自己修復時には GraphParams として実行時にも差し替え評価できる。
#include "common.hpp"
#include "imagegen.hpp"
#include "generated_params.hpp"

struct GraphParams {
    bool  glareClip;
    float glareThresh;
    bool  medianFilter;
};

// このバイナリにコンパイル時に焼き込まれたグラフ構成
inline GraphParams activeParams() {
    return { gen::STAGE_GLARE_CLIP, gen::GLARE_CLIP_THRESH, gen::STAGE_MEDIAN_FILTER };
}

// --- 前処理段 -------------------------------------------------------------

constexpr float BIN_T = 0.40f;   // 二値化閾値(BG=0.1 と FG=0.7 の間)

// 二値化した前景の4連結成分ラベリング。戻り値=最大成分の面積。
inline int labelComponents(const Image& im, std::vector<int>& lbl, int& bestLbl) {
    const int W = im.w, H = im.h;
    lbl.assign((size_t)W * H, 0);
    int nextLbl = 0, bestArea = 0;
    bestLbl = 0;
    std::vector<int> stack;
    for (int i = 0; i < W * H; i++) {
        if (im.px[i] <= BIN_T || lbl[i]) continue;
        int area = 0; nextLbl++;
        stack.clear(); stack.push_back(i); lbl[i] = nextLbl;
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back(); area++;
            int x = p % W, y = p / W;
            const int nx[4] = { x - 1, x + 1, x, x };
            const int ny[4] = { y, y, y - 1, y + 1 };
            for (int k = 0; k < 4; k++) {
                if (nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H) continue;
                int q = ny[k] * W + nx[k];
                if (im.px[q] > BIN_T && !lbl[q]) { lbl[q] = nextLbl; stack.push_back(q); }
            }
        }
        if (area > bestArea) { bestArea = area; bestLbl = nextLbl; }
    }
    return bestArea;
}

// Andrew's monotone chain による凸包(反時計回り)
inline std::vector<std::pair<int, int>> convexHull(std::vector<std::pair<int, int>> pts) {
    auto cross = [](const std::pair<int, int>& o, const std::pair<int, int>& a,
                    const std::pair<int, int>& b) -> long long {
        return (long long)(a.first - o.first) * (b.second - o.second)
             - (long long)(a.second - o.second) * (b.first - o.first);
    };
    std::sort(pts.begin(), pts.end());
    pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
    if (pts.size() < 3) return pts;
    std::vector<std::pair<int, int>> h(2 * pts.size());
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); i++) {                      // 下側
        while (k >= 2 && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i-- > 0;) {          // 上側
        while (k >= t && cross(h[k - 2], h[k - 1], pts[i]) <= 0) k--;
        h[k++] = pts[i];
    }
    h.resize(k - 1);
    return h;
}

inline bool insideHull(const std::vector<std::pair<int, int>>& h, int px, int py) {
    if (h.size() < 3) return false;
    for (size_t i = 0; i < h.size(); i++) {
        const auto& a = h[i];
        const auto& b = h[(i + 1) % h.size()];
        long long c = (long long)(b.first - a.first) * (py - a.second)
                    - (long long)(b.second - a.second) * (px - a.first);
        if (c < 0) return false;
    }
    return true;
}

// 飽和画素クリップ+オニオンピール式インペイント:
//   px >= thresh の画素を「反射で壊れた画素」とみなし、外周から順に
//   非飽和近傍の前景/背景多数決で復元する。
//   ※グレースケール中央値で埋めるとハローの中間輝度が混ざって図形が
//     侵食されるため、二値ラベルを伝播させて境界をクリスプに再構成する。
inline void stageGlareClip(Image& im, float thresh) {
    const int W = im.w, H = im.h;
    std::vector<char> sat((size_t)W * H, 0);
    int remain = 0;
    for (int i = 0; i < W * H; i++) { sat[i] = im.px[i] >= thresh; remain += sat[i]; }
    std::vector<char> wasSat = sat;   // 凸包復元用に元の遮蔽マスクを保存
    for (int pass = 0; pass < 64 && remain > 0; pass++) {
        std::vector<std::pair<int, float>> fills;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
            if (!sat[(size_t)y * W + x]) continue;
            int nfg = 0, nbg = 0;
            for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
                int nx = x + dx, ny = y + dy;
                if (nx < 0 || ny < 0 || nx >= W || ny >= H) continue;
                if (sat[(size_t)ny * W + nx]) continue;
                (im.at(nx, ny) > BIN_T ? nfg : nbg)++;
            }
            if (nfg + nbg == 0) continue;   // 内部画素は後のパスで埋まる
            fills.push_back({ y * W + x, nfg >= nbg ? FG : BG });
        }
        if (fills.empty()) break;
        for (auto& f : fills) { im.px[f.first] = f.second; sat[f.first] = 0; remain--; }
    }
    // 埋めきれなかった孤立領域は背景で塗る
    for (int i = 0; i < W * H; i++) if (sat[i]) im.px[i] = BG;

    // 凸性事前分布による復元:
    //   ブロブが図形の境界を覆っていた箇所は深い切り欠きとして残り、
    //   コンパクト度(周長)特徴を破壊する。対象図形は凸なので、
    //   「遮蔽されていた画素に限り」最大成分の凸包内部をFGで埋め、
    //   切り欠きを浅い面取りへ変換する。
    //   (飽和していなかった画素には触れないので、他の劣化要因まで
    //    隠してしまうことはない)
    std::vector<int> lbl; int bestLbl;
    if (labelComponents(im, lbl, bestLbl) >= 8) {
        std::vector<std::pair<int, int>> pts;
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if (lbl[(size_t)y * W + x] == bestLbl) pts.push_back({ x, y });
        auto hull = convexHull(pts);
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++)
            if (wasSat[(size_t)y * W + x] && insideHull(hull, x, y))
                im.at(x, y) = FG;
    }
}

inline void stageMedian3(Image& im) {
    Image out = im;
    for (int y = 0; y < im.h; y++) for (int x = 0; x < im.w; x++) {
        float nb[9]; int n = 0;
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
            int nx = std::clamp(x + dx, 0, im.w - 1);
            int ny = std::clamp(y + dy, 0, im.h - 1);
            nb[n++] = im.at(nx, ny);
        }
        std::nth_element(nb, nb + 4, nb + 9);
        out.at(x, y) = nb[4];
    }
    im = out;
}

// --- 特徴抽出 -------------------------------------------------------------

struct Features {
    double fill;      // 最大連結成分の面積 / バウンディングボックス面積
    double compact;   // コンパクト度 4πA / P^2 (穴・ギザつきに敏感)
};

// 二値化 → 最大4連結成分 → 特徴量。成分が見つからなければ false。
inline bool extractFeatures(const Image& im, Features& f) {
    const int W = im.w, H = im.h;
    std::vector<int> lbl;
    int bestLbl;
    int bestArea = labelComponents(im, lbl, bestLbl);
    if (bestArea < 8) return false;
    int x0 = W, y0 = H, x1 = -1, y1 = -1, perim = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        if (lbl[(size_t)y * W + x] != bestLbl) continue;
        x0 = std::min(x0, x); x1 = std::max(x1, x);
        y0 = std::min(y0, y); y1 = std::max(y1, y);
        const int nx[4] = { x - 1, x + 1, x, x };
        const int ny[4] = { y, y, y - 1, y + 1 };
        for (int k = 0; k < 4; k++) {
            bool out = nx[k] < 0 || ny[k] < 0 || nx[k] >= W || ny[k] >= H
                       || lbl[(size_t)ny[k] * W + nx[k]] != bestLbl;
            perim += out;
        }
    }
    double bbox = (double)(x1 - x0 + 1) * (y1 - y0 + 1);
    f.fill    = bestArea / bbox;
    f.compact = 4.0 * 3.14159265358979 * bestArea / ((double)perim * perim);
    return true;
}

// --- 分類器 ---------------------------------------------------------------

struct Pipeline {
    GraphParams gp;
    double cen[3][2] = {};   // クラス重心 (fill, compact)
    double sd[2] = { 1, 1 }; // 正規化用プールド標準偏差

    void preprocess(Image& im) const {
        if (gp.glareClip)    stageGlareClip(im, gp.glareThresh);
        if (gp.medianFilter) stageMedian3(im);
    }

    // 起動時セルフキャリブレーション:
    // クリーン合成画像を現在のグラフ構成で処理し、クラス重心を測る。
    // (グラフを書き換えた新バージョンも自動的に整合が取れる)
    void calibrate() {
        double var[2] = {};
        for (int c = 0; c < 3; c++) {
            std::vector<Features> fs;
            for (int i = 0; i < 40; i++) {
                Rng rng(42000ull + (uint64_t)c * 97 + i);
                Image im = renderShape((Shape)c, rng);
                preprocess(im);
                Features f;
                if (extractFeatures(im, f)) fs.push_back(f);
            }
            // 極端なパラメータ候補では図形が消えて標本が取れないことがある。
            // その場合は到達不能な重心を置き、この候補が選ばれないようにする。
            if (fs.empty()) { cen[c][0] = cen[c][1] = 1e9; continue; }
            double m0 = 0, m1 = 0;
            for (auto& f : fs) { m0 += f.fill; m1 += f.compact; }
            m0 /= fs.size(); m1 /= fs.size();
            cen[c][0] = m0; cen[c][1] = m1;
            for (auto& f : fs) {
                var[0] += (f.fill - m0) * (f.fill - m0);
                var[1] += (f.compact - m1) * (f.compact - m1);
            }
        }
        sd[0] = std::max(1e-3, std::sqrt(var[0] / 120.0));
        sd[1] = std::max(1e-3, std::sqrt(var[1] / 120.0));
    }

    // 予測: クラスID(0-2)を返す。conf は 1 - d1/d2 のマージン信頼度。
    int predict(const Image& raw, double& conf) const {
        Image im = raw;
        preprocess(im);
        Features f;
        if (!extractFeatures(im, f)) { conf = 0.0; return -1; }
        double d[3];
        for (int c = 0; c < 3; c++) {
            double a = (f.fill - cen[c][0]) / sd[0];
            double b = (f.compact - cen[c][1]) / sd[1];
            d[c] = std::sqrt(a * a + b * b);
        }
        int best = 0;
        for (int c = 1; c < 3; c++) if (d[c] < d[best]) best = c;
        double second = 1e18;
        for (int c = 0; c < 3; c++) if (c != best) second = std::min(second, d[c]);
        conf = second > 1e-12 ? std::max(0.0, 1.0 - d[best] / second) : 0.0;
        return best;
    }
};

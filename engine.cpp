// ============================================================================
// self-healing-vision : 自己書き換え型 組み込み画像認識エンジン
//
//   通常運転中にウォッチドッグが認識エラーの多発を検知すると、
//     1. 失敗フレーム群を数理診断(diagnose.hpp)して根本原因を特定
//     2. 修正後の計算グラフパラメータを最適化で算出
//     3. 自分のソース(generated_params.hpp)を書き換え、g++で再ビルド
//     4. 新バイナリに回帰ゲート(--selftest)を課し、合格したら
//        状態を引き継いで新バイナリへ制御を移譲(不合格ならロールバック)
//
//   ビルド:  build.bat  →  engine_v1.exe
//   実行 :  engine_v1.exe   (このフォルダをカレントディレクトリにして実行)
// ============================================================================
#include "common.hpp"
#include "imagegen.hpp"
#include "generated_params.hpp"
#include "pipeline.hpp"
#include "diagnose.hpp"
#include "selfpatch.hpp"
#include <deque>
#include <fstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

// --- 運転パラメータ ---------------------------------------------------------
constexpr int      TOTAL_FRAMES   = 600;
constexpr uint64_t STREAM_SEED    = 20260712ull;
constexpr int      WINDOW         = 25;    // ウォッチドッグ監視窓
constexpr int      ERR_TRIGGER    = 6;     // 窓内エラー数がこれ以上で発火 (24%)
constexpr int      COOLDOWN       = 40;    // 修復試行後の再発火抑止フレーム数
constexpr int      MAX_REPAIRS    = 5;
constexpr double   GATE_CLEAN     = 0.95;  // 回帰ゲート: クリーン精度下限
constexpr double   GATE_GLARE     = 0.75;
constexpr double   GATE_IMPULSE   = 0.85;
constexpr const char* STATE_FILE  = "state.txt";

#define LOGV(fmt, ...) std::printf("[v%d] " fmt "\n", gen::PARAMS_VERSION, ##__VA_ARGS__)

// --- フレームストリーム(センサ入力のシミュレーション) -----------------------
// フレーム番号だけから決定的に生成されるので、プロセスを再起動しても
// 同じストリームの続きを受け取れる(=実機のカメラ入力の代役)。
struct Frame {
    Image   img;
    int     truth;      // 教師信号(実機では冗長系センサ/上位系からの遅延ラベルに相当)
    Corrupt corrupt;
};

static int phaseOf(int f) {
    if (f < 150) return 0;
    if (f < 300) return 1;
    if (f < 450) return 2;
    return 3;
}

static Frame makeFrame(int f) {
    Rng rng(STREAM_SEED * 1000003ull + (uint64_t)f);
    Frame fr;
    fr.truth = rng.irange(0, 2);
    double r = rng.uni();
    switch (phaseOf(f)) {
    case 0:  fr.corrupt = Corrupt::None; break;
    case 1:  fr.corrupt = r < 0.9 ? Corrupt::Glare   : Corrupt::None; break;
    case 2:  fr.corrupt = r < 0.9 ? Corrupt::Impulse : Corrupt::None; break;
    default: fr.corrupt = r < 1.0 / 3 ? Corrupt::None
                        : r < 2.0 / 3 ? Corrupt::Glare : Corrupt::Impulse;
    }
    fr.img = renderShape((Shape)fr.truth, rng);
    applyCorrupt(fr.img, fr.corrupt, rng);
    return fr;
}

// --- 実行状態(再起動をまたいで引き継ぐ) -------------------------------------
struct RunState {
    int frame = 0;
    int repairs = 0;
    int lastAttempt = -1000;
    int phCorrect[4] = {}, phTotal[4] = {};
};

static void saveState(const RunState& st) {
    std::ofstream out(STATE_FILE);
    out << st.frame << " " << st.repairs << " " << st.lastAttempt;
    for (int i = 0; i < 4; i++) out << " " << st.phCorrect[i] << " " << st.phTotal[i];
    out << "\n";
}

static bool loadState(RunState& st) {
    std::ifstream in(STATE_FILE);
    if (!in) return false;
    in >> st.frame >> st.repairs >> st.lastAttempt;
    for (int i = 0; i < 4; i++) in >> st.phCorrect[i] >> st.phTotal[i];
    return (bool)in;
}

// --- 回帰ゲート用セルフテスト ------------------------------------------------
// 固定シードのフレームで精度を測り、下限を満たせば exit 0。
static int selftest(const char* mode) {
    Corrupt c; double gate; uint64_t seedBase;
    if      (!std::strcmp(mode, "clean"))   { c = Corrupt::None;    gate = GATE_CLEAN;   seedBase = 500000; }
    else if (!std::strcmp(mode, "glare"))   { c = Corrupt::Glare;   gate = GATE_GLARE;   seedBase = 600000; }
    else if (!std::strcmp(mode, "impulse")) { c = Corrupt::Impulse; gate = GATE_IMPULSE; seedBase = 700000; }
    else { std::printf("unknown selftest mode: %s\n", mode); return 2; }

    Pipeline p; p.gp = activeParams(); p.calibrate();
    int ok = 0; const int N = 200;
    for (int i = 0; i < N; i++) {
        Rng rng(seedBase + (uint64_t)i * 31);
        int truth = rng.irange(0, 2);
        Image im = renderShape((Shape)truth, rng);
        applyCorrupt(im, c, rng);
        double conf;
        ok += p.predict(im, conf) == truth;
    }
    double acc = (double)ok / N;
    bool pass = acc >= gate;
    std::printf("  [gate] selftest %-7s : acc=%5.1f%% (下限%.0f%%) → %s\n",
                mode, acc * 100.0, gate * 100.0, pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass ? 0 : 1;
}

// --- 自己修復シーケンス -------------------------------------------------------
// 戻り値: true = 新バイナリへ移譲した(戻ってこない) / false = 修復せず続行
static bool attemptSelfRepair(const std::vector<LabeledImage>& fails,
                              const CalibStats& calib, RunState& st) {
    LOGV("┌─ WATCHDOG発火: 直近%dフレーム中%dフレームで認識エラー。自己診断を開始",
         WINDOW, (int)fails.size());

    Evidence ev;
    Cause cause = diagnose(fails, calib, ev);
    LOGV("│  数理診断: 飽和率=%.3f(校正時%.4f) 飽和凝集度=%.2f 孤立極値率=%.3f KL=%.2f",
         ev.satRatio, calib.satMean, ev.satCluster, ev.impulseFrac, ev.klDiv);
    LOGV("│  → 根本原因の判定: %s", causeName(cause));

    if (cause == Cause::Unknown) {
        LOGV("└─ 原因を特定できないためパッチを見送り(縮退運転を継続)");
        st.lastAttempt = st.frame;
        return false;
    }

    GraphParams cur = activeParams();
    double curAcc = evalParams(cur, fails, makeRegressionSet());
    FixResult fix = (cause == Cause::Glare) ? solveGlareFix(cur, fails)
                                            : solveImpulseFix(cur, fails);
    LOGV("│  修正候補: %s", fix.note.c_str());
    // 適用基準: 絶対精度90%以上、かつ現行グラフのエラー率を半減できること
    if (!fix.ok || (1.0 - fix.acc) > (1.0 - curAcc) * 0.5) {
        LOGV("└─ 棄却: 候補エラー率%.1f%%が基準(絶対精度90%%以上かつ現行%.1f%%の半減)を満たさない → 縮退運転を継続",
             (1.0 - fix.acc) * 100.0, (1.0 - curAcc) * 100.0);
        st.lastAttempt = st.frame;
        return false;
    }

    int newVer = gen::PARAMS_VERSION + 1;
    LOGV("│  自己書き換え: %s を v%d へ更新して再ビルド (%s)",
         PARAMS_FILE, newVer, exeName(newVer).c_str());
    if (!writeParamsHeader(fix.gp, newVer, fix.note)) {
        rollbackParams();
        LOGV("└─ ヘッダ書き出しに失敗。ロールバックして中止");
        st.lastAttempt = st.frame;
        return false;
    }
    if (!rebuildSelf(newVer)) {
        rollbackParams();
        LOGV("└─ 再コンパイル失敗(build_v%d.log参照)。パラメータをロールバック", newVer);
        return false;
    }

    LOGV("│  回帰ゲート実行中 (%s):", exeName(newVer).c_str());
    const char* fixedMode = (cause == Cause::Glare) ? "glare" : "impulse";
    bool gateOk = runGate(newVer, "clean") && runGate(newVer, fixedMode);
    if (!gateOk) {
        rollbackParams();
        std::remove(exeName(newVer).c_str());
        LOGV("└─ 回帰ゲート不合格。パッチを破棄しロールバック(劣化防止)");
        st.lastAttempt = st.frame;
        return false;
    }

    st.frame++;                    // 現フレームは処理済み
    st.repairs++;
    st.lastAttempt = st.frame;
    saveState(st);
    LOGV("└─ ゲート合格。状態をf%dで保存し %s へ制御を移譲 ── 再起動", st.frame, exeName(newVer).c_str());
    std::printf("────────────────────────────────────────────────────────\n");
    handOver(newVer);              // noreturn
}

// --- メインループ -------------------------------------------------------------
static int runEngine(bool resume) {
    RunState st;
    if (resume && !loadState(st)) {
        std::printf("state.txt を読めません\n");
        return 1;
    }
    if (!resume) std::remove(STATE_FILE);

    GraphParams gp = activeParams();
    LOGV("起動: 計算グラフ v%d { glare_clip=%s(th=%.2f) median3x3=%s } フレームf%dから%s",
         gen::PARAMS_VERSION,
         gp.glareClip ? "on" : "off", gp.glareThresh,
         gp.medianFilter ? "on" : "off",
         st.frame, resume ? "再開" : "開始");

    Pipeline pipe; pipe.gp = gp; pipe.calibrate();
    LOGV("セルフキャリブレーション完了: 重心 ○(%.2f,%.2f) □(%.2f,%.2f) △(%.2f,%.2f)",
         pipe.cen[0][0], pipe.cen[0][1], pipe.cen[1][0], pipe.cen[1][1],
         pipe.cen[2][0], pipe.cen[2][1]);

    CalibStats calib = computeCalibStats();
    std::deque<std::pair<bool, LabeledImage>> window;   // (正解?, フレーム)
    int blockOk = 0, blockN = 0;

    for (; st.frame < TOTAL_FRAMES; st.frame++) {
        Frame fr = makeFrame(st.frame);
        double conf;
        int pred = pipe.predict(fr.img, conf);
        bool ok = pred == fr.truth;

        int ph = phaseOf(st.frame);
        st.phCorrect[ph] += ok;
        st.phTotal[ph]++;
        blockOk += ok; blockN++;

        window.push_back({ ok, { fr.img, fr.truth } });
        if ((int)window.size() > WINDOW) window.pop_front();

        if (blockN == 25) {
            LOGV("f%03d-%03d: 精度 %2d/25 (%s帯)", st.frame - 24, st.frame, blockOk,
                 ph == 0 ? "クリーン" : ph == 1 ? "グレア混入" : ph == 2 ? "ノイズ混入" : "混合");
            blockOk = blockN = 0;
        }

        int errs = 0;
        for (auto& w : window) errs += !w.first;
        if ((int)window.size() == WINDOW && errs >= ERR_TRIGGER
            && st.frame >= st.lastAttempt + COOLDOWN && st.repairs < MAX_REPAIRS) {
            std::vector<LabeledImage> fails;
            for (auto& w : window)
                if (!w.first && (int)fails.size() < 15) fails.push_back(w.second);
            if (attemptSelfRepair(fails, calib, st))
                ;   // 移譲済み(ここには戻らない)
            window.clear();
            blockOk = blockN = 0;
        }
    }

    // 最終レポート
    std::printf("────────────────────────────────────────────────────────\n");
    LOGV("全%dフレーム完了。世代交代 %d回", TOTAL_FRAMES, st.repairs);
    const char* phName[4] = { "P0 クリーン      ", "P1 グレア混入    ",
                              "P2 ノイズ混入    ", "P3 全外乱ミックス" };
    for (int i = 0; i < 4; i++)
        if (st.phTotal[i])
            LOGV("  %s: %3d/%3d (%.1f%%)", phName[i], st.phCorrect[i], st.phTotal[i],
                 100.0 * st.phCorrect[i] / st.phTotal[i]);
    LOGV("計算グラフの進化履歴:");
    for (auto& h : readParamHistory())
        LOGV("  %s", h.c_str() + 2);
    std::remove(STATE_FILE);
    return 0;
}

int main(int argc, char** argv) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    // 子プロセス(再ビルド/ゲート)の出力と混ざらないよう無バッファ化
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    // ソースフォルダで実行しているかの防御(自己再ビルドに必要)
    { std::ifstream chk("engine.cpp");
      if (!chk) { std::printf("engine.cpp が見つかりません。self-healing-vision フォルダ内で実行してください。\n"); return 1; } }

    if (argc >= 3 && !std::strcmp(argv[1], "--selftest")) return selftest(argv[2]);
    bool resume = argc >= 2 && !std::strcmp(argv[1], "--resume");
    return runEngine(resume);
}

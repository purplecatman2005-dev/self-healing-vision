#pragma once
// 自己パッチモジュール:
//   1. 新しい generated_params.hpp (計算グラフ定義)を自分で書き出す
//   2. g++ で自分自身のソースを再ビルドし engine_v{N+1}.exe を生成
//   3. 新バイナリに --selftest 回帰ゲートを課す(不合格ならロールバック)
//   4. 合格したら状態ファイルを引き継いで新バイナリを起動し、自分は退役する
#include "common.hpp"
#include "pipeline.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>

constexpr const char* PARAMS_FILE = "generated_params.hpp";
constexpr const char* PARAMS_BAK  = "generated_params.hpp.bak";

inline std::string exeName(int ver) {
    return "engine_v" + std::to_string(ver) + ".exe";
}

// 既存ヘッダから履歴コメント("//   vN: ...")を回収
inline std::vector<std::string> readParamHistory() {
    std::vector<std::string> h;
    std::ifstream in(PARAMS_FILE);
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("//   v", 0) == 0) h.push_back(line);
    return h;
}

inline bool copyFile(const char* src, const char* dst) {
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary);
    out << in.rdbuf();
    return (bool)out;
}

// 新しい計算グラフ定義ヘッダを書き出す(自己書き換えの本体)
inline bool writeParamsHeader(const GraphParams& gp, int newVer, const std::string& note) {
    auto history = readParamHistory();
    copyFile(PARAMS_FILE, PARAMS_BAK);
    std::ofstream out(PARAMS_FILE);
    if (!out) return false;
    out << "// ===== AUTO-GENERATED COMPUTATION-GRAPH PARAMETERS =====\n";
    out << "// このファイルはエンジン自身(selfpatch.hpp)が書き換える。手で編集しないこと。\n";
    out << "// version: " << newVer << "\n";
    out << "// history:\n";
    for (auto& h : history) out << h << "\n";
    out << "//   v" << newVer << ": " << note << "\n";
    out << "#pragma once\n";
    out << "namespace gen {\n";
    out << "inline constexpr int   PARAMS_VERSION      = " << newVer << ";\n";
    out << "inline constexpr bool  STAGE_GLARE_CLIP    = " << (gp.glareClip ? "true" : "false") << ";\n";
    char th[32]; snprintf(th, sizeof th, "%.4ff", gp.glareThresh);
    out << "inline constexpr float GLARE_CLIP_THRESH   = " << th << ";\n";
    out << "inline constexpr bool  STAGE_MEDIAN_FILTER = " << (gp.medianFilter ? "true" : "false") << ";\n";
    out << "}\n";
    return (bool)out;
}

inline void rollbackParams() {
    copyFile(PARAMS_BAK, PARAMS_FILE);
}

// 自分自身のソースを再コンパイル
inline bool rebuildSelf(int newVer) {
    std::string cmd = "g++ -O2 -std=c++20 -static engine.cpp -o " + exeName(newVer)
                    + " > build_v" + std::to_string(newVer) + ".log 2>&1";
    int rc = std::system(cmd.c_str());
    if (rc != 0) return false;
    std::ifstream chk(exeName(newVer), std::ios::binary);
    return (bool)chk;
}

// 新バイナリへの回帰ゲート(exit 0 = 合格)
// ※cmd.exe がカレントディレクトリのexeを解決しない環境があるため ".\" を明示
inline bool runGate(int newVer, const char* mode) {
    std::string cmd = ".\\" + exeName(newVer) + " --selftest " + mode;
    std::fflush(stdout);
    return std::system(cmd.c_str()) == 0;
}

// 新バイナリへ制御を移譲(状態ファイルは呼び出し側が保存済み)
[[noreturn]] inline void handOver(int newVer) {
    std::fflush(stdout);
    std::string cmd = ".\\" + exeName(newVer) + " --resume";
    int rc = std::system(cmd.c_str());
    std::exit(rc == 0 ? 0 : 1);
}

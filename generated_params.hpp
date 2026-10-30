// ===== AUTO-GENERATED COMPUTATION-GRAPH PARAMETERS =====
// このファイルはエンジン自身(selfpatch.hpp)が書き換える。手で編集しないこと。
// build.bat がこの baseline を generated_params.hpp へコピーして初期状態に戻す。
// version: 1
// history:
//   v1: baseline (defensive preprocessing stages: none)
#pragma once
namespace gen {
inline constexpr int   PARAMS_VERSION      = 1;
inline constexpr bool  STAGE_GLARE_CLIP    = false;   // 飽和画素クリップ+インペイント段
inline constexpr float GLARE_CLIP_THRESH   = 1.00f;   // 飽和判定閾値
inline constexpr bool  STAGE_MEDIAN_FILTER = false;   // 3x3メディアンフィルタ段
}

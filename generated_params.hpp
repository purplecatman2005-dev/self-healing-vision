// ===== AUTO-GENERATED COMPUTATION-GRAPH PARAMETERS =====
// このファイルはエンジン自身(selfpatch.hpp)が書き換える。手で編集しないこと。
// version: 3
// history:
//   v1: baseline (defensive preprocessing stages: none)
//   v2: glare-clip段を有効化 thresh=0.96 (グリッド探索: 最大精度95.5%のプラトー[0.92,0.99]の中点)
//   v3: median3x3段をグラフに追加 (失敗フレーム+回帰セットで精度100.0%を確認)
#pragma once
namespace gen {
inline constexpr int   PARAMS_VERSION      = 3;
inline constexpr bool  STAGE_GLARE_CLIP    = true;
inline constexpr float GLARE_CLIP_THRESH   = 0.9600f;
inline constexpr bool  STAGE_MEDIAN_FILTER = true;
}

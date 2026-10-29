@echo off
rem Reset to baseline (v1) and build. Also serves as the demo reset.
cd /d %~dp0
copy /y params_baseline.hpp generated_params.hpp >nul
del /q engine_v*.exe state.txt build_v*.log generated_params.hpp.bak 2>nul
g++ -O2 -std=c++20 -static engine.cpp -o engine_v1.exe
if errorlevel 1 (
  echo BUILD FAILED
  exit /b 1
)
echo OK: engine_v1.exe

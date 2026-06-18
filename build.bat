@echo off
setlocal
echo [Helix] Configuring...
cmake -B build -A x64
if %errorlevel% neq 0 (
  echo [Helix] Configure failed.
  pause
  exit /b 1
)

echo [Helix] Building...
cmake --build build --config Release
if %errorlevel% neq 0 (
  echo [Helix] Build failed.
  pause
  exit /b 1
)

echo.
echo [Helix] Done! Run: build\Release\Helix.exe

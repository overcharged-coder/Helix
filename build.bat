@echo off
setlocal
echo [Felix] Configuring...
cmake -B build -A x64
if %errorlevel% neq 0 ( echo [Felix] Configure failed. & pause & exit /b 1 )

echo [Felix] Building...
cmake --build build --config Release
if %errorlevel% neq 0 ( echo [Felix] Build failed. & pause & exit /b 1 )

echo.
echo [Felix] Done!  Run:  build\Felix.exe

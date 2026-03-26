@echo off
setlocal

set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
set CC="C:\Users\sould\Desktop\w64devkit\bin\x86_64-w64-mingw32-gcc.exe"
set CXX="C:\Users\sould\Desktop\w64devkit\bin\x86_64-w64-mingw32-g++.exe"

if not exist build mkdir build

%CMAKE% -S . -B build -DCMAKE_C_COMPILER=%CC% -DCMAKE_CXX_COMPILER=%CXX% -G "Ninja"
if %ERRORLEVEL% neq 0 ( echo [!] Configure failed. & exit /b %ERRORLEVEL% )

%CMAKE% --build build -- -j8
if %ERRORLEVEL% neq 0 ( echo [!] Build failed. & exit /b %ERRORLEVEL% )

copy /y build\imgui_bundle_example_integration.exe out.exe
echo [+] Done. Output: out.exe

out.exe
endlocal
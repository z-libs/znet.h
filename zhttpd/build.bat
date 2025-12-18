
@echo off
setlocal

echo [Windows] Downloading dependencies...

curl -s -L "https://raw.githubusercontent.com/z-libs/zerror.h/main/zerror.h" -o zerror.h
curl -s -L "https://raw.githubusercontent.com/z-libs/zstr.h/main/zstr.h" -o zstr.h
curl -s -L "https://raw.githubusercontent.com/z-libs/zfile.h/main/zfile.h" -o zfile.h
curl -s -L "https://raw.githubusercontent.com/z-libs/zthread.h/main/zthread.h" -o zthread.h

if exist "..\znet.h" (
    echo [Windows] Copying znet.h...
    copy /Y "..\znet.h" "." >nul
) else (
    echo [Error] Could not find ..\znet.h
    goto :error
)

echo [Windows] Compiling zhttpd.exe...
gcc zhttpd.c -o zhttpd.exe -std=c11 -lws2_32

if %errorlevel% neq 0 goto :error

echo [Windows] Build Success! Run zhttpd.exe to start.
goto :eof

:error
echo [Windows] Build Failed.
exit /b 1

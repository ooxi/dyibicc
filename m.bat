@echo off
if '%1==' goto :err
call python src/gen.py
call third_party\ninja\ninja-win.exe -C out\w%1 %2 %3 %4 %5
goto :EOF
:err
echo usage: m d^|r^|a [target]

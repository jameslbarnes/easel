@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64
cl.exe /EHsc /std:c++17 C:\Users\james\easel\tests\test_wasapi_capture.cpp /Fe:C:\Users\james\easel\build\test_wasapi.exe ole32.lib

call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
nmake "CXXFLAGS=/O2 /nologo" "CFLAGS=/O2 /nologo" /k /f NMakefile
main.exe --logo --exit
nmake "CXXFLAGS=/O2 /nologo" "CFLAGS=/O2 /nologo" /k /f NMakefile test\reconst\main.exe test\sysinfo\main.exe

@echo off
setlocal
where cl >nul 2>&1
if errorlevel 1 (
  echo Run from a Visual Studio Developer Command Prompt.
  exit /b 1
)
pushd "%~dp0"
if not exist build mkdir build
copy /Y "..\..\src\fah\client\CPUResources.cpp" "build\CPUResources.cpp" >nul
copy /Y "..\..\src\fah\client\CPUResources.h" "build\CPUResources.h" >nul
copy /Y "..\..\src\fah\client\CPUExecutionPlan.h" "build\CPUExecutionPlan.h" >nul
copy /Y "..\..\src\fah\client\CPUExecutionPlan.cpp" "build\CPUExecutionPlan.cpp" >nul
cl /nologo /EHsc /std:c++17 /I"stub" /I"build" "build\CPUResources.cpp" "build\CPUExecutionPlan.cpp" test.cpp /Fo"build/" /Fe"build\allocator-test.exe"
if errorlevel 1 (
  popd
  exit /b 1
)
build\allocator-test.exe
if errorlevel 1 (
  popd
  exit /b 1
)
cl /nologo /EHsc /std:c++17 /I"stub" /I"build" "build\CPUResources.cpp" "build\CPUExecutionPlan.cpp" gpu-reservation.cpp /Fo"build/" /Fe"build\gpu-reservation-test.exe"
if errorlevel 1 (
  popd
  exit /b 1
)
build\gpu-reservation-test.exe
if errorlevel 1 (popd & exit /b 1)
cl /nologo /EHsc /std:c++17 /I"stub" /I"build" "build\CPUResources.cpp" "build\CPUExecutionPlan.cpp" process-policy.cpp /Fo"build/" /Fe"build\process-policy-test.exe"
if errorlevel 1 (popd & exit /b 1)
build\process-policy-test.exe
set TaskQaExit=%errorlevel%
popd
exit /b %TaskQaExit%

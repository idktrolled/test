@echo off

:: You might need to change this path to point to your Visual Studio installation's MSBuild.exe
set msbuildPathCom="C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\MSBuild\Current\Bin\MSBuild.exe"
set msbuildPathPro="C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\Current\Bin\MSBuild.exe"

if exist %msbuildPathPro% (
	set msbuildPath=%msbuildPathPro%
) else (
	set msbuildPath=%msbuildPathCom%
)

if not exist %msbuildPath% (
	echo Error: No Visual Studio 2022 installation found
	pause
	exit /b 1
)

echo Using Visual Studio installation: %msbuildPath%
echo.

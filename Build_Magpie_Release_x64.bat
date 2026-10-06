@echo off
title DLSS5-AnyWindow Build

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -NoExit -Command ^
"$env:Path='C:\Users\Jeff\.pyenv\pyenv-win\versions\3.10.11\Scripts;C:\Users\Jeff\.pyenv\pyenv-win\versions\3.10.11;' + $env:Path; ^
Set-Location 'C:\Users\Jeff\DLSS5-AnyWindow'; ^
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' '.\Magpie.slnx' /m /p:Configuration=Release /p:Platform=x64 /p:RestorePackagesConfig=true; ^
Write-Host ''; ^
if ($LASTEXITCODE -eq 0) { ^
    Write-Host '============================================'; ^
    Write-Host 'BUILD SUCCEEDED'; ^
    Write-Host '============================================'; ^
    Write-Host 'C:\Users\Jeff\DLSS5-AnyWindow\bin\x64\Release\Magpie.exe'; ^
} else { ^
    Write-Host '============================================'; ^
    Write-Host ('BUILD FAILED - Exit code ' + $LASTEXITCODE); ^
    Write-Host '============================================'; ^
}"
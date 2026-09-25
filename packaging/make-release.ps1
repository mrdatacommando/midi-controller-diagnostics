# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Mark Van de Velde
#
# Builds a clean Release, runs the self-test, and writes the distributable zip
# to releases/. Run it from anywhere:
#
#     powershell -ExecutionPolicy Bypass -File packaging\make-release.ps1
#
# Pass -SkipTests to package without running the self-test (not recommended).

[CmdletBinding()]
param(
    [switch]$SkipTests,
    [string]$Generator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    # Version comes from CMakeLists.txt so there is only one place to bump it.
    $cmake = Get-Content 'CMakeLists.txt' -Raw
    if ($cmake -notmatch 'project\(\s*MidiScope\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
        throw 'Could not read the version out of CMakeLists.txt.'
    }
    $version = $Matches[1]
    Write-Host "MidiScope $version" -ForegroundColor Cyan

    Get-Process MidiScope -ErrorAction SilentlyContinue | Stop-Process -Force

    Write-Host 'Configuring...' -ForegroundColor Cyan
    cmake -S . -B build -G $Generator -A x64 | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }

    Write-Host 'Building...' -ForegroundColor Cyan
    $log = cmake --build build --config Release 2>&1
    if ($LASTEXITCODE -ne 0) { $log | Write-Host; throw 'Build failed.' }

    $issues = @($log | Select-String -Pattern ': error|: warning')
    if ($issues.Count -gt 0) {
        $issues | ForEach-Object { Write-Warning $_.ToString() }
        throw "Build produced $($issues.Count) warning(s) or error(s); not packaging."
    }

    if (-not $SkipTests) {
        Write-Host 'Running self-test...' -ForegroundColor Cyan
        & '.\build\Release\MidiScopeSelfTest.exe' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Self-test failed; not packaging.' }
    }

    $stage = Join-Path ([System.IO.Path]::GetTempPath()) "MidiScope-stage-$version"
    Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stage | Out-Null

    Copy-Item 'build\Release\MidiScope.exe' $stage
    Copy-Item 'LICENSE'                     $stage
    Copy-Item 'README.md'                   $stage
    Copy-Item 'packaging\HOW-TO-RUN.txt'    $stage

    New-Item -ItemType Directory -Force -Path 'releases' | Out-Null
    $zip = "releases\MidiScope-v$version-win64.zip"
    Remove-Item $zip -Force -ErrorAction SilentlyContinue
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
    Remove-Item -Recurse -Force $stage

    # Written to a sidecar rather than pasted into the README, which would go
    # stale every time the zip is rebuilt.
    $info = Get-Item $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
    "$hash *$(Split-Path $zip -Leaf)" |
        Set-Content "$zip.sha256" -Encoding ascii -NoNewline

    Write-Host ''
    Write-Host ("Wrote {0} ({1:N0} bytes)" -f $info.Name, $info.Length) -ForegroundColor Green
    Write-Host "SHA256: $hash"
}
finally {
    Pop-Location
}

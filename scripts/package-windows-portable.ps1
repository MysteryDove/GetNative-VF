[CmdletBinding()]
param(
    [string]$Backends = $(if ($env:GETNATIVE_WINDOWS_BACKENDS) {
        $env:GETNATIVE_WINDOWS_BACKENDS
    } else {
        "cuda-vulkan"
    })
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repository = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$appDirectory = Join-Path $repository "app"
$ffmpegRoot = if ($env:GETNATIVE_FFMPEG_ROOT) {
    $env:GETNATIVE_FFMPEG_ROOT
} else {
    Join-Path $repository ".deps\ffmpeg-windows-x64"
}
$ffmpegRuntime = if ($env:GETNATIVE_FFMPEG_RUNTIME_DIR) {
    $env:GETNATIVE_FFMPEG_RUNTIME_DIR
} else {
    Join-Path $ffmpegRoot "bin"
}

function Replace-EnvironmentEntry([string]$Name, [string]$Value) {
    foreach ($key in @([Environment]::GetEnvironmentVariables("Process").Keys)) {
        if ($key -ieq $Name) {
            Remove-Item -LiteralPath "Env:$key" -ErrorAction SilentlyContinue
        }
    }
    Set-Item -Path "Env:$Name" -Value $Value
}

function Import-VisualStudioEnvironment {
    if ($env:VSCMD_VER) { return }
    $devCommand = if ($env:GETNATIVE_VSDEVCMD) {
        $env:GETNATIVE_VSDEVCMD
    } else {
        "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
    }
    if (-not (Test-Path -LiteralPath $devCommand -PathType Leaf)) {
        throw "Visual Studio developer command file was not found: $devCommand"
    }
    $lines = & cmd.exe /d /s /c "call `"$devCommand`" -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) {
        throw "Visual Studio developer environment initialization failed"
    }
    foreach ($line in $lines) {
        $separator = $line.IndexOf("=")
        if ($separator -le 0) { continue }
        Replace-EnvironmentEntry $line.Substring(0, $separator) $line.Substring($separator + 1)
    }
}

foreach ($name in @("VULKAN_SDK", "CUDA_PATH", "GETNATIVE_VSDEVCMD", "GETNATIVE_FFMPEG_ROOT", "GETNATIVE_FFMPEG_RUNTIME_DIR")) {
    if (-not [Environment]::GetEnvironmentVariable($name, "Process")) {
        $value = [Environment]::GetEnvironmentVariable($name, "User")
        if (-not $value) {
            $value = [Environment]::GetEnvironmentVariable($name, "Machine")
        }
        if ($value) { Set-Item -Path "Env:$name" -Value $value }
    }
}

$env:Path = @(
    "$env:USERPROFILE\.cargo\bin",
    "$env:LOCALAPPDATA\pnpm\bin",
    "C:\Program Files\nodejs",
    $(if ($env:CUDA_PATH) { Join-Path $env:CUDA_PATH "bin" }),
    $(if ($env:VULKAN_SDK) { Join-Path $env:VULKAN_SDK "Bin" }),
    "C:\Python314",
    $env:Path
) -ne $null -join ";"

Import-VisualStudioEnvironment

if (-not (Test-Path -LiteralPath (Join-Path $ffmpegRuntime "avformat-62.dll"))) {
    throw "Pinned FFmpeg runtime is missing: $ffmpegRuntime. Build it with scripts/build-ffmpeg-windows.sh first."
}

$env:GETNATIVE_WINDOWS_BACKENDS = $Backends
$env:GETNATIVE_FFMPEG_ROOT = $ffmpegRoot
$env:GETNATIVE_FFMPEG_RUNTIME_DIR = $ffmpegRuntime
if (-not $env:RUSTFLAGS) {
    $env:RUSTFLAGS = "-C target-feature=+crt-static"
}

Write-Host "Packaging Windows portable build"
Write-Host "  backends=$env:GETNATIVE_WINDOWS_BACKENDS"
Write-Host "  ffmpeg=$env:GETNATIVE_FFMPEG_RUNTIME_DIR"
Write-Host "  vulkan=$env:VULKAN_SDK"
Write-Host "  cuda=$env:CUDA_PATH"

Push-Location $appDirectory
try {
    if (-not (Test-Path "node_modules")) {
        pnpm install --frozen-lockfile
        if ($LASTEXITCODE -ne 0) { throw "pnpm install failed" }
    }
    pnpm tauri build --no-bundle --ci
    if ($LASTEXITCODE -ne 0) { throw "pnpm tauri build failed" }
} finally {
    Pop-Location
}

$gui = Join-Path $appDirectory "src-tauri\target\release\getnative-gui.exe"
$stage = Join-Path $appDirectory "src-tauri\bundle-stage"
if (-not (Test-Path -LiteralPath $gui)) {
    throw "GUI binary was not produced: $gui"
}
if (-not (Test-Path -LiteralPath (Join-Path $stage "bin\getnative-engine.exe"))) {
    throw "Staged engine was not produced"
}

$version = (Get-Content (Join-Path $appDirectory "src-tauri\tauri.conf.json") -Raw |
    ConvertFrom-Json).version
$portableName = "GetNative VF_${version}_x64-portable"
$artifactRoot = Join-Path $repository "artifacts\windows-x64"
$portableRoot = Join-Path $artifactRoot $portableName
$archive = Join-Path $artifactRoot "$portableName.zip"

Remove-Item -LiteralPath $portableRoot -Recurse -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $archive -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $portableRoot | Out-Null
Copy-Item -LiteralPath $gui -Destination $portableRoot
Copy-Item -Path (Join-Path $stage "*") -Destination $portableRoot -Recurse
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -LiteralPath $portableRoot -DestinationPath $archive -CompressionLevel Optimal

$engine = Join-Path $portableRoot "bin\getnative-engine.exe"
$capabilities = & $engine capabilities | ConvertFrom-Json
if ($LASTEXITCODE -ne 0) { throw "portable engine failed to start" }
if ($capabilities.media.ffmpeg_abi -ne "62.62.60.9") {
    throw "unexpected FFmpeg ABI: $($capabilities.media.ffmpeg_abi)"
}
$cuda = @($capabilities.backends | Where-Object { $_.id -eq "cuda" })
$vulkan = @($capabilities.backends | Where-Object { $_.id -eq "vulkan" })
if ($Backends -match "cuda" -and ($cuda.Count -ne 1 -or -not $cuda[0].compiled)) {
    throw "portable engine is missing a compiled CUDA backend"
}
if ($Backends -match "vulkan" -and ($vulkan.Count -ne 1 -or -not $vulkan[0].compiled)) {
    throw "portable engine is missing a compiled Vulkan backend"
}

Write-Host "portable_dir=$portableRoot"
Write-Host "portable_zip=$archive"
Write-Host "cuda_compiled=$($cuda[0].compiled) device=$($cuda[0].device_available)"
Write-Host "vulkan_compiled=$($vulkan[0].compiled) device=$($vulkan[0].device_available)"

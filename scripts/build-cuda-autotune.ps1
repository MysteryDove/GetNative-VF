[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$getnativeScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$getnativeRepository = [System.IO.Path]::GetFullPath(
    (Join-Path $getnativeScriptDirectory ".."))
$getnativeSource = Join-Path $getnativeRepository "engine"
$getnativeBuild = Join-Path $getnativeRepository "build\cuda-autotune"
$getnativeArtifactDirectory = Join-Path $getnativeRepository "artifacts\cuda-autotune"

$getnativeDefaultVsDev =
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
$getnativeVsDev = if ($env:GETNATIVE_VSDEVCMD) {
    $env:GETNATIVE_VSDEVCMD
} else {
    $getnativeDefaultVsDev
}
if (-not (Test-Path -LiteralPath $getnativeVsDev -PathType Leaf)) {
    throw "Visual Studio developer command file was not found: $getnativeVsDev"
}

$getnativeEnvironmentCommand =
    'call "' + $getnativeVsDev + '" -arch=x64 -host_arch=x64 >nul && set'
$getnativeEnvironmentLines = & cmd.exe /d /s /c $getnativeEnvironmentCommand
if ($LASTEXITCODE -ne 0) {
    throw "Visual Studio developer environment initialization failed"
}
foreach ($getnativeLine in $getnativeEnvironmentLines) {
    $getnativeSeparator = $getnativeLine.IndexOf('=')
    if ($getnativeSeparator -le 0) { continue }
    $getnativeName = $getnativeLine.Substring(0, $getnativeSeparator)
    $getnativeValue = $getnativeLine.Substring($getnativeSeparator + 1)
    [Environment]::SetEnvironmentVariable(
        $getnativeName, $getnativeValue, [EnvironmentVariableTarget]::Process)
}

$getnativeVisualStudioCMakeRoot =
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake"
$getnativeBundledCmake = Join-Path $getnativeVisualStudioCMakeRoot "CMake\bin\cmake.exe"
$getnativeBundledNinja = Join-Path $getnativeVisualStudioCMakeRoot "Ninja\ninja.exe"
$getnativeCmake = if ($env:CMAKE) {
    $env:CMAKE
} elseif (Test-Path -LiteralPath $getnativeBundledCmake -PathType Leaf) {
    $getnativeBundledCmake
} else {
    (Get-Command cmake.exe -ErrorAction Stop).Source
}

$getnativeMinimumArchitecture = if ($env:GETNATIVE_CUDA_MIN_ARCHITECTURE) {
    $env:GETNATIVE_CUDA_MIN_ARCHITECTURE
} else {
    "75"
}
$getnativeArchitectures = if ($env:GETNATIVE_CUDA_ARCHITECTURES) {
    $env:GETNATIVE_CUDA_ARCHITECTURES
} else {
    "75;86;89;120"
}
$getnativePtxArchitectures = if ($env:GETNATIVE_CUDA_PTX_ARCHITECTURES) {
    $env:GETNATIVE_CUDA_PTX_ARCHITECTURES
} else {
    "75;120"
}

$getnativeConfigureArguments = @(
    "-S", $getnativeSource,
    "-B", $getnativeBuild,
    "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
    "-DBUILD_TESTING=OFF",
    "-DGETNATIVE_ENABLE_METAL=OFF",
    "-DGETNATIVE_ENABLE_CUDA=ON",
    "-DGETNATIVE_BUILD_CUDA_AUTOTUNER=ON",
    "-DGETNATIVE_ENABLE_X86_SIMD=ON",
    "-DGETNATIVE_ENABLE_X86_AVX512=ON",
    "-DGETNATIVE_CUDA_MIN_ARCHITECTURE=$getnativeMinimumArchitecture",
    "-DGETNATIVE_CUDA_ARCHITECTURES=$getnativeArchitectures",
    "-DGETNATIVE_CUDA_PTX_ARCHITECTURES=$getnativePtxArchitectures"
)
if (Test-Path -LiteralPath $getnativeBundledNinja -PathType Leaf) {
    $getnativeConfigureArguments +=
        "-DCMAKE_MAKE_PROGRAM=$getnativeBundledNinja"
}

Write-Host "Configuring standalone CUDA autotuner..."
& $getnativeCmake @getnativeConfigureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

Write-Host "Building standalone CUDA autotuner..."
& $getnativeCmake --build $getnativeBuild --target getnative_cuda_autotune `
    --clean-first --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

$getnativeBuiltExecutable = Join-Path $getnativeBuild "getnative-cuda-autotune.exe"
if (-not (Test-Path -LiteralPath $getnativeBuiltExecutable -PathType Leaf)) {
    throw "Autotuner executable was not produced: $getnativeBuiltExecutable"
}

$getnativeDumpbin = (Get-Command dumpbin.exe -ErrorAction Stop).Source
$getnativeDependencyOutput = & $getnativeDumpbin /nologo /dependents $getnativeBuiltExecutable
if ($LASTEXITCODE -ne 0) { throw "dumpbin dependency inspection failed" }
$getnativeDependencies = @(
    $getnativeDependencyOutput |
        ForEach-Object {
            if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1] }
        } |
        Sort-Object -Unique
)
if ($getnativeDependencies -match '^(VCRUNTIME|MSVCP|ucrtbase)') {
    throw "The exported executable still depends on a dynamic MSVC runtime"
}

New-Item -ItemType Directory -Path $getnativeArtifactDirectory -Force | Out-Null
$getnativeExportedExecutable =
    Join-Path $getnativeArtifactDirectory "getnative-cuda-autotune.exe"
Copy-Item -LiteralPath $getnativeBuiltExecutable `
    -Destination $getnativeExportedExecutable -Force

$getnativeHash = (Get-FileHash -LiteralPath $getnativeExportedExecutable `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$getnativeSize = (Get-Item -LiteralPath $getnativeExportedExecutable).Length
$getnativeManifest = [ordered]@{
    schema_version = 1
    artifact = "getnative-cuda-autotune.exe"
    tool_version = "1.4.0"
    report_schema_version = 2
    sha256 = $getnativeHash
    size_bytes = $getnativeSize
    minimum_compute_capability = "7.5"
    native_architectures = $getnativeArchitectures
    ptx_architectures = $getnativePtxArchitectures
    target_compute_capabilities = @("sm_75", "sm_86", "sm_89", "sm_120")
    runtime_architecture_whitelist_enforced = $true
    imported_dlls = $getnativeDependencies
    cuda_toolkit_required_on_runner = $false
    nvidia_driver_required_on_runner = $true
    default_mode = "survey"
    available_modes = @("survey", "quick", "standard")
    candidate_scope = "launch_policy_v1"
    candidate_policy_count = 14
    standard_candidate_policy_count = 36
    workload_contract_version = "descale-launch-v5"
    survey_screening_workload_count = 8
    survey_confirmation_workload_count = 8
    survey_maximum_confirmation_finalists = 2
    survey_can_recommend_production_policy = $false
    filter_families = @(
        "bilinear", "bicubic", "lanczos3", "spline64", "lanczos8")
    filter_topologies = @("B2", "B4", "B6", "B8", "B16-wide")
    concurrency_search_included = $false
    device_calibrated_sample_lengths = $true
    timing_scope = "steady_state_backend_api_e2e"
    hardware_performance_counters_required = $false
    production_configuration_is_modified = $false
}
$getnativeManifestPath = Join-Path $getnativeArtifactDirectory "manifest.json"
$getnativeManifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $getnativeManifestPath -Encoding utf8

Write-Host "artifact=$getnativeExportedExecutable"
Write-Host "sha256=$getnativeHash"
Write-Host "size_bytes=$getnativeSize"
Write-Host "manifest=$getnativeManifestPath"

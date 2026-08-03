[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$getnativeZigVersion = "0.15.2"
$getnativeZigArchiveSha256 =
    "3a0ed1e8799a2f8ce2a6e6290a9ff22e6906f8227865911fb7ddedc3cc14cb0c"
$getnativeZigArchiveUrl =
    "https://ziglang.org/download/$getnativeZigVersion/zig-x86_64-windows-$getnativeZigVersion.zip"
$getnativeLinuxTarget = "x86_64-linux-gnu.2.39"
$getnativeMaximumGlibcVersion = [version]"2.39"
$getnativeExpectedInterpreter = "/lib64/ld-linux-x86-64.so.2"
$getnativeExpectedSharedLibraries = @(
    "ld-linux-x86-64.so.2",
    "libc.so.6",
    "libm.so.6"
)

function Get-GetNativeNullTerminatedAscii {
    param(
        [Parameter(Mandatory)] [byte[]] $Bytes,
        [Parameter(Mandatory)] [long] $Offset,
        [Parameter(Mandatory)] [long] $Limit
    )

    if ($Offset -lt 0 -or $Limit -gt $Bytes.LongLength -or $Offset -ge $Limit) {
        throw "Invalid ELF string range [$Offset, $Limit)"
    }
    $getnativeEnd = $Offset
    while ($getnativeEnd -lt $Limit -and $Bytes[$getnativeEnd] -ne 0) {
        ++$getnativeEnd
    }
    if ($getnativeEnd -eq $Limit) {
        throw "ELF string at offset $Offset is not null terminated"
    }
    return [System.Text.Encoding]::ASCII.GetString(
        $Bytes, [int]$Offset, [int]($getnativeEnd - $Offset))
}

function Convert-GetNativeElfAddressToOffset {
    param(
        [Parameter(Mandatory)] [long] $Address,
        [Parameter(Mandatory)] [long] $Size,
        [Parameter(Mandatory)] [object[]] $LoadSegments,
        [Parameter(Mandatory)] [long] $FileSize
    )

    foreach ($getnativeSegment in $LoadSegments) {
        $getnativeRelative = $Address - $getnativeSegment.VirtualAddress
        if ($getnativeRelative -ge 0 -and
            $getnativeRelative + $Size -le $getnativeSegment.FileSize) {
            $getnativeOffset = $getnativeSegment.Offset + $getnativeRelative
            if ($getnativeOffset -ge 0 -and
                $getnativeOffset + $Size -le $FileSize) {
                return [long]$getnativeOffset
            }
        }
    }
    throw "ELF virtual address 0x$($Address.ToString('x')) is not file-backed"
}

$getnativeScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$getnativeRepository = [System.IO.Path]::GetFullPath(
    (Join-Path $getnativeScriptDirectory ".."))
$getnativeSource = Join-Path $getnativeRepository "engine"
$getnativeBuild = Join-Path $getnativeRepository `
    "build\cuda-autotune-linux-x86_64"
$getnativeArtifactDirectory = Join-Path $getnativeRepository `
    "artifacts\cuda-autotune-linux-x86_64"
$getnativeToolRoot = Join-Path $getnativeRepository "build\cross-tools"

foreach ($getnativePath in @(
        $getnativeBuild, $getnativeArtifactDirectory, $getnativeToolRoot)) {
    $getnativeResolved = [System.IO.Path]::GetFullPath($getnativePath)
    if (-not $getnativeResolved.StartsWith(
            $getnativeRepository,
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cross-build path escaped the repository: $getnativeResolved"
    }
}

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
$getnativeCmake = if ($env:CMAKE) {
    $env:CMAKE
} else {
    Join-Path $getnativeVisualStudioCMakeRoot "CMake\bin\cmake.exe"
}
$getnativeNinja = Join-Path $getnativeVisualStudioCMakeRoot "Ninja\ninja.exe"
foreach ($getnativeTool in @($getnativeCmake, $getnativeNinja)) {
    if (-not (Test-Path -LiteralPath $getnativeTool -PathType Leaf)) {
        throw "Required cross-build tool was not found: $getnativeTool"
    }
}

$getnativeCudaRoot = if ($env:GETNATIVE_CUDA_PATH) {
    $env:GETNATIVE_CUDA_PATH
} elseif ($env:CUDA_PATH) {
    $env:CUDA_PATH
} else {
    "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
}
$getnativeNvcc = Join-Path $getnativeCudaRoot "bin\nvcc.exe"
$getnativeCuobjdump = Join-Path $getnativeCudaRoot "bin\cuobjdump.exe"
$getnativeCudaInclude = Join-Path $getnativeCudaRoot "include"
foreach ($getnativeTool in @(
        $getnativeNvcc, $getnativeCuobjdump,
        (Join-Path $getnativeCudaInclude "cuda.h"))) {
    if (-not (Test-Path -LiteralPath $getnativeTool)) {
        throw "CUDA cross-build input was not found: $getnativeTool"
    }
}
$env:CUDA_PATH = $getnativeCudaRoot

New-Item -ItemType Directory -Path $getnativeToolRoot -Force | Out-Null
$getnativeZigArchive = Join-Path $getnativeToolRoot `
    "zig-x86_64-windows-$getnativeZigVersion.zip"
$getnativeZigRoot = Join-Path $getnativeToolRoot `
    "zig-x86_64-windows-$getnativeZigVersion"
$getnativeBundledZig = Join-Path $getnativeZigRoot "zig.exe"
$getnativeZig = if ($env:GETNATIVE_ZIG) {
    $env:GETNATIVE_ZIG
} else {
    $getnativeBundledZig
}

if (-not (Test-Path -LiteralPath $getnativeZig -PathType Leaf)) {
    if ($env:GETNATIVE_ZIG) {
        throw "GETNATIVE_ZIG does not name a file: $getnativeZig"
    }
    if (-not (Test-Path -LiteralPath $getnativeZigArchive -PathType Leaf)) {
        Write-Host "Downloading Zig $getnativeZigVersion..."
        & curl.exe --fail --location --silent --show-error `
            --output $getnativeZigArchive $getnativeZigArchiveUrl
        if ($LASTEXITCODE -ne 0) { throw "Zig download failed" }
    }
    $getnativeZigArchiveHash = (
        Get-FileHash -LiteralPath $getnativeZigArchive -Algorithm SHA256
    ).Hash.ToLowerInvariant()
    if ($getnativeZigArchiveHash -ne $getnativeZigArchiveSha256) {
        throw "Zig archive SHA-256 mismatch: $getnativeZigArchiveHash"
    }
    Write-Host "Extracting Zig $getnativeZigVersion..."
    & tar.exe -xf $getnativeZigArchive -C $getnativeToolRoot
    if ($LASTEXITCODE -ne 0) { throw "Zig extraction failed" }
}
if (-not (Test-Path -LiteralPath $getnativeZig -PathType Leaf)) {
    throw "Zig executable was not produced: $getnativeZig"
}
$env:GETNATIVE_ZIG = [System.IO.Path]::GetFullPath($getnativeZig)
$getnativeDetectedZigVersion = (& $getnativeZig version).Trim()
if ($LASTEXITCODE -ne 0) { throw "Zig version probe failed" }

New-Item -ItemType Directory -Path $getnativeBuild -Force | Out-Null
$getnativeWrapperDirectory = Join-Path $getnativeBuild "zig-wrappers"
New-Item -ItemType Directory -Path $getnativeWrapperDirectory -Force | Out-Null
$getnativeAr = Join-Path $getnativeWrapperDirectory "zig-ar.cmd"
$getnativeRanlib = Join-Path $getnativeWrapperDirectory "zig-ranlib.cmd"
$getnativeUtf8NoBom = [System.Text.UTF8Encoding]::new($false)
[System.IO.File]::WriteAllText(
    $getnativeAr,
    "@echo off`r`n`"%GETNATIVE_ZIG%`" ar %*`r`n",
    $getnativeUtf8NoBom)
[System.IO.File]::WriteAllText(
    $getnativeRanlib,
    "@echo off`r`n`"%GETNATIVE_ZIG%`" ranlib %*`r`n",
    $getnativeUtf8NoBom)

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
    "-DCMAKE_SYSTEM_NAME=Linux",
    "-DCMAKE_SYSTEM_PROCESSOR=x86_64",
    "-DCMAKE_C_COMPILER=$getnativeZig",
    "-DCMAKE_C_COMPILER_ARG1=cc",
    "-DCMAKE_CXX_COMPILER=$getnativeZig",
    "-DCMAKE_CXX_COMPILER_ARG1=c++",
    "-DCMAKE_C_FLAGS=-target $getnativeLinuxTarget -fPIE",
    "-DCMAKE_CXX_FLAGS=-target $getnativeLinuxTarget -fPIE",
    "-DCMAKE_EXE_LINKER_FLAGS=-pie -Wl,-z,relro,-z,now",
    "-DCMAKE_AR=$getnativeAr",
    "-DCMAKE_RANLIB=$getnativeRanlib",
    "-DCMAKE_MAKE_PROGRAM=$getnativeNinja",
    "-DCMAKE_BUILD_TYPE=Release",
    "-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY",
    "-DTHREADS_PREFER_PTHREAD_FLAG=ON",
    "-DBUILD_TESTING=OFF",
    "-DGETNATIVE_ENABLE_METAL=OFF",
    "-DGETNATIVE_ENABLE_CUDA=ON",
    "-DGETNATIVE_BUILD_CUDA_AUTOTUNER=ON",
    "-DGETNATIVE_ENABLE_X86_SIMD=ON",
    "-DGETNATIVE_ENABLE_X86_AVX512=OFF",
    "-DGETNATIVE_CUDA_MIN_ARCHITECTURE=$getnativeMinimumArchitecture",
    "-DGETNATIVE_CUDA_ARCHITECTURES=$getnativeArchitectures",
    "-DGETNATIVE_CUDA_PTX_ARCHITECTURES=$getnativePtxArchitectures",
    "-DGETNATIVE_NVCC_EXECUTABLE=$getnativeNvcc",
    "-DGETNATIVE_CUOBJDUMP_EXECUTABLE=$getnativeCuobjdump",
    "-DGETNATIVE_CUDA_INCLUDE_DIR=$getnativeCudaInclude"
)

Write-Host "Configuring Ubuntu 24.04 x86_64 CUDA autotuner cross-build..."
& $getnativeCmake --fresh @getnativeConfigureArguments
if ($LASTEXITCODE -ne 0) { throw "CMake cross-configure failed" }

Write-Host "Building Ubuntu 24.04 x86_64 CUDA autotuner..."
& $getnativeCmake --build $getnativeBuild --target getnative_cuda_autotune `
    --clean-first --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake cross-build failed" }

$getnativeBuiltExecutable = Join-Path $getnativeBuild "getnative-cuda-autotune"
if (-not (Test-Path -LiteralPath $getnativeBuiltExecutable -PathType Leaf)) {
    throw "Linux autotuner executable was not produced: $getnativeBuiltExecutable"
}

$getnativeElfBytes = [System.IO.File]::ReadAllBytes($getnativeBuiltExecutable)
$getnativeValidElf = $getnativeElfBytes.Length -ge 64 -and
    $getnativeElfBytes[0] -eq 0x7f -and $getnativeElfBytes[1] -eq 0x45 -and
    $getnativeElfBytes[2] -eq 0x4c -and $getnativeElfBytes[3] -eq 0x46 -and
    $getnativeElfBytes[4] -eq 2 -and $getnativeElfBytes[5] -eq 1 -and
    [BitConverter]::ToUInt16($getnativeElfBytes, 18) -eq 62
if (-not $getnativeValidElf) {
    throw "Cross-build output is not a little-endian ELF64 x86-64 executable"
}

$getnativeElfType = [BitConverter]::ToUInt16($getnativeElfBytes, 16)
if ($getnativeElfType -ne 3) {
    throw "Cross-build output is not a PIE executable (ELF type $getnativeElfType)"
}
$getnativeProgramHeaderOffset = [long][BitConverter]::ToUInt64(
    $getnativeElfBytes, 32)
$getnativeProgramHeaderEntrySize = [BitConverter]::ToUInt16(
    $getnativeElfBytes, 54)
$getnativeProgramHeaderCount = [BitConverter]::ToUInt16(
    $getnativeElfBytes, 56)
if ($getnativeProgramHeaderEntrySize -lt 56 -or
    $getnativeProgramHeaderOffset -lt 64 -or
    $getnativeProgramHeaderOffset +
        ([long]$getnativeProgramHeaderEntrySize * $getnativeProgramHeaderCount) -gt
        $getnativeElfBytes.LongLength) {
    throw "ELF program header table is invalid"
}

$getnativeProgramHeaders = @()
for ($getnativeIndex = 0;
     $getnativeIndex -lt $getnativeProgramHeaderCount;
     ++$getnativeIndex) {
    $getnativeOffset = $getnativeProgramHeaderOffset +
        ([long]$getnativeIndex * $getnativeProgramHeaderEntrySize)
    $getnativeProgramHeaders += [pscustomobject]@{
        Type = [BitConverter]::ToUInt32($getnativeElfBytes, [int]$getnativeOffset)
        Flags = [BitConverter]::ToUInt32(
            $getnativeElfBytes, [int]($getnativeOffset + 4))
        Offset = [long][BitConverter]::ToUInt64(
            $getnativeElfBytes, [int]($getnativeOffset + 8))
        VirtualAddress = [long][BitConverter]::ToUInt64(
            $getnativeElfBytes, [int]($getnativeOffset + 16))
        FileSize = [long][BitConverter]::ToUInt64(
            $getnativeElfBytes, [int]($getnativeOffset + 32))
    }
}

$getnativeInterpreterHeaders = @(
    $getnativeProgramHeaders | Where-Object Type -eq 3)
if ($getnativeInterpreterHeaders.Count -ne 1) {
    throw "ELF must contain exactly one PT_INTERP program header"
}
$getnativeInterpreterHeader = $getnativeInterpreterHeaders[0]
$getnativeInterpreter = Get-GetNativeNullTerminatedAscii `
    -Bytes $getnativeElfBytes `
    -Offset $getnativeInterpreterHeader.Offset `
    -Limit ($getnativeInterpreterHeader.Offset +
        $getnativeInterpreterHeader.FileSize)
if ($getnativeInterpreter -ne $getnativeExpectedInterpreter) {
    throw "Unexpected ELF interpreter: $getnativeInterpreter"
}

$getnativeStackHeaders = @(
    $getnativeProgramHeaders | Where-Object Type -eq 0x6474e551)
if ($getnativeStackHeaders.Count -ne 1 -or
    (($getnativeStackHeaders[0].Flags -band 1) -ne 0)) {
    throw "ELF GNU stack is missing or executable"
}
if (@($getnativeProgramHeaders | Where-Object Type -eq 0x6474e552).Count -eq 0) {
    throw "ELF is missing GNU RELRO"
}

$getnativeDynamicHeaders = @(
    $getnativeProgramHeaders | Where-Object Type -eq 2)
if ($getnativeDynamicHeaders.Count -ne 1) {
    throw "ELF must contain exactly one PT_DYNAMIC program header"
}
$getnativeDynamicHeader = $getnativeDynamicHeaders[0]
if ($getnativeDynamicHeader.Offset -lt 0 -or
    $getnativeDynamicHeader.FileSize -lt 16 -or
    $getnativeDynamicHeader.Offset + $getnativeDynamicHeader.FileSize -gt
        $getnativeElfBytes.LongLength) {
    throw "ELF dynamic table is invalid"
}
$getnativeDynamicEntries = @()
for ($getnativeOffset = $getnativeDynamicHeader.Offset;
     $getnativeOffset + 16 -le
        $getnativeDynamicHeader.Offset + $getnativeDynamicHeader.FileSize;
     $getnativeOffset += 16) {
    $getnativeTag = [BitConverter]::ToInt64(
        $getnativeElfBytes, [int]$getnativeOffset)
    $getnativeValue = [long][BitConverter]::ToUInt64(
        $getnativeElfBytes, [int]($getnativeOffset + 8))
    if ($getnativeTag -eq 0) { break }
    $getnativeDynamicEntries += [pscustomobject]@{
        Tag = $getnativeTag
        Value = $getnativeValue
    }
}

$getnativeStringTableEntries = @(
    $getnativeDynamicEntries | Where-Object Tag -eq 5)
$getnativeStringTableSizeEntries = @(
    $getnativeDynamicEntries | Where-Object Tag -eq 10)
if ($getnativeStringTableEntries.Count -ne 1 -or
    $getnativeStringTableSizeEntries.Count -ne 1 -or
    $getnativeStringTableSizeEntries[0].Value -le 0) {
    throw "ELF dynamic string table metadata is invalid"
}
$getnativeLoadSegments = @(
    $getnativeProgramHeaders | Where-Object Type -eq 1)
$getnativeStringTableSize = $getnativeStringTableSizeEntries[0].Value
$getnativeStringTableOffset = Convert-GetNativeElfAddressToOffset `
    -Address $getnativeStringTableEntries[0].Value `
    -Size $getnativeStringTableSize `
    -LoadSegments $getnativeLoadSegments `
    -FileSize $getnativeElfBytes.LongLength

$getnativeSharedLibraries = @()
foreach ($getnativeNeeded in @(
        $getnativeDynamicEntries | Where-Object Tag -eq 1)) {
    if ($getnativeNeeded.Value -lt 0 -or
        $getnativeNeeded.Value -ge $getnativeStringTableSize) {
        throw "ELF DT_NEEDED string offset is invalid"
    }
    $getnativeSharedLibraries += Get-GetNativeNullTerminatedAscii `
        -Bytes $getnativeElfBytes `
        -Offset ($getnativeStringTableOffset + $getnativeNeeded.Value) `
        -Limit ($getnativeStringTableOffset + $getnativeStringTableSize)
}
$getnativeSharedLibraries = @($getnativeSharedLibraries | Sort-Object -Unique)
$getnativeExpectedSharedLibraries = @(
    $getnativeExpectedSharedLibraries | Sort-Object -Unique)
$getnativeUnexpectedLibraries = @(Compare-Object `
    -ReferenceObject $getnativeExpectedSharedLibraries `
    -DifferenceObject $getnativeSharedLibraries)
if ($getnativeUnexpectedLibraries.Count -ne 0) {
    throw "Unexpected ELF shared-library inventory: $($getnativeSharedLibraries -join ', ')"
}
if (@($getnativeDynamicEntries | Where-Object {
            $_.Tag -eq 15 -or $_.Tag -eq 29 }).Count -ne 0) {
    throw "ELF contains an RPATH or RUNPATH"
}

$getnativeBindNow = @(
    $getnativeDynamicEntries | Where-Object Tag -eq 24).Count -ne 0
foreach ($getnativeEntry in $getnativeDynamicEntries) {
    if (($getnativeEntry.Tag -eq 30 -and
         ($getnativeEntry.Value -band 8) -ne 0) -or
        ($getnativeEntry.Tag -eq 0x6ffffffb -and
         ($getnativeEntry.Value -band 1) -ne 0)) {
        $getnativeBindNow = $true
    }
}
if (-not $getnativeBindNow) {
    throw "ELF does not enable immediate symbol binding"
}
$getnativeFlags1Entries = @(
    $getnativeDynamicEntries | Where-Object Tag -eq 0x6ffffffb)
if ($getnativeFlags1Entries.Count -eq 0 -or
    -not ($getnativeFlags1Entries | Where-Object {
        ($_.Value -band 0x08000000) -ne 0 })) {
    throw "ELF dynamic flags do not identify a PIE executable"
}

$getnativeElfText = [System.Text.Encoding]::GetEncoding(28591).GetString(
    $getnativeElfBytes)
$getnativeGlibcMatches = [regex]::Matches(
    $getnativeElfText, 'GLIBC_([0-9]+\.[0-9]+(?:\.[0-9]+)?)')
if ($getnativeGlibcMatches.Count -eq 0) {
    throw "ELF has no detectable GLIBC symbol-version requirements"
}
$getnativeMinimumGlibcVersion = [version]"0.0"
foreach ($getnativeMatch in $getnativeGlibcMatches) {
    $getnativeVersion = [version]$getnativeMatch.Groups[1].Value
    if ($getnativeVersion -gt $getnativeMinimumGlibcVersion) {
        $getnativeMinimumGlibcVersion = $getnativeVersion
    }
}
if ($getnativeMinimumGlibcVersion -gt $getnativeMaximumGlibcVersion) {
    throw "ELF requires GLIBC $getnativeMinimumGlibcVersion, above target $getnativeMaximumGlibcVersion"
}

$getnativeFatbin = Join-Path $getnativeBuild `
    "generated\cuda\getnative_cuda_staged.fatbin"
& $getnativeCmake `
    "-DFATBIN=$getnativeFatbin" `
    "-DCUOBJDUMP=$getnativeCuobjdump" `
    "-DNATIVE_ARCHITECTURES=$getnativeArchitectures" `
    "-DPTX_ARCHITECTURES=$getnativePtxArchitectures" `
    -P (Join-Path $getnativeSource "cmake\verify_cuda_fatbin.cmake")
if ($LASTEXITCODE -ne 0) { throw "CUDA fatbin verification failed" }

New-Item -ItemType Directory -Path $getnativeArtifactDirectory -Force | Out-Null
$getnativeExportedExecutable = Join-Path $getnativeArtifactDirectory `
    "getnative-cuda-autotune"
Copy-Item -LiteralPath $getnativeBuiltExecutable `
    -Destination $getnativeExportedExecutable -Force

$getnativeHash = (Get-FileHash -LiteralPath $getnativeExportedExecutable `
    -Algorithm SHA256).Hash.ToLowerInvariant()
$getnativeSize = (Get-Item -LiteralPath $getnativeExportedExecutable).Length
$getnativeManifest = [ordered]@{
    schema_version = 1
    artifact = "getnative-cuda-autotune"
    tool_version = "1.4.0"
    report_schema_version = 2
    sha256 = $getnativeHash
    size_bytes = $getnativeSize
    target_os = "ubuntu-24.04"
    target_architecture = "x86_64"
    target_abi = "glibc-2.39"
    minimum_glibc_version = $getnativeMinimumGlibcVersion.ToString()
    elf_type = "ET_DYN"
    position_independent_executable = $true
    elf_interpreter = $getnativeInterpreter
    shared_library_dependencies = $getnativeSharedLibraries
    hardening = @("nx-stack", "relro", "bind-now")
    cross_compiler = "zig-$getnativeDetectedZigVersion"
    cross_compiler_target = $getnativeLinuxTarget
    minimum_compute_capability = "7.5"
    native_architectures = $getnativeArchitectures
    ptx_architectures = $getnativePtxArchitectures
    target_compute_capabilities = @("sm_75", "sm_86", "sm_89", "sm_120")
    runtime_architecture_whitelist_enforced = $true
    cuda_toolkit_required_on_runner = $false
    nvidia_driver_and_libcuda_required_on_runner = $true
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
    production_configuration_is_modified = $false
}
$getnativeManifestPath = Join-Path $getnativeArtifactDirectory "manifest.json"
$getnativeManifest | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath $getnativeManifestPath -Encoding utf8

Write-Host "artifact=$getnativeExportedExecutable"
Write-Host "sha256=$getnativeHash"
Write-Host "size_bytes=$getnativeSize"
Write-Host "manifest=$getnativeManifestPath"

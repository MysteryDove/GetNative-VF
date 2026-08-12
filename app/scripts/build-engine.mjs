import { createHash } from "node:crypto";
import {
  copyFileSync,
  existsSync,
  readFileSync,
  readdirSync,
  rmSync,
  statSync,
  writeFileSync,
} from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const appDirectory = resolve(scriptDirectory, "..");
const repositoryDirectory = resolve(appDirectory, "..");
const sourceDirectory = join(repositoryDirectory, "engine");
const buildDirectory = join(repositoryDirectory, "build", "engine");
const stageDirectory = join(appDirectory, "src-tauri", "bundle-stage");
const defaultFfmpegRuntimeDirectory = join(appDirectory, "src-tauri", "ffmpeg-runtime");
const debug = process.argv.includes("--debug");
const buildType = debug ? "Debug" : "Release";
const ffmpegRuntimePatterns = process.platform === "win32"
  ? [/^avformat-62\.dll$/iu, /^avcodec-62\.dll$/iu,
    /^avutil-60\.dll$/iu, /^swscale-9\.dll$/iu]
  : process.platform === "darwin"
    ? [/^libavformat\.62\.dylib$/u, /^libavcodec\.62\.dylib$/u,
      /^libavutil\.60\.dylib$/u, /^libswscale\.9\.dylib$/u]
    : [/^libavformat\.so\.62$/u, /^libavcodec\.so\.62$/u,
      /^libavutil\.so\.60$/u, /^libswscale\.so\.9$/u];
const managedFfmpegPatterns = [
  /^avformat-62\.dll$/iu,
  /^avcodec-62\.dll$/iu,
  /^avutil-60\.dll$/iu,
  /^swscale-9\.dll$/iu,
  /^libavformat\.so\.62(?:\.|$)/u,
  /^libavcodec\.so\.62(?:\.|$)/u,
  /^libavutil\.so\.60(?:\.|$)/u,
  /^libswscale\.so\.9(?:\.|$)/u,
  /^libavformat\.62\.dylib$/u,
  /^libavcodec\.62\.dylib$/u,
  /^libavutil\.60\.dylib$/u,
  /^libswscale\.9\.dylib$/u,
];

function fail(message) {
  console.error(message);
  process.exit(1);
}

function run(command, args, environment) {
  const result = spawnSync(command, args, {
    cwd: appDirectory,
    env: environment,
    stdio: "inherit",
    windowsHide: true,
  });
  if (result.error) fail(`${command} failed to start: ${result.error.message}`);
  if (result.status !== 0) process.exit(result.status ?? 1);
}

function replaceEnvironmentEntry(environment, name, value) {
  for (const key of Object.keys(environment)) {
    if (key.toLowerCase() === name.toLowerCase()) delete environment[key];
  }
  environment[name] = value;
}

function enabled(value) {
  return ["1", "on", "true", "yes"].includes((value || "").toLowerCase());
}

function cudaToolkitAvailable() {
  const roots = [
    process.env.GETNATIVE_CUDA_ROOT,
    process.env.CUDAToolkit_ROOT,
    process.env.CUDA_PATH,
    process.env.CUDA_HOME,
    "/usr/local/cuda",
  ].filter(Boolean);
  const executable = process.platform === "win32" ? "nvcc.exe" : "nvcc";
  return roots.some((root) => existsSync(join(root, "bin", executable)));
}

function cleanManagedStageFiles() {
  const binaryDirectory = join(stageDirectory, "bin");
  if (!existsSync(binaryDirectory)) return;
  for (const name of readdirSync(binaryDirectory)) {
    const managedEngine = name === "getnative-engine" || name === "getnative-engine.exe";
    const managedFfmpeg = managedFfmpegPatterns.some((pattern) => pattern.test(name));
    const forbiddenSidecar = /^(?:ffmpeg|ffprobe)(?:\.exe)?$/iu.test(name);
    if (managedEngine || managedFfmpeg || forbiddenSidecar) rmSync(join(binaryDirectory, name));
  }
}

function stageFfmpegRuntime() {
  const runtimeDirectory = process.env.GETNATIVE_FFMPEG_RUNTIME_DIR
    || (existsSync(defaultFfmpegRuntimeDirectory) ? defaultFfmpegRuntimeDirectory : null);
  if (!runtimeDirectory) {
    if (!debug) {
      fail("Release packaging requires GETNATIVE_FFMPEG_RUNTIME_DIR or src-tauri/ffmpeg-runtime");
    }
    return [];
  }
  const resolved = resolve(runtimeDirectory);
  if (!existsSync(resolved) || !statSync(resolved).isDirectory()) {
    fail(`GETNATIVE_FFMPEG_RUNTIME_DIR is not a directory: ${resolved}`);
  }
  const directoryEntries = readdirSync(resolved);
  for (const pattern of ffmpegRuntimePatterns) {
    if (!directoryEntries.some((entry) => pattern.test(entry))) {
      fail(`FFmpeg runtime bundle is missing ${pattern}`);
    }
  }
  const entries = directoryEntries
    .filter((name) => ffmpegRuntimePatterns.some((pattern) => pattern.test(name)))
    .sort();
  return entries.map((name) => {
    const source = join(resolved, name);
    const destination = join(stageDirectory, "bin", name);
    copyFileSync(source, destination);
    return {
      name,
      sha256: createHash("sha256").update(readFileSync(destination)).digest("hex"),
    };
  });
}

function visualStudioEnvironment(baseEnvironment) {
  const defaultDevCommand =
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\Tools\\VsDevCmd.bat";
  const devCommand = process.env.GETNATIVE_VSDEVCMD || defaultDevCommand;
  if (!existsSync(devCommand)) {
    fail(`Visual Studio developer command file was not found: ${devCommand}`);
  }
  const result = spawnSync(
    "cmd.exe",
    ["/d", "/s", "/c", `call "${devCommand}" -arch=x64 -host_arch=x64 >nul && set`],
    {
      cwd: appDirectory,
      env: baseEnvironment,
      encoding: "utf8",
      windowsHide: true,
      windowsVerbatimArguments: true,
    },
  );
  if (result.error || result.status !== 0) {
    fail(`Visual Studio developer environment failed: ${result.error?.message || result.stderr}`);
  }
  const environment = { ...baseEnvironment };
  for (const line of result.stdout.split(/\r?\n/u)) {
    const separator = line.indexOf("=");
    if (separator <= 0) continue;
    replaceEnvironmentEntry(
      environment,
      line.slice(0, separator),
      line.slice(separator + 1),
    );
  }
  return environment;
}

let environment = { ...process.env };
let cmake = process.env.CMAKE || "cmake";
let ctest = process.env.CTEST || "ctest";
const configureArguments = [
  "-S",
  sourceDirectory,
  "-B",
  buildDirectory,
  `-DCMAKE_BUILD_TYPE=${buildType}`,
];

let windowsBackends = "not-applicable";
let linuxBackends = "not-applicable";
let cudaEnabled = false;
let vulkanEnabled = enabled(process.env.GETNATIVE_ENABLE_VULKAN);
let cudaArchitectures = "not-applicable";
let cudaMinimumArchitecture = "not-applicable";
let cudaPtxArchitectures = "not-applicable";
if (process.platform === "win32") {
  environment = visualStudioEnvironment(environment);
  const visualStudioCMakeRoot =
    "C:\\Program Files (x86)\\Microsoft Visual Studio\\2022\\BuildTools\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake";
  const bundledCmake = join(visualStudioCMakeRoot, "CMake", "bin", "cmake.exe");
  const bundledCtest = join(visualStudioCMakeRoot, "CMake", "bin", "ctest.exe");
  const bundledNinja = join(visualStudioCMakeRoot, "Ninja", "ninja.exe");
  if (!process.env.CMAKE && existsSync(bundledCmake)) cmake = bundledCmake;
  if (!process.env.CTEST && existsSync(bundledCtest)) ctest = bundledCtest;
  if (existsSync(bundledNinja)) {
    configureArguments.push("-G", "Ninja", `-DCMAKE_MAKE_PROGRAM=${bundledNinja}`);
  }

  windowsBackends = (process.env.GETNATIVE_WINDOWS_BACKENDS || "cpu").toLowerCase();
  if (!["cpu", "cuda", "vulkan", "cuda-vulkan"].includes(windowsBackends)) {
    fail("GETNATIVE_WINDOWS_BACKENDS must be cpu, cuda, vulkan, or cuda-vulkan");
  }
  cudaEnabled = windowsBackends === "cuda" || windowsBackends === "cuda-vulkan";
  vulkanEnabled = vulkanEnabled
    || windowsBackends === "vulkan"
    || windowsBackends === "cuda-vulkan";
  cudaMinimumArchitecture = process.env.GETNATIVE_CUDA_MIN_ARCHITECTURE || "75";
  cudaArchitectures =
    process.env.GETNATIVE_CUDA_ARCHITECTURES ||
    "75;80;86;87;88;89;90;100;103;110;120;121";
  cudaPtxArchitectures = process.env.GETNATIVE_CUDA_PTX_ARCHITECTURES || "75;121";
  configureArguments.push(
    "-DGETNATIVE_ENABLE_METAL=OFF",
    "-DGETNATIVE_ENABLE_X86_SIMD=ON",
    "-DGETNATIVE_ENABLE_X86_AVX512=ON",
    `-DGETNATIVE_ENABLE_CUDA=${cudaEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_ENABLE_VULKAN=${vulkanEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_CUDA_MIN_ARCHITECTURE=${cudaMinimumArchitecture}`,
    `-DGETNATIVE_CUDA_ARCHITECTURES=${cudaArchitectures}`,
    `-DGETNATIVE_CUDA_PTX_ARCHITECTURES=${cudaPtxArchitectures}`,
  );
} else if (process.platform === "linux") {
  linuxBackends = (process.env.GETNATIVE_LINUX_BACKENDS || "auto").toLowerCase();
  if (!["auto", "cpu", "cuda", "vulkan", "cuda-vulkan"].includes(linuxBackends)) {
    fail("GETNATIVE_LINUX_BACKENDS must be auto, cpu, cuda, vulkan, or cuda-vulkan");
  }
  cudaEnabled = linuxBackends === "cuda"
    || linuxBackends === "cuda-vulkan"
    || (linuxBackends === "auto" && cudaToolkitAvailable());
  vulkanEnabled = vulkanEnabled
    || linuxBackends === "vulkan"
    || linuxBackends === "cuda-vulkan";
  cudaMinimumArchitecture = process.env.GETNATIVE_CUDA_MIN_ARCHITECTURE || "75";
  cudaArchitectures = process.env.GETNATIVE_CUDA_ARCHITECTURES
    || "75;80;86;87;88;89;90;100;103;110;120;121";
  cudaPtxArchitectures = process.env.GETNATIVE_CUDA_PTX_ARCHITECTURES || "75;121";
  configureArguments.push(
    `-DGETNATIVE_ENABLE_CUDA=${cudaEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_ENABLE_VULKAN=${vulkanEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_CUDA_MIN_ARCHITECTURE=${cudaMinimumArchitecture}`,
    `-DGETNATIVE_CUDA_ARCHITECTURES=${cudaArchitectures}`,
    `-DGETNATIVE_CUDA_PTX_ARCHITECTURES=${cudaPtxArchitectures}`,
  );
} else {
  vulkanEnabled = false;
}

run(cmake, configureArguments, environment);
run(cmake, ["--build", buildDirectory, "--parallel"], environment);
run(ctest, ["--test-dir", buildDirectory, "--output-on-failure"], environment);
cleanManagedStageFiles();
run(cmake, ["--install", buildDirectory, "--prefix", stageDirectory], environment);
const ffmpegRuntime = stageFfmpegRuntime();

const executableName = process.platform === "win32" ? "getnative-engine.exe" : "getnative-engine";
const stagedEngine = join(stageDirectory, "bin", executableName);
if (!existsSync(stagedEngine)) fail(`staged engine was not found: ${stagedEngine}`);
for (const forbidden of ["ffmpeg", "ffprobe", "ffmpeg.exe", "ffprobe.exe"]) {
  if (existsSync(join(stageDirectory, "bin", forbidden))) {
    fail(`external media executable must not be packaged: ${forbidden}`);
  }
}
if (!debug) {
  const result = spawnSync(stagedEngine, ["capabilities"], {
    cwd: appDirectory,
    env: environment,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.error || result.status !== 0) {
    fail(`staged engine media capability check failed: ${result.error?.message || result.stderr}`);
  }
  let capabilities;
  try {
    capabilities = JSON.parse(result.stdout);
  } catch (error) {
    fail(`staged engine returned invalid capabilities JSON: ${error.message}`);
  }
  const commands = capabilities?.commands || {};
  const mediaCommands = [
    "media_index_begin",
    "media_frame_window",
    "media_preview_begin",
    "media_asset_batch_begin",
  ];
  if (!capabilities?.media?.available
      || capabilities.media.ffmpeg_abi !== "62.62.60.9"
      || capabilities.media.index_version !== 1
      || mediaCommands.some((command) => commands[command] !== true)) {
    fail("release engine is missing required in-process media capabilities");
  }
}
const sha256 = createHash("sha256").update(readFileSync(stagedEngine)).digest("hex");
writeFileSync(
  join(stageDirectory, "build-provenance.json"),
  `${JSON.stringify(
    {
      schema_version: 1,
      build_type: buildType,
      platform: process.platform,
      windows_backends: windowsBackends,
      linux_backends: linuxBackends,
      cuda_enabled: cudaEnabled,
      vulkan_enabled: vulkanEnabled,
      cuda_minimum_architecture: cudaMinimumArchitecture,
      cuda_architectures: cudaArchitectures,
      cuda_ptx_architectures: cudaPtxArchitectures,
      engine_sha256: sha256,
      ffmpeg_runtime: ffmpegRuntime,
      ctest_passed: true,
    },
    null,
    2,
  )}\n`,
  "utf8",
);
console.log(`staged_engine=${stagedEngine}`);
console.log(`engine_sha256=${sha256}`);
if (ffmpegRuntime.length > 0) {
  console.log(`ffmpeg_runtime=${ffmpegRuntime.map(({ name }) => name).join(",")}`);
}

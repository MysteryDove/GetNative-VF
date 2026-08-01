import { createHash } from "node:crypto";
import { existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { spawnSync } from "node:child_process";

const scriptDirectory = dirname(fileURLToPath(import.meta.url));
const appDirectory = resolve(scriptDirectory, "..");
const repositoryDirectory = resolve(appDirectory, "..");
const sourceDirectory = join(repositoryDirectory, "engine");
const buildDirectory = join(repositoryDirectory, "build", "engine");
const stageDirectory = join(appDirectory, "src-tauri", "bundle-stage");
const debug = process.argv.includes("--debug");
const buildType = debug ? "Debug" : "Release";

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
let cudaArchitectures = "not-applicable";
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

  windowsBackends = (process.env.GETNATIVE_WINDOWS_BACKENDS || "both").toLowerCase();
  if (!["cpu", "cuda", "vulkan", "both"].includes(windowsBackends)) {
    fail("GETNATIVE_WINDOWS_BACKENDS must be cpu, cuda, vulkan, or both");
  }
  const cudaEnabled = windowsBackends === "cuda" || windowsBackends === "both";
  const vulkanEnabled = windowsBackends === "vulkan" || windowsBackends === "both";
  cudaArchitectures = process.env.GETNATIVE_CUDA_ARCHITECTURES || "120";
  configureArguments.push(
    "-DGETNATIVE_ENABLE_METAL=OFF",
    "-DGETNATIVE_ENABLE_X86_SIMD=ON",
    "-DGETNATIVE_ENABLE_X86_AVX512=ON",
    `-DGETNATIVE_ENABLE_CUDA=${cudaEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_ENABLE_VULKAN=${vulkanEnabled ? "ON" : "OFF"}`,
    `-DGETNATIVE_CUDA_ARCHITECTURES=${cudaArchitectures}`,
  );
}

run(cmake, configureArguments, environment);
run(cmake, ["--build", buildDirectory, "--parallel"], environment);
run(ctest, ["--test-dir", buildDirectory, "--output-on-failure"], environment);
run(cmake, ["--install", buildDirectory, "--prefix", stageDirectory], environment);

const executableName = process.platform === "win32" ? "getnative-engine.exe" : "getnative-engine";
const stagedEngine = join(stageDirectory, "bin", executableName);
if (!existsSync(stagedEngine)) fail(`staged engine was not found: ${stagedEngine}`);
const sha256 = createHash("sha256").update(readFileSync(stagedEngine)).digest("hex");
writeFileSync(
  join(stageDirectory, "build-provenance.json"),
  `${JSON.stringify(
    {
      schema_version: 1,
      build_type: buildType,
      platform: process.platform,
      windows_backends: windowsBackends,
      cuda_architectures: cudaArchitectures,
      engine_sha256: sha256,
      ctest_passed: true,
    },
    null,
    2,
  )}\n`,
  "utf8",
);
console.log(`staged_engine=${stagedEngine}`);
console.log(`engine_sha256=${sha256}`);

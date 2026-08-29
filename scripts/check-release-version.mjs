import { readFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const repository = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const packageJson = JSON.parse(readFileSync(join(repository, "app", "package.json"), "utf8"));
const tauriConfig = JSON.parse(
  readFileSync(join(repository, "app", "src-tauri", "tauri.conf.json"), "utf8"),
);
const cargoToml = readFileSync(join(repository, "app", "src-tauri", "Cargo.toml"), "utf8");
const cmake = readFileSync(join(repository, "engine", "CMakeLists.txt"), "utf8");
const cargoVersion = cargoToml.match(/^version\s*=\s*"([^"]+)"/mu)?.[1];
const engineVersion = cmake.match(/project\(getnative_engine VERSION ([^ )]+)/u)?.[1];
const versions = {
  package_json: packageJson.version,
  tauri_config: tauriConfig.version,
  cargo_toml: cargoVersion,
  engine_cmake: engineVersion,
};
const unique = new Set(Object.values(versions));
if (unique.size !== 1 || unique.has(undefined)) {
  console.error(`release versions do not agree: ${JSON.stringify(versions)}`);
  process.exit(1);
}
const tag = process.argv[2];
const version = packageJson.version;
if (tag && tag !== `v${version}`) {
  console.error(`release tag ${tag} does not match project version v${version}`);
  process.exit(1);
}
console.log(`release_version=${version}`);

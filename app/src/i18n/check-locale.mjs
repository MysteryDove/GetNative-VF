import { readFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const root = join(dirname(fileURLToPath(import.meta.url)));
const zh = JSON.parse(readFileSync(join(root, "locales/zh-CN.json"), "utf8"));
const en = JSON.parse(readFileSync(join(root, "locales/en.json"), "utf8"));

const zhKeys = Object.keys(zh).sort();
const enKeys = Object.keys(en).sort();
const missingEn = zhKeys.filter((key) => !(key in en));
const extraEn = enKeys.filter((key) => !(key in zh));

if (missingEn.length || extraEn.length) {
  console.error("Locale key parity failed");
  if (missingEn.length) console.error("missing in en:", missingEn.join(", "));
  if (extraEn.length) console.error("extra in en:", extraEn.join(", "));
  process.exit(1);
}

console.log(`Locale parity OK (${zhKeys.length} keys)`);

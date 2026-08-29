// 生产 CSP 门禁：所有媒体预览都渲染为 blob: object URL（src/media/service.ts），
// 打包后 Tauri 通过 custom protocol 响应头强制 CSP，而 dev 由 vite 伺服无 CSP 头——
// img-src 缺 blob: 时开发全程正常、仅在安装包里所有预览空白（2026-08 实踩）。
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

const configPath = fileURLToPath(new URL("../src-tauri/tauri.conf.json", import.meta.url));
const csp = JSON.parse(readFileSync(configPath, "utf8")).app.security.csp;

const imgSrc = csp
  .split(";")
  .map((directive) => directive.trim())
  .find((directive) => directive.startsWith("img-src"));

const failures = [];
if (!imgSrc) failures.push("img-src 指令缺失");
for (const scheme of ["blob:", "data:"]) {
  if (!imgSrc?.includes(scheme)) failures.push(`img-src 缺少 ${scheme}（媒体预览会被 CSP 拦截）`);
}

if (failures.length > 0) {
  console.error(`tauri.conf.json 生产 CSP 不合格:\n  ${failures.join("\n  ")}`);
  process.exit(1);
}
console.log("tauri.conf.json 生产 CSP 检查通过 (img-src 允许 blob:/data:)");

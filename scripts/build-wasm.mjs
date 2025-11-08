#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const projectRoot = dirname(__dirname);
const distDir = join(projectRoot, "dist");

mkdirSync(distDir, { recursive: true });

const sources = [
  join(projectRoot, "src", "c", "littlefs_wasm.c"),
  join(projectRoot, "third_party", "littlefs", "lfs.c"),
  join(projectRoot, "third_party", "littlefs", "lfs_util.c")
];

const includeDir = join(projectRoot, "third_party", "littlefs");
const output = join(distDir, "littlefs.wasm");

const emccArgs = [
  ...sources,
  "-I",
  includeDir,
  "-O3",
  "--no-entry",
  "-s",
  "STANDALONE_WASM=1",
  "-s",
  "ALLOW_MEMORY_GROWTH=1",
  "-s",
  "FILESYSTEM=0",
  "-s",
  "EXPORTED_FUNCTIONS=['_lfsjs_init','_lfsjs_format','_lfsjs_add_file','_lfsjs_delete_file','_lfsjs_list','_malloc','_free']",
  "-o",
  output
];

const quoted = ["emcc", ...emccArgs].map(quoteArgument).join(" ");
const result = spawnSync(quoted, {
  stdio: "inherit",
  env: process.env,
  shell: true
});

if (result.error) {
  console.error("Failed to execute emcc:", result.error.message);
  process.exit(1);
}

if (result.status !== 0) {
  process.exit(result.status ?? 1);
}

console.log(`Created ${output}`);

function quoteArgument(arg) {
  if (!/[\s"]/u.test(arg)) {
    return arg;
  }
  const escaped = arg.replace(/(["\\])/g, "\\$1");
  return `"${escaped}"`;
}

#!/usr/bin/env node

import { spawnSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);
const projectRoot = dirname(__dirname);
const distDir = join(projectRoot, "dist");

const targets = [
  {
    name: "littlefs",
    outputDir: join(distDir, "littlefs"),
    outputWasm: join(distDir, "littlefs", "littlefs.wasm"),
    sources: [
      join(projectRoot, "src", "c", "littlefs_wasm.c"),
      join(projectRoot, "third_party", "littlefs", "lfs.c"),
      join(projectRoot, "third_party", "littlefs", "lfs_util.c")
    ],
    includes: [join(projectRoot, "third_party", "littlefs")],
    exports:
      "['_lfsjs_init','_lfsjs_init_from_image','_lfsjs_format','_lfsjs_add_file','_lfsjs_delete_file','_lfsjs_remove','_lfsjs_mkdir','_lfsjs_rename','_lfsjs_list','_lfsjs_file_size','_lfsjs_read_file','_lfsjs_export_image','_lfsjs_storage_size','_malloc','_free']"
  },
  {
    name: "fatfs",
    outputDir: join(distDir, "fatfs"),
    outputWasm: join(distDir, "fatfs", "fatfs.wasm"),
    sources: [
      join(projectRoot, "src", "c", "fatfs_wasm.c"),
      join(projectRoot, "third_party", "fatfs", "ff.c"),
      join(projectRoot, "third_party", "fatfs", "ffunicode.c")
    ],
    includes: [join(projectRoot, "third_party", "fatfs")],
    exports:
      "['_fatfsjs_init','_fatfsjs_init_from_image','_fatfsjs_format','_fatfsjs_write_file','_fatfsjs_delete_file','_fatfsjs_list','_fatfsjs_file_size','_fatfsjs_read_file','_fatfsjs_export_image','_fatfsjs_storage_size','_malloc','_free']"
  }
];

mkdirSync(distDir, { recursive: true });

for (const target of targets) {
  mkdirSync(target.outputDir, { recursive: true });
  const emccArgs = [
    ...target.sources,
    ...target.includes.flatMap((inc) => ["-I", inc]),
    "-O3",
    "--no-entry",
    "-s",
    "STANDALONE_WASM=1",
    "-s",
    "ALLOW_MEMORY_GROWTH=1",
    "-s",
    "FILESYSTEM=0",
    "-s",
    `EXPORTED_FUNCTIONS=${target.exports}`,
    "-o",
    target.outputWasm
  ];

  const quoted = ["emcc", ...emccArgs].map(quoteArgument).join(" ");
  const result = spawnSync(quoted, {
    stdio: "inherit",
    env: process.env,
    shell: true
  });

  if (result.error) {
    console.error(`Failed to execute emcc for ${target.name}:`, result.error.message);
    process.exit(1);
  }

  if (result.status !== 0) {
    process.exit(result.status ?? 1);
  }

  console.log(`Created ${target.outputWasm}`);
}

function quoteArgument(arg) {
  if (!/[\s"]/u.test(arg)) {
    return arg;
  }
  const escaped = arg.replace(/(["\\])/g, "\\$1");
  return `"${escaped}"`;
}

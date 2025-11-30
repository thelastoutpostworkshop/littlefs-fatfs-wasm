#!/usr/bin/env node

import assert from "node:assert";
import { readFile } from "node:fs/promises";
import { createLittleFS, createLittleFSFromImage } from "../dist/littlefs/index.js";

// Minimal file:// fetch support for Node so the wasm loader works in tests.
const originalFetch = globalThis.fetch;
globalThis.fetch = async (input, init) => {
  const url = typeof input === "string" ? input : input instanceof URL ? input.href : input.url;
  if (url.startsWith("file://")) {
    const data = await readFile(new URL(url));
    return new Response(data, { status: 200 });
  }
  return originalFetch(input, init);
};

async function main() {
  // fresh filesystem
  const fs = await createLittleFS({ formatOnInit: true });

  fs.mkdir("docs");
  fs.mkdir("notes");
  fs.writeFile("docs/readme.txt", "hello");
  fs.writeFile("notes/todo.txt", "one");
  fs.rename("notes/todo.txt", "notes/done.txt");

  const listRoot = fs.list("/");
  const paths = listRoot.map((e) => e.path).sort();
  assert(paths.includes("docs"), "docs directory missing");
  assert(paths.includes("docs/readme.txt"), "readme missing");
  assert(paths.includes("notes"), "notes directory missing");
  assert(paths.includes("notes/done.txt"), "renamed file missing");

  const content = new TextDecoder().decode(fs.readFile("docs/readme.txt"));
  assert.strictEqual(content, "hello");

  // Export/import round-trip
  const image = fs.toImage();
  const fs2 = await createLittleFSFromImage(image, { blockSize: 512, blockCount: image.length / 512 });
  const list2 = fs2.list("/");
  assert.strictEqual(list2.length, listRoot.length, "round-trip entry count mismatch");

  // Recursive delete
  fs2.delete("notes", { recursive: true });
  const remaining = fs2.list("/").map((e) => e.path);
  assert(!remaining.some((p) => p.startsWith("notes")), "recursive delete failed");

  console.log("littlefs self-test passed");
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});

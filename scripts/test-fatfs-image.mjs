import assert from "node:assert/strict";
import { readFile, stat } from "node:fs/promises";
import { resolve } from "node:path";
import { fileURLToPath } from "node:url";

const originalFetch = globalThis.fetch;
globalThis.fetch = async (resource, init) => {
  const url = resource instanceof URL ? resource : new URL(resource.toString());
  if (url.protocol === "file:") {
    const path = fileURLToPath(url);
    const body = await readFile(path);
    return new Response(body, { status: 200 });
  }
  return originalFetch ? originalFetch(resource, init) : fetch(resource, init);
};

const DEFAULT_IMAGE = "C:\\Users\\charles\\Downloads\\fatfs_good.bin";

function normalizeEntryPath(path) {
  if (!path) {
    return "";
  }
  if (path === "/") {
    return "/";
  }
  return path.replace(/^\/+/, "");
}

try {
  const pathArg = process.argv[2] ?? DEFAULT_IMAGE;
  const imagePath = resolve(pathArg);
  const info = await stat(imagePath);
  console.log("Image size", info.size);

  const bytes = await readFile(imagePath);
  const overrideBlockSize = process.argv[3]
    ? Number(process.argv[3])
    : undefined;
  const blockSize =
    overrideBlockSize ?? inferSectorSize(bytes) ?? 512;

  if (![512, 1024, 2048, 4096].includes(blockSize)) {
    throw new Error(
      `Unsupported block size ${blockSize}. Use 512, 1024, 2048 or 4096.`
    );
  }

  if (overrideBlockSize) {
    console.info("[fatfs-wasm] Using override block size", blockSize);
  }

  const blockCount = Math.floor(bytes.length / blockSize);
  if (blockCount === 0) {
    throw new Error("Image too small");
  }
  if (blockCount * blockSize !== bytes.length) {
    console.warn(
      "Image size is not a whole multiple of blockSize; trimming extras."
    );
  }

  const trimmed = bytes.slice(0, blockCount * blockSize);

const { createFatFSFromImage } = await import("../dist/fatfs/index.js");
const fatfs = await createFatFSFromImage(trimmed, {
  blockSize,
  blockCount
});
  console.log("Mounted FatFS image", blockCount, "blocks");

  const usage = fatfs.getUsage();
  const totalBytes = blockSize * blockCount;
  assert.strictEqual(
    usage.capacityBytes,
    totalBytes,
    "Usage reports the configured volume size"
  );
  assert.strictEqual(
    usage.usedBytes + usage.freeBytes,
    totalBytes,
    "Capacity = used + free"
  );
  assert.ok(usage.freeBytes >= 0, "Free bytes are non-negative");
  console.log("Usage", usage);

  const entries = fatfs.list();
  console.log("Entry count", entries.length);
  entries.forEach((entry) => console.log(entry.path + "\t" + entry.size));
  assert(entries.length > 0, "Listing should return at least one entry");

  const rootEntry = entries.find((entry) => entry.path === "/");
  assert(rootEntry, "Root directory entry should exist");
  assert.strictEqual(rootEntry?.type, "dir");

  const firstFile = entries.find((entry) => entry.type === "file");
  if (firstFile) {
    const fileBytes = fatfs.readFile(firstFile.path);
    assert.strictEqual(
      fileBytes.length,
      firstFile.size,
      "readFile returns the declared file size"
    );
  } else {
    console.info("No pre-existing files to read");
  }

  const testDir = "/fatfs-test";
  fatfs.mkdir(testDir);
  const dirList = fatfs.list(testDir);
  const normalizedTestDir = normalizeEntryPath(testDir);
  const dirEntry = dirList.find((entry) => entry.path === normalizedTestDir);
  assert(dirEntry, "Directory listing should include the directory itself");
  assert.strictEqual(dirEntry?.type, "dir", "Directory entry should be marked as 'dir'");

  const testFile = `${testDir}/smoke.txt`;
  const payload = Buffer.from(`fatfs test payload ${Date.now().toString()}`);
  fatfs.writeFile(testFile, payload);
  const roundTrip = fatfs.readFile(testFile);
  assert.strictEqual(Buffer.compare(roundTrip, payload), 0, "write/read round trip");
  const afterWriteUsage = fatfs.getUsage();
  assert.strictEqual(
    afterWriteUsage.capacityBytes,
    totalBytes,
    "Capacity should not change after writes"
  );
  assert.ok(
    afterWriteUsage.usedBytes >= usage.usedBytes,
    "Used bytes should not shrink after writes"
  );
  assert.strictEqual(
    afterWriteUsage.usedBytes + afterWriteUsage.freeBytes,
    totalBytes,
    "Usage should still sum to the volume size"
  );

  const renamed = `${testDir}/smoke-renamed.txt`;
  fatfs.rename(testFile, renamed);
  const renamedBytes = fatfs.readFile(renamed);
  assert.strictEqual(Buffer.compare(renamedBytes, payload), 0, "rename preserves contents");

  fatfs.deleteFile(renamed);
  assert.throws(
    () => fatfs.readFile(renamed),
    undefined,
    "Deleted file should not be readable"
  );

  const afterDeleteUsage = fatfs.getUsage();
  assert.strictEqual(
    afterDeleteUsage.capacityBytes,
    totalBytes,
    "Capacity should remain constant after deletion"
  );
  assert.ok(
    afterDeleteUsage.usedBytes <= afterWriteUsage.usedBytes,
    "Deleting a file should reduce used bytes"
  );

  const exported = fatfs.toImage();
  console.log("Exported image size after inspection", exported.length);
  assert.strictEqual(
    exported.length,
    totalBytes,
    "Exported image should cover the full volume"
  );
  console.log("FatFS smoke test completed successfully");
} catch (error) {
  console.error(error);
  process.exit(1);
} finally {
  globalThis.fetch = originalFetch;
}

function inferSectorSize(bytes) {
  if (bytes.length >= 13) {
    const candidate = bytes[11] | (bytes[12] << 8);
    if ([512, 1024, 2048, 4096].includes(candidate)) {
      console.info("[fatfs-wasm] Detected sector size", candidate);
      return candidate;
    }
  }
  return undefined;
}

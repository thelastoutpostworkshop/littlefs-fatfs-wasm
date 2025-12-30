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

  const entries = fatfs.list();
  console.log("Entry count", entries.length);
  entries.forEach((entry) => console.log(entry.path + "\t" + entry.size));

  let lastRead = null;
  for (const entry of entries) {
    try {
      const bytesValue = fatfs.readFile(entry.path);
      console.log(
        `SUCCESS read ${entry.path} (${bytesValue.length} bytes) ->`,
        Buffer.from(bytesValue).toString("utf8")
      );
      lastRead = entry.path;
      break;
    } catch (error) {
      console.warn(
        "Unable to read entry",
        entry.path,
        "->",
        error.message ?? error
      );
    }
  }

  if (!lastRead) {
    const testPath = "/fatfs-test.txt";
    const testPayload = Buffer.from(
      "fatfs wasm smoke payload " + Date.now().toString()
    );
    fatfs.writeFile(testPath, testPayload);
    const roundTrip = fatfs.readFile(testPath);
    console.log(
      "Created test file",
      testPath,
      "read back",
      roundTrip.length,
      "bytes ->",
      Buffer.from(roundTrip).toString("utf8")
    );
  }

  const exported = fatfs.toImage();
  console.log("Exported image size after inspection", exported.length);
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

import { readFile } from "node:fs/promises";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const repoRoot = process.cwd();
const wasmURL = pathToFileURL(path.join(repoRoot, "dist", "fatfs", "fatfs.wasm"));
const moduleUrl = pathToFileURL(path.join(repoRoot, "dist", "fatfs", "index.js"));

const imagePath = process.argv[2] ?? "C:\\Users\\charles\\Downloads\\fatfs_good.bin";

const originalFetch = globalThis.fetch;
if (typeof originalFetch !== "function") {
  throw new Error("fetch is not available in this Node runtime");
}

function isBootSector(bytes, offset) {
  if (offset + 512 > bytes.length) {
    return false;
  }
  const jump = bytes[offset];
  if (jump !== 0xeb && jump !== 0xe9) {
    return false;
  }
  const sig = bytes[offset + 510] | (bytes[offset + 511] << 8);
  if (sig !== 0xaa55) {
    return false;
  }
  const bps = bytes[offset + 11] | (bytes[offset + 12] << 8);
  return bps === 4096;
}

function sectorsEqual(bytes, offsetA, offsetB, length) {
  if (offsetA + length > bytes.length || offsetB + length > bytes.length) {
    return false;
  }
  for (let i = 0; i < length; i++) {
    if (bytes[offsetA + i] !== bytes[offsetB + i]) {
      return false;
    }
  }
  return true;
}

globalThis.fetch = async (input, init) => {
  const url =
    typeof input === "string"
      ? new URL(input)
      : input instanceof URL
      ? input
      : new URL(input.url);

  if (url.protocol === "file:") {
    const filePath = fileURLToPath(url);
    const data = await readFile(filePath);
    return new Response(data, { status: 200, headers: { "Content-Type": "application/wasm" } });
  }

  return originalFetch(input, init);
};

async function main() {
const { createFatFS, createFatFSFromImage, FAT_MOUNT } = await import(moduleUrl.href);

  const image = await readFile(imagePath);
  const hasBootMirror = isBootSector(image, 0) && isBootSector(image, 4096);
  const fs = await createFatFSFromImage(image, { wasmURL });

  const list = fs.list(FAT_MOUNT);
  console.log("list:", list);

  const bytes = fs.readFile("/fatfs/info.txt");
  const text = new TextDecoder().decode(bytes);
  console.log("info.txt:", JSON.stringify(text));

  const usage = fs.getUsage();
  console.log("usage:", usage);

  const exported = fs.toImage();
  console.log("exported bytes:", exported.length);

  const firstDir = list.find((entry) => entry.type === "dir");
  if (firstDir) {
    const dirList = fs.list(firstDir.path);
    console.log(`list (${firstDir.path}):`, dirList);

    const firstFile = dirList.find((entry) => entry.type === "file");
    if (firstFile) {
      const fileBytes = fs.readFile(firstFile.path);
      const fileText = new TextDecoder().decode(fileBytes);
      console.log(`${firstFile.path}:`, JSON.stringify(fileText));
    }
  }

  fs.format();
  const formattedImage = fs.toImage();
  const formattedRemount = await createFatFSFromImage(formattedImage, { wasmURL });
  const formattedList = formattedRemount.list(FAT_MOUNT);
  console.log("formatted remount list:", formattedList);
  if (formattedList.length !== 0) {
    throw new Error("formatted image did not mount cleanly");
  }
  if (hasBootMirror) {
    const mirrored = sectorsEqual(formattedImage, 0, 4096, 4096);
    console.log("formatted boot mirror:", mirrored);
    if (!mirrored) {
      throw new Error("formatted image boot mirror mismatch");
    }
  }

  const scratch = await createFatFS({ wasmURL, formatOnInit: true });
  scratch.format();
  scratch.mkdir("/fatfs/test_dir");
  scratch.writeFile("/fatfs/test_dir/hello.txt", "fatfs wasm test");
  scratch.writeFile("/fatfs/test_dir/rename_me.txt", "rename me");
  scratch.rename("/fatfs/test_dir/rename_me.txt", "/fatfs/test_dir/renamed.txt");

  const scratchList = scratch.list("/fatfs");
  console.log("scratch list:", scratchList);
  const readBack = scratch.readFile("/fatfs/test_dir/hello.txt");
  console.log("scratch read:", JSON.stringify(new TextDecoder().decode(readBack)));
  scratch.deleteFile("/fatfs/test_dir/renamed.txt");
  console.log("scratch usage:", scratch.getUsage());
  console.log("scratch image bytes:", scratch.toImage().length);

  scratch.writeFile("/fatfs/wipe_check.txt", "wipe me");
  scratch.format();
  const wipedList = scratch.list("/fatfs");
  console.log("after format list:", wipedList);
  if (wipedList.length !== 0) {
    throw new Error("format() did not wipe filesystem contents");
  }

  const wipedImage = scratch.toImage();
  const remount = await createFatFSFromImage(wipedImage, { wasmURL });
  const remountList = remount.list("/fatfs");
  console.log("remount list:", remountList);
  if (remountList.length !== 0) {
    throw new Error("formatted image failed to mount cleanly");
  }
}

try {
  await main();
  console.log("RESULT: PASS");
} catch (error) {
  console.error("RESULT: FAIL");
  console.error(error);
  process.exitCode = 1;
}

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

const { createFatFS, createFatFSFromImage, FAT_MOUNT } = await import(moduleUrl.href);

const image = await readFile(imagePath);
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

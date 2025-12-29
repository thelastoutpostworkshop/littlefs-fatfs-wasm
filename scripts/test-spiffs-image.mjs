import { readFile, stat } from 'node:fs/promises';
import { resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const originalFetch = globalThis.fetch;
globalThis.fetch = async (resource, init) => {
  const url = resource instanceof URL ? resource : new URL(resource.toString());
  if (url.protocol === 'file:') {
    const path = fileURLToPath(url);
    const body = await readFile(path);
    return new Response(body, { status: 200 });
  }
  return originalFetch ? originalFetch(resource, init) : fetch(resource, init);
};

const pathArg = process.argv[2];
if (!pathArg) {
  console.error('Usage: node scripts/test-spiffs-image.mjs <path>');
  process.exit(1);
}
const imagePath = resolve(pathArg);
const info = await stat(imagePath);
console.log('Image size', info.size);
const blockSize = 4096;
const blockCount = Math.floor(info.size / blockSize);
if (blockCount === 0) {
  throw new Error('image too small');
}
if (blockCount * blockSize !== info.size) {
  console.warn('Image size is not a whole multiple of blockSize; trimming extras.');
}
const bytes = await readFile(imagePath);
const trimmed = bytes.slice(0, blockCount * blockSize);

const { createSpiffsFromImage } = await import('../dist/spiffs/index.js');
const spiffs = await createSpiffsFromImage(trimmed, {
  blockSize,
  blockCount,
  pageSize: 256,
});
console.log('Mounted SPIFFS image', blockCount, 'blocks');
const entries = await spiffs.list();
console.log('Entry count', entries.length);
entries.forEach((e) => console.log(e.type + '\t' + e.size + '\t' + e.name));
if (entries.length > 0) {
  const first = entries[0];
  try {
    const bytesValue = await spiffs.read(first.name);
    console.log('First entry size ' + bytesValue.length + ' bytes');
  } catch (error) {
    console.warn('Unable to read first entry:', error.message ?? error);
  }
}
const usage = await spiffs.getUsage();
console.log('Usage', usage);

globalThis.fetch = originalFetch;

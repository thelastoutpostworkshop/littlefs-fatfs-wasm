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

try {
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
      console.log('First entry size ' + bytesValue.length + ' bytes', Buffer.from(bytesValue).toString('utf8'));
    } catch (error) {
      console.warn('Unable to read first entry:', error.message ?? error, 'code', error.code ?? '(n/a)');
    }
  }

  const usage = await spiffs.getUsage();
  console.log('Usage before format', usage);

  await spiffs.format();
  console.log('Filesystem formatted');

  const postFormatEntries = await spiffs.list();
  console.log('Entry count after format', postFormatEntries.length);

  const usageAfterFormat = await spiffs.getUsage();
  console.log('Usage after format', usageAfterFormat);

  if (typeof spiffs.canFit === 'function') {
    const fit = spiffs.canFit('/conformance.txt', 64);
    console.log('canFit /conformance.txt for 64 bytes?', fit);
  } else {
    console.log('canFit not available on this instance');
  }

  const testFile = '/spiffs-test-script.txt';
  const testPayload = Buffer.from('SPIFFS test payload ' + Date.now());
  await spiffs.write(testFile, testPayload);
  console.log('Wrote', testFile, testPayload.length, 'bytes');
  const readBack = await spiffs.read(testFile);
  console.log('Read back', testFile, readBack.length, 'bytes ->', Buffer.from(readBack).toString('utf8'));
  await spiffs.remove(testFile);
  console.log('Removed', testFile);

  const exported = await spiffs.toImage();
  console.log('Exported image size after cleanup', exported.length);
} finally {
  globalThis.fetch = originalFetch;
}

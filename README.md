## littlefs-wasm

WASM bindings for LittleFS, FatFS, and SPIFFS with RAM backed block devices. This README focuses on how to use the interfaces and how to test them.

### Using the WASM interfaces

All three modules expose a `create*` function that returns a filesystem instance plus a `create*FromImage` helper to mount an existing image without formatting. The storage lives in memory, so `toImage()` returns a `Uint8Array` snapshot you can save or flash elsewhere.

Each `create*` accepts a `wasmURL` option when you need to override asset resolution. By default the modules load `*.wasm` relative to `import.meta.url`, so bundlers can track the assets automatically.

#### LittleFS

```ts
import { createLittleFS, createLittleFSFromImage } from "littlefs-wasm/littlefs";

const fs = await createLittleFS({
  formatOnInit: true,
  blockSize: 512,
  blockCount: 512,
});

fs.mkdir("docs");
fs.writeFile("docs/readme.txt", "hello littlefs");
console.log(fs.list("/"));

const bytes = fs.readFile("docs/readme.txt");
console.log(new TextDecoder().decode(bytes));

fs.rename("docs/readme.txt", "docs/hello.txt");
fs.delete("docs", { recursive: true });

const image = fs.toImage();
const mounted = await createLittleFSFromImage(image, {
  blockSize: 512,
  blockCount: image.length / 512,
});
console.log(mounted.getUsage());
```

Key methods:

```ts
interface LittleFS {
  format(): void;
  list(path?: string): Array<{ path: string; size: number; type: "file" | "dir" }>;
  writeFile(path: string, data: Uint8Array | ArrayBuffer | string): void;
  readFile(path: string): Uint8Array;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
  delete(path: string, options?: { recursive?: boolean }): void;
  toImage(): Uint8Array;
  getUsage(): { capacityBytes: number; usedBytes: number; freeBytes: number };
}
```

#### FatFS

```ts
import { createFatFS, createFatFSFromImage, FAT_MOUNT } from "littlefs-wasm/fatfs";

const fatfs = await createFatFS({ formatOnInit: true });

fatfs.mkdir(`${FAT_MOUNT}/notes`);
fatfs.writeFile(`${FAT_MOUNT}/notes/todo.txt`, "fatfs wasm");
console.log(fatfs.list(FAT_MOUNT));

const bytes = fatfs.readFile(`${FAT_MOUNT}/notes/todo.txt`);
console.log(new TextDecoder().decode(bytes));

const image = fatfs.toImage();
const mounted = await createFatFSFromImage(image, { wasmURL: new URL("./fatfs.wasm", import.meta.url) });
console.log(mounted.getUsage());
```

Notes:

- `FAT_MOUNT` is `/fatfs`. Use it as the root when reading and writing so paths match ESP32 firmware behavior.
- `writeFile` creates intermediate directories if they do not exist.

Key methods:

```ts
interface FatFS {
  format(): void;
  list(path?: string): Array<{ path: string; size: number; type: "file" | "dir" }>;
  writeFile(path: string, data: Uint8Array | ArrayBuffer | string): void;
  readFile(path: string): Uint8Array;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
  deleteFile(path: string): void;
  toImage(): Uint8Array;
  getUsage(): { capacityBytes: number; usedBytes: number; freeBytes: number };
}
```

#### SPIFFS

SPIFFS is async and uses a slightly different API (`read`, `write`, `remove`).

```ts
import { createSpiffs, createSpiffsFromImage } from "littlefs-wasm/spiffs";

const spiffs = await createSpiffs({
  blockSize: 4096,
  blockCount: 256,
  pageSize: 256,
  formatOnInit: true,
});

await spiffs.write("/logs/ready.txt", "spiffs ready");
const entries = await spiffs.list();
console.log(entries);

const bytes = await spiffs.read("/logs/ready.txt");
console.log(new TextDecoder().decode(bytes));

const image = await spiffs.toImage();
const mounted = await createSpiffsFromImage(image, {
  blockSize: 4096,
  blockCount: image.length / 4096,
  pageSize: 256,
});
console.log(await mounted.getUsage());
```

Key methods:

```ts
interface Spiffs {
  list(): Promise<Array<{ name: string; size: number; type: "file" | "dir" }>>;
  read(name: string): Promise<Uint8Array>;
  write(name: string, data: Uint8Array | ArrayBuffer | string): Promise<void>;
  remove(name: string): Promise<void>;
  format(): Promise<void>;
  toImage(): Promise<Uint8Array>;
  getUsage(): Promise<{ capacityBytes: number; usedBytes: number; freeBytes: number }>;
  canFit?(name: string, dataLength: number): boolean;
}
```

### Testing

All tests import from `dist`, so build first:

```bash
npm install
npm run build
```

#### LittleFS self test

```bash
node scripts/test-littlefs.mjs
```

Expected output ends with `littlefs self-test passed`.

#### FatFS image test

```bash
npm run test:fatfs -- <path-to-fatfs-image>
# or
node scripts/test-fatfs-image.mjs <path-to-fatfs-image>
```

The script mounts the supplied image, reads files under `/fatfs`, formats, and re-mounts. Pass an explicit path; the default path in the script is machine specific.

#### SPIFFS image test

```bash
npm run test:spiffs -- <path-to-spiffs-image>
# or
node scripts/test-spiffs-image.mjs <path-to-spiffs-image>
```

The script expects a raw image whose size is a multiple of the block size (4096). If it is not, the script trims the extra bytes.

## littlefs-wasm

Tiny, dependency-free LittleFS **and FatFS** bindings compiled with Emscripten for browsers. Both modules keep their data in RAM-backed block devices so you can mount raw flash images, mutate them entirely in the browser, and export the updated bytes without any native shims.

### Features

- Upstream LittleFS (vendored in `third_party/littlefs`) and ESP-IDF’s FatFS (elm-chan) with the same defaults we use in firmware (512-byte sectors, 32 KB cluster cap, LFN enabled).
- Configurable RAM block devices; mount existing images or start fresh.
- TypeScript-first API with no runtime dependencies.
- Ships as ES modules plus `.wasm` assets (`import.meta.url` friendly) so bundlers (Vite/Rollup/Webpack/etc.) can track them automatically.
- Minimal build pipeline (`npm run build`) produces `dist/littlefs/*` and `dist/fatfs/*` bundles ready for `public/wasm/{littlefs|fatfs}`.

### Quick Start

#### LittleFS

```ts
import { createLittleFS } from "littlefs-wasm/littlefs";

const fs = await createLittleFS({ formatOnInit: true });

fs.mkdir("docs");
fs.writeFile("docs/readme.txt", "Hello LittleFS!");
fs.writeFile("images/icon.bin", new Uint8Array([0xde, 0xad]));

console.log(fs.list("/"));
// => [ { path: "docs", type: "dir", size: 0 }, { path: "docs/readme.txt", type: "file", size: 16 }, ... ]

const bytes = fs.readFile("docs/readme.txt");
console.log(new TextDecoder().decode(bytes)); // "Hello LittleFS!"

fs.rename("docs/readme.txt", "docs/hello.txt");
fs.delete("images", { recursive: true });
const image = fs.toImage(); // Entire filesystem image as Uint8Array
```

#### FatFS

```ts
import { createFatFS } from "littlefs-wasm/fatfs";

const fatfs = await createFatFS({ formatOnInit: true });

fatfs.writeFile("MUSIC/track1.raw", someAudioBytes);
fatfs.writeFile("NOTES/todo.txt", "Ship FatFS preview");

console.log(fatfs.list());
// => [
//      { path: "/", type: "dir", size: 0 },
//      { path: "MUSIC", type: "dir", size: 0 },
//      { path: "MUSIC/track1.raw", type: "file", size: 32768 },
//      { path: "NOTES", type: "dir", size: 0 },
//      { path: "NOTES/todo.txt", type: "file", size: 17 }
//    ]

console.log(fatfs.list("MUSIC"));
// => [
//      { path: "MUSIC", type: "dir", size: 0 },
//      { path: "MUSIC/track1.raw", type: "file", size: 32768 }
//    ]

const exported = fatfs.toImage(); // Persist it back to disk or flash.
```

#### SPIFFS

```ts
import { createSpiffs } from "littlefs-wasm/spiffs";

const spiffs = await createSpiffs({
  pageSize: 256,
  blockSize: 4096,
  blockCount: 256,
  formatOnInit: true
});

await spiffs.write("logs/ready.txt", "SPIFFS is ready!");
console.log(await spiffs.list());
const usage = await spiffs.getUsage();
console.log("Free bytes", usage.freeBytes);
```

SPIFFS exposes the same RAM-backed benefits as the other bindings, including `list`, `read`, `write`, `remove`, `format`, `toImage`, `getUsage`, and the optional `canFit` guard for pacing writes.

### API surface

```ts
export async function createLittleFS(options?: LittleFSOptions): Promise<LittleFS>;
export async function createLittleFSFromImage(image: ArrayBuffer | Uint8Array, options?: LittleFSOptions): Promise<LittleFS>;

export async function createFatFS(options?: FatFSOptions): Promise<FatFS>;
export async function createFatFSFromImage(image: ArrayBuffer | Uint8Array, options?: FatFSOptions): Promise<FatFS>;

export async function createSpiffs(options?: SpiffsOptions): Promise<Spiffs>;
export async function createSpiffsFromImage(image: ArrayBuffer | Uint8Array, options?: SpiffsOptions): Promise<Spiffs>;

interface LittleFS {
  format(): void;
  list(path?: string): Array<{ path: string; size: number; type: "file" | "dir" }>;
  addFile(path: string, data: Uint8Array | ArrayBuffer | string): void; // alias of writeFile
  writeFile(path: string, data: Uint8Array | ArrayBuffer | string): void;
  deleteFile(path: string): void; // alias of delete
  delete(path: string, options?: { recursive?: boolean }): void;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
  readFile(path: string): Uint8Array;
  toImage(): Uint8Array;
}

interface FatFS {
  format(): void;
  list(path?: string): Array<{ path: string; size: number; type: "file" | "dir" }>;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
  writeFile(path: string, data: Uint8Array | ArrayBuffer | string): void;
  readFile(path: string): Uint8Array;
  deleteFile(path: string): void;
  toImage(): Uint8Array;
}

interface LittleFSOptions {
  blockSize?: number;      // default 512 bytes
  blockCount?: number;     // default 512 blocks (256 KiB)
  lookaheadSize?: number;  // default 32 bytes
  wasmURL?: string | URL;  // override asset resolution
  formatOnInit?: boolean;  // auto-format right after init
}

interface FatFSOptions {
  blockSize?: number;      // default 512-byte sectors
  blockCount?: number;     // default 1024 sectors (512 KiB)
  wasmURL?: string | URL;
  formatOnInit?: boolean;
}

interface SpiffsEntry {
  name: string;
  size: number;
  type: "file" | "dir";
}

interface SpiffsUsage {
  capacityBytes: number;
  usedBytes: number;
  freeBytes: number;
}

interface Spiffs {
  list(): Promise<SpiffsEntry[]>;
  read(name: string): Promise<Uint8Array>;
  write(name: string, data: Uint8Array | ArrayBuffer | string): Promise<void>;
  remove(name: string): Promise<void>;
  format(): Promise<void>;
  toImage(): Promise<Uint8Array>;
  getUsage(): Promise<SpiffsUsage>;
  canFit?(name: string, dataLength: number): boolean;
}

interface SpiffsOptions {
  pageSize?: number;      // default 256 bytes
  blockSize?: number;     // default 4096 bytes per block
  blockCount?: number;    // default 256 blocks (1 MiB)
  fdCount?: number;       // default 16
  cachePages?: number;    // default 64
  wasmURL?: string | URL;
  formatOnInit?: boolean;
}
```

`list()` returns every file and directory (depth-first) under the given path. Entries include normalized paths, type, and byte sizes. `createLittleFSFromImage` / `createFatFSFromImage` mount existing flash images without formatting, `toImage()` exports the current contents, and `readFile()` returns raw file bytes so your Vue client can preview text/images/audio. FatFS automatically creates missing directories for `writeFile`.

### Building from source

Requirements:

- [Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (`emcc`) available in your shell.
- Node.js ≥ 18 for the TypeScript build pipeline.

Steps:

```bash
npm install
npm run build:wasm   # compiles littlefs.wasm + fatfs.wasm into dist/littlefs|fatfs
npm run build:types  # compiles src/ts -> dist (ES2020 JS + .d.ts)
# or just run both:
npm run build
```

Copy the relevant folders (`dist/littlefs/*` and `dist/fatfs/*`) into your web app (e.g., `public/wasm/littlefs/` and `public/wasm/fatfs/`). The modules default to `new URL("./littlefs.wasm", import.meta.url)` / `new URL("./fatfs.wasm", import.meta.url)`, so bundlers automatically include the binary assets.

### File layout

```
src/
  |- c/littlefs_wasm.c      # LittleFS glue + RAM block device
  |- c/fatfs_wasm.c         # FatFS glue + RAM block device
  |- ts/littlefs/index.ts   # Typed LittleFS API
  |- ts/fatfs/index.ts      # Typed FatFS API
third_party/
  |- littlefs               # Upstream LittleFS sources
  |- fatfs                  # ESP-IDF/elm-chan FatFS sources + config
scripts/build-wasm.mjs      # Builds both wasm binaries
dist/
  |- littlefs/{index.js,index.d.ts,littlefs.wasm}
  |- fatfs/{index.js,index.d.ts,fatfs.wasm}
  |- index.{js,d.ts}        # Aggregate re-export
```

### Notes

- RAM storage is initialized to `0xFF` (NOR flash style). `format()` wipes and recreates the filesystem.
- `createLittleFS` retries mounting after formatting by default; pass `formatOnInit: true` for a guaranteed clean start. FatFS behaves the same.
- When bundling, ensure both wasm assets are copied/served so `fetch(new URL("./*.wasm", import.meta.url))` succeeds.
- Error codes bubble through `LittleFSError` (LittleFS) and `FatFSError` (FatFS) with the original numeric codes (`lfs.h` / `ff.h`) so advanced clients can inspect them.

### Licensing

- `littlefs-wasm`: MIT (see `LICENSE`).
- `third_party/littlefs`: retains its upstream license (`third_party/littlefs/LICENSE.md`).
- `third_party/fatfs`: retains the FatFS license (`third_party/fatfs/LICENSE.txt`).

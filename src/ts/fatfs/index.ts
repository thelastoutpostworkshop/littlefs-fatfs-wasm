import type { BinarySource, FileSource, FileSystemUsage } from "../shared/types";

export const FAT_MOUNT = "/fatfs";

const INITIAL_LIST_BUFFER = 4096;
const FATFS_ERR_NOSPC = -5;
const FATFS_ERR_UNSUPPORTED = -6;

export interface FatFSEntry {
  path: string;
  size: number;
  type: "file" | "dir";
}

export interface FatFSOptions {
  wasmURL?: string | URL;
}

export interface FatFS {
  list(path?: string): FatFSEntry[];
  readFile(path: string): Uint8Array;
  toImage(): Uint8Array;
  getUsage(): FileSystemUsage;
  format(): void;
  writeFile(path: string, data: FileSource): void;
  deleteFile(path: string): void;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
}

interface FatFSExports {
  memory: WebAssembly.Memory;
  fatfsjs_init(blockSize: number, blockCount: number): number;
  fatfsjs_init_from_image(imagePtr: number, imageLen: number): number;
  fatfsjs_format(): number;
  fatfsjs_write_file(pathPtr: number, dataPtr: number, dataLen: number): number;
  fatfsjs_delete_file(pathPtr: number): number;
  fatfsjs_list(pathPtr: number, bufferPtr: number, bufferLen: number): number;
  fatfsjs_file_size(pathPtr: number): number;
  fatfsjs_read_file(
    pathPtr: number,
    bufferPtr: number,
    bufferLen: number
  ): number;
  fatfsjs_export_image(bufferPtr: number, bufferLen: number): number;
  fatfsjs_storage_size(): number;
  malloc(size: number): number;
  free(ptr: number): void;
}

export class FatFSError extends Error {
  readonly code: number;

  constructor(message: string, code: number) {
    super(message);
    this.code = code;
    this.name = "FatFSError";
  }
}

export async function createFatFSFromImage(
  image: BinarySource,
  options: FatFSOptions = {}
): Promise<FatFS> {
  console.info("[fatfs-wasm] createFatFSFromImage() starting");
  const wasmURL = options.wasmURL ?? new URL("./fatfs.wasm", import.meta.url);
  const exports = await instantiateFatFSModule(wasmURL);
  const bytes = asBinaryUint8Array(image);

  const heap = new Uint8Array(exports.memory.buffer);
  const imagePtr = exports.malloc(bytes.length || 1);
  if (!imagePtr) {
    throw new FatFSError("Failed to allocate WebAssembly memory", FATFS_ERR_NOSPC);
  }

  try {
    heap.set(bytes, imagePtr);
    const initResult = exports.fatfsjs_init_from_image(imagePtr, bytes.length);
    if (initResult < 0) {
      throw new FatFSError("Failed to initialize FAT16 image", initResult);
    }
  } finally {
    exports.free(imagePtr);
  }

  console.info("[fatfs-wasm] Filesystem initialized from image");
  return new FatFSClient(exports);
}

export async function createFatFS(_: FatFSOptions = {}): Promise<FatFS> {
  throw new FatFSError(
    "FAT16 reader is read-only; use createFatFSFromImage()",
    FATFS_ERR_UNSUPPORTED
  );
}

class FatFSClient implements FatFS {
  private readonly exports: FatFSExports;
  private heapU8: Uint8Array;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();
  private listBufferSize = INITIAL_LIST_BUFFER;

  constructor(exports: FatFSExports) {
    this.exports = exports;
    this.heapU8 = new Uint8Array(this.exports.memory.buffer);
  }

  list(path: string = FAT_MOUNT): FatFSEntry[] {
    const normalized = normalizeMountPath(path);
    const basePath = normalized;

    const pathPtr = this.allocString(normalized);
    let capacity = this.listBufferSize;

    try {
      while (true) {
        const ptr = this.alloc(capacity);
        try {
          const used = this.exports.fatfsjs_list(pathPtr, ptr, capacity);
          if (used === FATFS_ERR_NOSPC) {
            this.listBufferSize = capacity * 2;
            capacity = this.listBufferSize;
            continue;
          }
          this.assertOk(used, "list files");
          if (used === 0) {
            return [];
          }
          const payload = this.decoder.decode(this.heapU8.subarray(ptr, ptr + used));
          return parseListPayload(payload).map((entry) => ({
            ...entry,
            path: joinListPath(basePath, entry.path),
          }));
        } finally {
          this.exports.free(ptr);
        }
      }
    } finally {
      this.exports.free(pathPtr);
    }
  }

  readFile(path: string): Uint8Array {
    const normalized = normalizeMountPath(path);
    if (normalized === FAT_MOUNT) {
      throw new FatFSError("Path must point to a file", FATFS_ERR_UNSUPPORTED);
    }

    const pathPtr = this.allocString(normalized);
    try {
      const size = this.exports.fatfsjs_file_size(pathPtr);
      this.assertOk(size, `stat file "${normalized}"`);
      if (size === 0) {
        return new Uint8Array();
      }

      const dataPtr = this.alloc(size);
      try {
        const read = this.exports.fatfsjs_read_file(pathPtr, dataPtr, size);
        this.assertOk(read, `read file "${normalized}"`);
        return this.heapU8.slice(dataPtr, dataPtr + size);
      } finally {
        this.exports.free(dataPtr);
      }
    } finally {
      this.exports.free(pathPtr);
    }
  }

  toImage(): Uint8Array {
    const size = this.exports.fatfsjs_storage_size();
    if (size === 0) {
      return new Uint8Array();
    }
    const ptr = this.alloc(size);
    try {
      const copied = this.exports.fatfsjs_export_image(ptr, size);
      this.assertOk(copied, "export filesystem image");
      return this.heapU8.slice(ptr, ptr + size);
    } finally {
      this.exports.free(ptr);
    }
  }

  getUsage(): FileSystemUsage {
    const capacityBytes = this.exports.fatfsjs_storage_size();
    const entries = this.list(FAT_MOUNT);
    const usedBytes = entries.reduce(
      (acc, entry) => (entry.type === "file" ? acc + entry.size : acc),
      0
    );
    const freeBytes = capacityBytes > usedBytes ? capacityBytes - usedBytes : 0;
    return {
      capacityBytes,
      usedBytes,
      freeBytes,
    };
  }

  format(): void {
    throw new FatFSError("FAT16 reader is read-only", FATFS_ERR_UNSUPPORTED);
  }

  writeFile(_path: string, _data: FileSource): void {
    throw new FatFSError("FAT16 reader is read-only", FATFS_ERR_UNSUPPORTED);
  }

  deleteFile(_path: string): void {
    throw new FatFSError("FAT16 reader is read-only", FATFS_ERR_UNSUPPORTED);
  }

  mkdir(_path: string): void {
    throw new FatFSError("FAT16 reader is read-only", FATFS_ERR_UNSUPPORTED);
  }

  rename(_oldPath: string, _newPath: string): void {
    throw new FatFSError("FAT16 reader is read-only", FATFS_ERR_UNSUPPORTED);
  }

  private refreshHeap(): void {
    if (this.heapU8.buffer !== this.exports.memory.buffer) {
      this.heapU8 = new Uint8Array(this.exports.memory.buffer);
    }
  }

  private alloc(size: number): number {
    if (size <= 0) {
      return 0;
    }
    const ptr = this.exports.malloc(size);
    if (!ptr) {
      throw new FatFSError("Failed to allocate WebAssembly memory", FATFS_ERR_NOSPC);
    }
    this.refreshHeap();
    return ptr;
  }

  private allocString(value: string): number {
    const encoded = this.encoder.encode(value);
    const ptr = this.alloc(encoded.length + 1);
    this.heapU8.set(encoded, ptr);
    this.heapU8[ptr + encoded.length] = 0;
    return ptr;
  }

  private assertOk(code: number, action: string): void {
    if (code < 0) {
      throw new FatFSError(`Unable to ${action}`, code);
    }
  }
}

async function instantiateFatFSModule(input: string | URL): Promise<FatFSExports> {
  const source = resolveWasmURL(input);
  console.info("[fatfs-wasm] Fetching wasm from", source.href);
  const wasmContext: WasmContext = { memory: null };
  const imports: WebAssembly.Imports = createDefaultImports(wasmContext);

  let response = await fetch(source);
  if (!response.ok) {
    throw new Error(`Unable to fetch FATFS wasm from ${response.url}`);
  }
  console.info("[fatfs-wasm] Fetch complete, status", response.status);

  if ("instantiateStreaming" in WebAssembly && typeof WebAssembly.instantiateStreaming === "function") {
    try {
      console.info("[fatfs-wasm] Attempting instantiateStreaming");
      const streaming = await WebAssembly.instantiateStreaming(response, imports);
      wasmContext.memory = getExportedMemory(streaming.instance.exports);
      console.info("[fatfs-wasm] instantiateStreaming succeeded");
      return streaming.instance.exports as unknown as FatFSExports;
    } catch (error) {
      console.warn(
        "Unable to instantiate FATFS wasm via streaming, retrying with arrayBuffer()",
        error
      );
      response = await fetch(source);
      if (!response.ok) {
        throw new Error(`Unable to fetch FATFS wasm from ${response.url}`);
      }
      console.info("[fatfs-wasm] Fallback fetch complete, status", response.status);
    }
  }

  console.info("[fatfs-wasm] Instantiating from ArrayBuffer fallback");
  const bytes = await response.arrayBuffer();
  const instance = await WebAssembly.instantiate(bytes, imports);
  wasmContext.memory = getExportedMemory(instance.instance.exports);
  console.info("[fatfs-wasm] instantiate(bytes) succeeded");
  return instance.instance.exports as unknown as FatFSExports;
}

function parseListPayload(payload: string): FatFSEntry[] {
  if (!payload) {
    return [];
  }
  return payload
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => {
      const [rawPath, rawSize, rawType] = line.split("\t");
      return {
        path: rawPath ?? "",
        size: Number(rawSize ?? "0") || 0,
        type: rawType === "d" ? "dir" : "file",
      };
    });
}

function normalizeMountPath(input?: string): string {
  const raw = (input ?? "").trim();
  if (!raw || raw === "/") {
    return FAT_MOUNT;
  }
  const normalized = raw.replace(/\\/g, "/").replace(/\/{2,}/g, "/");
  const collapsed =
    normalized.endsWith("/") && normalized !== "/" ? normalized.slice(0, -1) : normalized;
  const lower = collapsed.toLowerCase();
  if (lower.startsWith(FAT_MOUNT)) {
    const rest = collapsed.slice(FAT_MOUNT.length);
    return rest ? `${FAT_MOUNT}${rest}` : FAT_MOUNT;
  }
  if (collapsed.startsWith("/")) {
    return `${FAT_MOUNT}${collapsed}`;
  }
  return `${FAT_MOUNT}/${collapsed}`;
}

function joinListPath(basePath: string, entryPath: string): string {
  const base = basePath.replace(/\/+$/, "");
  if (!entryPath || entryPath === "/") {
    return base || FAT_MOUNT;
  }
  const trimmed = entryPath.replace(/^\/+/, "");
  return base === FAT_MOUNT ? `${FAT_MOUNT}/${trimmed}` : `${base}/${trimmed}`;
}

function asBinaryUint8Array(source: BinarySource): Uint8Array {
  if (source instanceof Uint8Array) {
    return source;
  }
  if (source instanceof ArrayBuffer) {
    return new Uint8Array(source);
  }
  throw new Error("Expected Uint8Array or ArrayBuffer for filesystem image");
}

function resolveWasmURL(input: string | URL): URL {
  if (input instanceof URL) {
    return input;
  }

  const locationLike =
    typeof globalThis !== "undefined" && "location" in globalThis
      ? (globalThis as { location?: Location }).location
      : undefined;
  const baseHref = locationLike?.href;

  try {
    return baseHref ? new URL(input, baseHref) : new URL(input);
  } catch (error) {
    throw new Error(`Unable to resolve wasm URL from "${input}": ${String(error)}`);
  }
}

interface WasmContext {
  memory: WebAssembly.Memory | null;
}

function createDefaultImports(context: WasmContext): WebAssembly.Imports {
  const noop = () => {};
  const ok = () => 0;

  return {
    env: {
      emscripten_notify_memory_growth: noop,
    },
    wasi_snapshot_preview1: {
      fd_close: ok,
      fd_seek: ok,
      fd_write: (fd: number, iov: number, iovcnt: number, pnum: number) =>
        handleFdWrite(context, fd, iov, iovcnt, pnum),
    },
  };
}

function handleFdWrite(
  context: WasmContext,
  fd: number,
  iov: number,
  iovcnt: number,
  pnum: number
): number {
  const memory = context.memory;
  if (!memory) {
    return 0;
  }

  const view = new DataView(memory.buffer);
  let total = 0;
  for (let i = 0; i < iovcnt; i++) {
    const base = iov + i * 8;
    const ptr = view.getUint32(base, true);
    const len = view.getUint32(base + 4, true);
    total += len;

    if (fd === 1 || fd === 2) {
      const bytes = new Uint8Array(memory.buffer, ptr, len);
      const text = new TextDecoder().decode(bytes);
      console.info(`[fatfs-wasm::fd_write fd=${fd}] ${text}`);
    }
  }

  view.setUint32(pnum, total, true);
  return 0;
}

function getExportedMemory(exports: WebAssembly.Exports): WebAssembly.Memory | null {
  for (const value of Object.values(exports)) {
    if (value instanceof WebAssembly.Memory) {
      return value;
    }
  }
  return null;
}

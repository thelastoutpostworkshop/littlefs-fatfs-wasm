import type { FileSource, BinarySource } from "../shared/types";

const DEFAULT_BLOCK_SIZE = 512;
const DEFAULT_BLOCK_COUNT = 512;
const DEFAULT_LOOKAHEAD_SIZE = 32;
const INITIAL_LIST_BUFFER = 4096;
const LFS_ERR_NOSPC = -28;

export interface LittleFSEntry {
  path: string;
  size: number;
  type: "file" | "dir";
}

export interface LittleFSOptions {
  blockSize?: number;
  blockCount?: number;
  lookaheadSize?: number;
  /**
   * Optional override for the wasm asset location. Useful when bundlers move files.
   */
  wasmURL?: string | URL;
  /**
   * Formats the filesystem immediately after initialization.
   */
  formatOnInit?: boolean;
}

export interface LittleFS {
  format(): void;
  list(path?: string): LittleFSEntry[];
  addFile(path: string, data: FileSource): void;
  writeFile(path: string, data: FileSource): void;
  deleteFile(path: string): void; // backward-compat alias
  delete(path: string, options?: { recursive?: boolean }): void;
  mkdir(path: string): void;
  rename(oldPath: string, newPath: string): void;
  toImage(): Uint8Array;
  readFile(path: string): Uint8Array;
}

interface LittleFSExports {
  memory: WebAssembly.Memory;
  lfsjs_init(blockSize: number, blockCount: number, lookaheadSize: number): number;
  lfsjs_init_from_image(
    blockSize: number,
    blockCount: number,
    lookaheadSize: number,
    imagePtr: number,
    imageLength: number
  ): number;
  lfsjs_format(): number;
  lfsjs_list(pathPtr: number, bufferPtr: number, bufferLen: number): number;
  lfsjs_add_file(pathPtr: number, dataPtr: number, dataLen: number): number;
  lfsjs_delete_file(pathPtr: number): number;
  lfsjs_remove(pathPtr: number, recursive: number): number;
  lfsjs_mkdir(pathPtr: number): number;
  lfsjs_rename(oldPathPtr: number, newPathPtr: number): number;
  lfsjs_file_size(pathPtr: number): number;
  lfsjs_read_file(pathPtr: number, bufferPtr: number, bufferLen: number): number;
  lfsjs_export_image(bufferPtr: number, bufferLen: number): number;
  lfsjs_storage_size(): number;
  malloc(size: number): number;
  free(ptr: number): void;
}

export class LittleFSError extends Error {
  readonly code: number;

  constructor(message: string, code: number) {
    super(message);
    this.code = code;
    this.name = "LittleFSError";
  }
}

export async function createLittleFS(options: LittleFSOptions = {}): Promise<LittleFS> {
  console.info("[littlefs-wasm] createLittleFS() starting", options);
  const wasmURL = options.wasmURL ?? new URL("./littlefs.wasm", import.meta.url);
  const exports = await instantiateLittleFSModule(wasmURL);

  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  const blockCount = options.blockCount ?? DEFAULT_BLOCK_COUNT;
  const lookaheadSize = options.lookaheadSize ?? DEFAULT_LOOKAHEAD_SIZE;

  console.info("[littlefs-wasm] Calling lfsjs_init with", {
    blockSize,
    blockCount,
    lookaheadSize
  });
  const initResult = exports.lfsjs_init(blockSize, blockCount, lookaheadSize);
  console.info("[littlefs-wasm] lfsjs_init returned", initResult);
  if (initResult < 0) {
    throw new LittleFSError("Failed to initialize LittleFS", initResult);
  }

  if (options.formatOnInit) {
    console.info("[littlefs-wasm] Calling lfsjs_format()");
    const formatResult = exports.lfsjs_format();
    console.info("[littlefs-wasm] lfsjs_format returned", formatResult);
    if (formatResult < 0) {
      throw new LittleFSError("Failed to format filesystem", formatResult);
    }
  }

  console.info("[littlefs-wasm] Filesystem initialized");
  const client = new LittleFSClient(exports);
  client.refreshStorageSize();
  return client;
}

export async function createLittleFSFromImage(image: BinarySource, options: LittleFSOptions = {}): Promise<LittleFS> {
  console.info("[littlefs-wasm] createLittleFSFromImage() starting");
  const wasmURL = options.wasmURL ?? new URL("./littlefs.wasm", import.meta.url);
  const exports = await instantiateLittleFSModule(wasmURL);
  const bytes = asBinaryUint8Array(image);

  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  if (blockSize === 0) {
    throw new Error("blockSize must be a positive integer");
  }
  const inferredBlockCount = bytes.length / blockSize;
  const blockCount = options.blockCount ?? inferredBlockCount;
  if (blockCount * blockSize !== bytes.length) {
    throw new Error("Image size must equal blockSize * blockCount");
  }
  const lookaheadSize = options.lookaheadSize ?? DEFAULT_LOOKAHEAD_SIZE;

  const heap = new Uint8Array(exports.memory.buffer);
  const imagePtr = exports.malloc(bytes.length || 1);
  if (!imagePtr) {
    throw new LittleFSError("Failed to allocate WebAssembly memory", LFS_ERR_NOSPC);
  }

  try {
    heap.set(bytes, imagePtr);
    const initResult = exports.lfsjs_init_from_image(blockSize, blockCount, lookaheadSize, imagePtr, bytes.length);
    if (initResult < 0) {
      throw new LittleFSError("Failed to initialize LittleFS from image", initResult);
    }
  } finally {
    exports.free(imagePtr);
  }

  const client = new LittleFSClient(exports);
  client.refreshStorageSize();
  console.info("[littlefs-wasm] Filesystem initialized from image");
  return client;
}

class LittleFSClient implements LittleFS {
  private readonly exports: LittleFSExports;
  private heapU8: Uint8Array;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();
  private listBufferSize = INITIAL_LIST_BUFFER;
  private storageSize = 0;

  constructor(exports: LittleFSExports) {
    this.exports = exports;
    this.heapU8 = new Uint8Array(this.exports.memory.buffer);
    this.refreshStorageSize();
  }

  format(): void {
    const result = this.exports.lfsjs_format();
    this.assertOk(result, "format filesystem");
  }

  list(path = "/"): LittleFSEntry[] {
    const normalizedPath = normalizePathOptional(path);
    const pathPtr = this.allocString(normalizedPath);
    let capacity = this.listBufferSize;

    while (true) {
      const ptr = this.alloc(capacity);
      try {
        const used = this.exports.lfsjs_list(pathPtr, ptr, capacity);
        if (used === LFS_ERR_NOSPC) {
          this.listBufferSize = capacity * 2;
          capacity = this.listBufferSize;
          continue;
        }
        this.assertOk(used, "list files");
        if (used === 0) {
          return [];
        }
        const payload = this.decoder.decode(this.heapU8.subarray(ptr, ptr + used));
        return parseListPayload(payload);
      } finally {
        this.exports.free(ptr);
      }
    }
  }

  addFile(path: string, data: FileSource): void {
    this.writeFile(path, data);
  }

  writeFile(path: string, data: FileSource): void {
    const normalizedPath = normalizePath(path);
    const payload = asUint8Array(data, this.encoder);

    const pathPtr = this.allocString(normalizedPath);
    const dataPtr = this.alloc(payload.length);

    try {
      this.heapU8.set(payload, dataPtr);
      const result = this.exports.lfsjs_add_file(pathPtr, dataPtr, payload.length);
      this.assertOk(result, `add file at "${normalizedPath}"`);
    } finally {
      this.exports.free(dataPtr);
      this.exports.free(pathPtr);
    }
  }

  delete(path: string, options?: { recursive?: boolean }): void {
    const recursive = options?.recursive === true;
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const result = this.exports.lfsjs_remove(pathPtr, recursive ? 1 : 0);
      this.assertOk(result, `delete "${normalizedPath}"${recursive ? " (recursive)" : ""}`);
    } finally {
      this.exports.free(pathPtr);
    }
  }

  deleteFile(path: string): void {
    this.delete(path);
  }

  mkdir(path: string): void {
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const result = this.exports.lfsjs_mkdir(pathPtr);
      this.assertOk(result, `mkdir "${normalizedPath}"`);
    } finally {
      this.exports.free(pathPtr);
    }
  }

  rename(oldPath: string, newPath: string): void {
    const from = normalizePath(oldPath);
    const to = normalizePath(newPath);
    const fromPtr = this.allocString(from);
    const toPtr = this.allocString(to);
    try {
      const result = this.exports.lfsjs_rename(fromPtr, toPtr);
      this.assertOk(result, `rename "${from}" -> "${to}"`);
    } finally {
      this.exports.free(fromPtr);
      this.exports.free(toPtr);
    }
  }

  toImage(): Uint8Array {
    const size = this.ensureStorageSize();
    if (size === 0) {
      return new Uint8Array();
    }
    const ptr = this.alloc(size);
    try {
      const copied = this.exports.lfsjs_export_image(ptr, size);
      this.assertOk(copied, "export filesystem image");
      return this.heapU8.slice(ptr, ptr + size);
    } finally {
      this.exports.free(ptr);
    }
  }

  readFile(path: string): Uint8Array {
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const size = this.exports.lfsjs_file_size(pathPtr);
      this.assertOk(size, `stat file "${normalizedPath}"`);
      if (size === 0) {
        return new Uint8Array();
      }

      const dataPtr = this.alloc(size);
      try {
        const read = this.exports.lfsjs_read_file(pathPtr, dataPtr, size);
        this.assertOk(read, `read file "${normalizedPath}"`);
        return this.heapU8.slice(dataPtr, dataPtr + size);
      } finally {
        this.exports.free(dataPtr);
      }
    } finally {
      this.exports.free(pathPtr);
    }
  }

  private refreshHeap(): void {
    if (this.heapU8.buffer !== this.exports.memory.buffer) {
      this.heapU8 = new Uint8Array(this.exports.memory.buffer);
    }
  }

  refreshStorageSize(): void {
    const size = this.exports.lfsjs_storage_size();
    if (size > 0) {
      this.storageSize = size;
    }
  }

  private ensureStorageSize(): number {
    if (this.storageSize === 0) {
      this.refreshStorageSize();
    }
    return this.storageSize;
  }

  private alloc(size: number): number {
    if (size <= 0) {
      return 0;
    }
    const ptr = this.exports.malloc(size);
    if (!ptr) {
      throw new LittleFSError("Failed to allocate WebAssembly memory", LFS_ERR_NOSPC);
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
      throw new LittleFSError(`Unable to ${action}`, code);
    }
  }
}

async function instantiateLittleFSModule(input: string | URL): Promise<LittleFSExports> {
  const source = resolveWasmURL(input);
  console.info("[littlefs-wasm] Fetching wasm from", source.href);
  const wasmContext: WasmContext = { memory: null };
  const imports: WebAssembly.Imports = createDefaultImports(wasmContext);
  let response = await fetch(source);
  if (!response.ok) {
    throw new Error(`Unable to fetch LittleFS wasm from ${response.url}`);
  }
  console.info("[littlefs-wasm] Fetch complete, status", response.status);

  if ("instantiateStreaming" in WebAssembly && typeof WebAssembly.instantiateStreaming === "function") {
    try {
      console.info("[littlefs-wasm] Attempting instantiateStreaming");
      const streaming = await WebAssembly.instantiateStreaming(response, imports);
      wasmContext.memory = getExportedMemory(streaming.instance.exports);
      console.info("[littlefs-wasm] instantiateStreaming succeeded");
      return streaming.instance.exports as unknown as LittleFSExports;
    } catch (error) {
      console.warn("Unable to instantiate LittleFS wasm via streaming, retrying with arrayBuffer()", error);
      response = await fetch(source);
      if (!response.ok) {
        throw new Error(`Unable to fetch LittleFS wasm from ${response.url}`);
      }
      console.info("[littlefs-wasm] Fallback fetch complete, status", response.status);
    }
  }

  console.info("[littlefs-wasm] Instantiating from ArrayBuffer fallback");
  const bytes = await response.arrayBuffer();
  const instance = await WebAssembly.instantiate(bytes, imports);
  wasmContext.memory = getExportedMemory(instance.instance.exports);
  console.info("[littlefs-wasm] instantiate(bytes) succeeded");
  return instance.instance.exports as unknown as LittleFSExports;
}

function parseListPayload(payload: string): LittleFSEntry[] {
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
        type: rawType === "d" ? "dir" : "file"
      };
    });
}

function normalizePath(input: string): string {
  const value = input.trim().replace(/\\/g, "/");
  const withoutRoot = value.replace(/^\/+/, "");
  if (!withoutRoot) {
    throw new Error("Path must point to a file (e.g. \"docs/readme.txt\")");
  }
  const collapsed = withoutRoot.replace(/\/{2,}/g, "/");
  const parts = collapsed.split("/").filter(Boolean);
  if (parts.some((segment) => segment === "..")) {
    throw new Error("Path must not contain '..'");
  }
  const clean = parts.join("/");
  return clean;
}

function normalizePathOptional(input: string): string {
  const trimmed = input.trim();
  if (trimmed === "" || trimmed === "/") {
    return "/";
  }
  return `/${normalizePath(trimmed)}`;
}

function asUint8Array(source: FileSource, encoder: TextEncoder): Uint8Array {
  if (typeof source === "string") {
    return encoder.encode(source);
  }
  if (source instanceof Uint8Array) {
    return source;
  }
  if (source instanceof ArrayBuffer) {
    return new Uint8Array(source);
  }
  throw new Error("Unsupported file payload type");
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
      emscripten_notify_memory_growth: noop
    },
    wasi_snapshot_preview1: {
      fd_close: ok,
      fd_seek: ok,
      fd_write: (fd: number, iov: number, iovcnt: number, pnum: number) =>
        handleFdWrite(context, fd, iov, iovcnt, pnum)
    }
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
      console.info(`[littlefs-wasm::fd_write fd=${fd}] ${text}`);
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

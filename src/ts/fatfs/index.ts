import type { FileSource, BinarySource } from "../shared/types";

const DEFAULT_BLOCK_SIZE = 512;
const DEFAULT_BLOCK_COUNT = 1024;
const INITIAL_LIST_BUFFER = 4096;
const FATFS_ERR_NOT_ENOUGH_CORE = -17;

export interface FatFSEntry {
  path: string;
  size: number;
}

export interface FatFSOptions {
  blockSize?: number;
  blockCount?: number;
  wasmURL?: string | URL;
  formatOnInit?: boolean;
}

export interface FatFS {
  format(): void;
  list(): FatFSEntry[];
  readFile(path: string): Uint8Array;
  writeFile(path: string, data: FileSource): void;
  deleteFile(path: string): void;
  toImage(): Uint8Array;
}

interface FatFSExports {
  memory: WebAssembly.Memory;
  fatfsjs_init(blockSize: number, blockCount: number): number;
  fatfsjs_init_from_image(blockSize: number, blockCount: number, imagePtr: number, imageLen: number): number;
  fatfsjs_format(): number;
  fatfsjs_list(bufferPtr: number, bufferLen: number): number;
  fatfsjs_file_size(pathPtr: number): number;
  fatfsjs_read_file(pathPtr: number, bufferPtr: number, bufferLen: number): number;
  fatfsjs_write_file(pathPtr: number, dataPtr: number, dataLen: number): number;
  fatfsjs_delete_file(pathPtr: number): number;
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

export async function createFatFS(options: FatFSOptions = {}): Promise<FatFS> {
  console.info("[fatfs-wasm] createFatFS() starting", options);
  const wasmURL = options.wasmURL ?? new URL("./fatfs.wasm", import.meta.url);
  const exports = await instantiateFatFSModule(wasmURL);

  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  const blockCount = options.blockCount ?? DEFAULT_BLOCK_COUNT;

  const initResult = exports.fatfsjs_init(blockSize, blockCount);
  console.info("[fatfs-wasm] fatfsjs_init returned", initResult);
  if (initResult < 0) {
    throw new FatFSError("Failed to initialize FatFS", initResult);
  }

  if (options.formatOnInit) {
    const formatResult = exports.fatfsjs_format();
    console.info("[fatfs-wasm] fatfsjs_format returned", formatResult);
    if (formatResult < 0) {
      throw new FatFSError("Failed to format FatFS volume", formatResult);
    }
  }

  const client = new FatFSClient(exports);
  console.info("[fatfs-wasm] Filesystem initialized");
  return client;
}

export async function createFatFSFromImage(image: BinarySource, options: FatFSOptions = {}): Promise<FatFS> {
  console.info("[fatfs-wasm] createFatFSFromImage() starting");
  const wasmURL = options.wasmURL ?? new URL("./fatfs.wasm", import.meta.url);
  const exports = await instantiateFatFSModule(wasmURL);
  const bytes = asBinaryUint8Array(image);

  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  if (blockSize === 0) {
    throw new Error("blockSize must be a positive integer");
  }

  const inferredBlocks = bytes.length / blockSize;
  const blockCount = options.blockCount ?? inferredBlocks;
  if (blockCount * blockSize !== bytes.length) {
    throw new Error("Image size must equal blockSize * blockCount");
  }

  const heap = new Uint8Array(exports.memory.buffer);
  const ptr = exports.malloc(bytes.length || 1);
  if (!ptr) {
    throw new FatFSError("Failed to allocate WebAssembly memory", FATFS_ERR_NOT_ENOUGH_CORE);
  }

  try {
    heap.set(bytes, ptr);
    const initResult = exports.fatfsjs_init_from_image(blockSize, blockCount, ptr, bytes.length);
    if (initResult < 0) {
      throw new FatFSError("Failed to initialize FatFS from image", initResult);
    }
  } finally {
    exports.free(ptr);
  }

  const client = new FatFSClient(exports);
  console.info("[fatfs-wasm] Filesystem initialized from image");
  return client;
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

  format(): void {
    const result = this.exports.fatfsjs_format();
    this.assertOk(result, "format filesystem");
  }

  list(): FatFSEntry[] {
    let capacity = this.listBufferSize;
    while (true) {
      const ptr = this.alloc(capacity);
      try {
        const used = this.exports.fatfsjs_list(ptr, capacity);
        if (used === FATFS_ERR_NOT_ENOUGH_CORE) {
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

  readFile(path: string): Uint8Array {
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const size = this.exports.fatfsjs_file_size(pathPtr);
      this.assertOk(size, `stat file "${normalizedPath}"`);
      if (size === 0) {
        return new Uint8Array();
      }
      const dataPtr = this.alloc(size);
      try {
        const read = this.exports.fatfsjs_read_file(pathPtr, dataPtr, size);
        this.assertOk(read, `read file "${normalizedPath}"`);
        return this.heapU8.slice(dataPtr, dataPtr + size);
      } finally {
        this.exports.free(dataPtr);
      }
    } finally {
      this.exports.free(pathPtr);
    }
  }

  writeFile(path: string, data: FileSource): void {
    const normalizedPath = normalizePath(path);
    const payload = asUint8Array(data, this.encoder);
    const pathPtr = this.allocString(normalizedPath);
    const dataPtr = this.alloc(payload.length);
    try {
      if (payload.length > 0 && dataPtr) {
        this.heapU8.set(payload, dataPtr);
      }
      const result = this.exports.fatfsjs_write_file(pathPtr, payload.length > 0 ? dataPtr : 0, payload.length);
      this.assertOk(result, `write file "${normalizedPath}"`);
    } finally {
      if (dataPtr) {
        this.exports.free(dataPtr);
      }
      this.exports.free(pathPtr);
    }
  }

  deleteFile(path: string): void {
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const result = this.exports.fatfsjs_delete_file(pathPtr);
      this.assertOk(result, `delete file "${normalizedPath}"`);
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
      throw new FatFSError("Failed to allocate WebAssembly memory", FATFS_ERR_NOT_ENOUGH_CORE);
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
    throw new Error(`Unable to fetch FatFS wasm from ${response.url}`);
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
      console.warn("Unable to instantiate FatFS wasm via streaming, retrying with arrayBuffer()", error);
      response = await fetch(source);
      if (!response.ok) {
        throw new Error(`Unable to fetch FatFS wasm from ${response.url}`);
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
      const [rawPath, rawSize] = line.split("\t");
      return {
        path: rawPath ?? "",
        size: Number(rawSize ?? "0") || 0
      };
    });
}

function normalizePath(input: string): string {
  const value = input.trim().replace(/\\/g, "/");
  const collapsed = value.replace(/\/{2,}/g, "/");
  const withoutRoot = collapsed.replace(/^\/+/, "");
  if (!withoutRoot) {
    throw new Error('Path must point to a file (e.g. "/docs/readme.txt")');
  }
  const withoutTrailing = withoutRoot.replace(/\/+$/g, "");
  return "/" + withoutTrailing;
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

function handleFdWrite(context: WasmContext, fd: number, iov: number, iovcnt: number, pnum: number): number {
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

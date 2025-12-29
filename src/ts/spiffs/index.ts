import type { FileSource, BinarySource } from "../shared/types";

const DEFAULT_PAGE_SIZE = 256;
const DEFAULT_BLOCK_SIZE = 4096;
const DEFAULT_BLOCK_COUNT = 256;
const DEFAULT_FD_COUNT = 16;
const DEFAULT_CACHE_PAGES = 64;
const INITIAL_LIST_BUFFER = 4096;
const SPIFFS_CAN_FIT_SUCCESS = 1;

export interface SpiffsEntry {
  name: string;
  size: number;
  type: "file" | "dir";
}

export interface SpiffsUsage {
  capacityBytes: number;
  usedBytes: number;
  freeBytes: number;
}

export interface SpiffsOptions {
  wasmURL?: string | URL;
  pageSize?: number;
  blockSize?: number;
  blockCount?: number;
  fdCount?: number;
  cachePages?: number;
  formatOnInit?: boolean;
}

export interface Spiffs {
  list(): Promise<SpiffsEntry[]>;
  read(name: string): Promise<Uint8Array>;
  write(name: string, data: FileSource): Promise<void>;
  remove(name: string): Promise<void>;
  format(): Promise<void>;
  toImage(): Promise<Uint8Array>;
  getUsage(): Promise<SpiffsUsage>;
  canFit?(name: string, dataLength: number): boolean;
}

interface SpiffsExports {
  memory: WebAssembly.Memory;
  spiffsjs_init(
    pageSize: number,
    blockSize: number,
    blockCount: number,
    fdCount: number,
    cachePages: number
  ): number;
  spiffsjs_init_from_image(
    pageSize: number,
    blockSize: number,
    blockCount: number,
    fdCount: number,
    cachePages: number,
    imagePtr: number,
    imageLen: number
  ): number;
  spiffsjs_format(): number;
  spiffsjs_list(bufferPtr: number, bufferLen: number): number;
  spiffsjs_file_size(pathPtr: number): number;
  spiffsjs_read_file(
    pathPtr: number,
    bufferPtr: number,
    bufferLen: number
  ): number;
  spiffsjs_write_file(
    pathPtr: number,
    dataPtr: number,
    dataLen: number
  ): number;
  spiffsjs_remove_file(pathPtr: number): number;
  spiffsjs_storage_size(): number;
  spiffsjs_export_image(bufferPtr: number, bufferLen: number): number;
  spiffsjs_get_usage(usagePtr: number): number;
  spiffsjs_can_fit(pathPtr: number, length: number): number;
  malloc(size: number): number;
  free(ptr: number): void;
}

export class SpiffsError extends Error {
  readonly code: number;
  constructor(message: string, code: number) {
    super(message);
    this.code = code;
    this.name = "SpiffsError";
  }
}

export async function createSpiffs(options: SpiffsOptions = {}): Promise<Spiffs> {
  console.info("[spiffs-wasm] createSpiffs() starting", options);
  const wasmURL = options.wasmURL ?? new URL("./spiffs.wasm", import.meta.url);
  const exports = await instantiateSpiffsModule(wasmURL);

  const pageSize = options.pageSize ?? DEFAULT_PAGE_SIZE;
  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  const blockCount = options.blockCount ?? DEFAULT_BLOCK_COUNT;
  validateSpiffsLayout(pageSize, blockSize, blockCount);

  const fdCount = options.fdCount ?? DEFAULT_FD_COUNT;
  const cachePages = options.cachePages ?? DEFAULT_CACHE_PAGES;

  const initResult = exports.spiffsjs_init(
    pageSize,
    blockSize,
    blockCount,
    fdCount,
    cachePages
  );
  console.info("[spiffs-wasm] spiffsjs_init returned", initResult);
  if (initResult < 0) {
    throw new SpiffsError("Failed to initialize SPIFFS", initResult);
  }

  if (options.formatOnInit) {
    const formatResult = exports.spiffsjs_format();
    console.info("[spiffs-wasm] spiffsjs_format returned", formatResult);
    if (formatResult < 0) {
      throw new SpiffsError("Failed to format SPIFFS volume", formatResult);
    }
  }

  const client = new SpiffsClient(exports);
  console.info("[spiffs-wasm] Filesystem initialized");
  return client;
}

export async function createSpiffsFromImage(
  image: BinarySource,
  options: SpiffsOptions = {}
): Promise<Spiffs> {
  console.info("[spiffs-wasm] createSpiffsFromImage() starting");
  const wasmURL = options.wasmURL ?? new URL("./spiffs.wasm", import.meta.url);
  const exports = await instantiateSpiffsModule(wasmURL);
  const bytes = asBinaryUint8Array(image);

  const pageSize = options.pageSize ?? DEFAULT_PAGE_SIZE;
  const blockSize = options.blockSize ?? DEFAULT_BLOCK_SIZE;
  const blockCount =
    options.blockCount ??
    Math.max(1, Math.floor(bytes.length / Math.max(blockSize, 1)));
  validateSpiffsLayout(pageSize, blockSize, blockCount);

  if (blockCount * blockSize !== bytes.length) {
    throw new Error("Image size must equal blockSize * blockCount");
  }

  const fdCount = options.fdCount ?? DEFAULT_FD_COUNT;
  const cachePages = options.cachePages ?? DEFAULT_CACHE_PAGES;

  const heap = new Uint8Array(exports.memory.buffer);
  const ptr = exports.malloc(bytes.length || 1);
  if (!ptr) {
    throw new SpiffsError(
      "Failed to allocate WebAssembly memory",
      -1 // mimic not enough core
    );
  }

  try {
    heap.set(bytes, ptr);
    const initResult = exports.spiffsjs_init_from_image(
      pageSize,
      blockSize,
      blockCount,
      fdCount,
      cachePages,
      ptr,
      bytes.length
    );
    if (initResult < 0) {
      throw new SpiffsError("Failed to initialize SPIFFS from image", initResult);
    }
  } finally {
    exports.free(ptr);
  }

  const client = new SpiffsClient(exports);
  console.info("[spiffs-wasm] Filesystem initialized from image");
  return client;
}

class SpiffsClient implements Spiffs {
  private readonly exports: SpiffsExports;
  private heapU8: Uint8Array;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();
  private listBufferSize = INITIAL_LIST_BUFFER;

  constructor(exports: SpiffsExports) {
    this.exports = exports;
    this.heapU8 = new Uint8Array(this.exports.memory.buffer);
  }

  async list(): Promise<SpiffsEntry[]> {
    let capacity = this.listBufferSize;
    while (true) {
      const ptr = this.alloc(capacity);
      try {
        const used = this.exports.spiffsjs_list(ptr, capacity);
        if (used < 0) {
          this.assertOk(used, "list files");
        }
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

  async read(name: string): Promise<Uint8Array> {
    const normalized = normalizePath(name);
    const pathPtr = this.allocString(normalized);
    try {
      const size = this.exports.spiffsjs_file_size(pathPtr);
      this.assertOk(size, `stat file "${normalized}"`);
      if (size === 0) {
        return new Uint8Array();
      }
      const dataPtr = this.alloc(size);
      try {
        const read = this.exports.spiffsjs_read_file(pathPtr, dataPtr, size);
        this.assertOk(read, `read file "${normalized}"`);
        return this.heapU8.slice(dataPtr, dataPtr + size);
      } finally {
        this.exports.free(dataPtr);
      }
    } finally {
      this.exports.free(pathPtr);
    }
  }

  async write(name: string, data: FileSource): Promise<void> {
    const normalized = normalizePath(name);
    const payload = asUint8Array(data, this.encoder);
    const pathPtr = this.allocString(normalized);
    const dataPtr = payload.length ? this.alloc(payload.length) : 0;
    try {
      if (payload.length > 0) {
        this.heapU8.set(payload, dataPtr);
      }
      const result = this.exports.spiffsjs_write_file(
        pathPtr,
        dataPtr,
        payload.length
      );
      this.assertOk(result, `write file "${normalized}"`);
    } finally {
      if (dataPtr) {
        this.exports.free(dataPtr);
      }
      this.exports.free(pathPtr);
    }
  }

  async remove(name: string): Promise<void> {
    const normalized = normalizePath(name);
    const pathPtr = this.allocString(normalized);
    try {
      const result = this.exports.spiffsjs_remove_file(pathPtr);
      this.assertOk(result, `delete file "${normalized}"`);
    } finally {
      this.exports.free(pathPtr);
    }
  }

  async format(): Promise<void> {
    const result = this.exports.spiffsjs_format();
    this.assertOk(result, "format filesystem");
  }

  async toImage(): Promise<Uint8Array> {
    const size = this.exports.spiffsjs_storage_size();
    if (size === 0) {
      return new Uint8Array();
    }
    const ptr = this.alloc(size);
    try {
      const copied = this.exports.spiffsjs_export_image(ptr, size);
      this.assertOk(copied, "export filesystem image");
      return this.heapU8.slice(ptr, ptr + size);
    } finally {
      this.exports.free(ptr);
    }
  }

  async getUsage(): Promise<SpiffsUsage> {
    const ptr = this.alloc(12);
    try {
      const result = this.exports.spiffsjs_get_usage(ptr);
      this.assertOk(result, "get usage");
      const view = new DataView(this.heapU8.buffer, ptr, 12);
      return {
        capacityBytes: view.getUint32(0, true),
        usedBytes: view.getUint32(4, true),
        freeBytes: view.getUint32(8, true),
      };
    } finally {
      this.exports.free(ptr);
    }
  }

  canFit(name: string, dataLength: number): boolean {
    const normalized = normalizePath(name);
    const pathPtr = this.allocString(normalized);
    try {
      const result = this.exports.spiffsjs_can_fit(pathPtr, dataLength);
      if (result >= 0) {
        return result === SPIFFS_CAN_FIT_SUCCESS;
      }
      this.assertOk(result, `check space for "${normalized}"`);
      return false;
    } finally {
      this.exports.free(pathPtr);
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
      throw new SpiffsError("Failed to allocate WebAssembly memory", -1);
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
      throw new SpiffsError(`Unable to ${action}`, code);
    }
  }
}

async function instantiateSpiffsModule(input: string | URL): Promise<SpiffsExports> {
  const source = resolveWasmURL(input);
  console.info("[spiffs-wasm] Fetching wasm from", source.href);
  const wasmContext: WasmContext = { memory: null };
  const imports: WebAssembly.Imports = createDefaultImports(wasmContext);

  let response = await fetch(source);
  if (!response.ok) {
    throw new Error(`Unable to fetch SPIFFS wasm from ${response.url}`);
  }
  console.info("[spiffs-wasm] Fetch complete, status", response.status);

  if ("instantiateStreaming" in WebAssembly && typeof WebAssembly.instantiateStreaming === "function") {
    try {
      console.info("[spiffs-wasm] Attempting instantiateStreaming");
      const streaming = await WebAssembly.instantiateStreaming(response, imports);
      wasmContext.memory = getExportedMemory(streaming.instance.exports);
      console.info("[spiffs-wasm] instantiateStreaming succeeded");
      return streaming.instance.exports as unknown as SpiffsExports;
    } catch (error) {
      console.warn(
        "Unable to instantiate SPIFFS wasm via streaming, retrying with arrayBuffer()",
        error
      );
      response = await fetch(source);
      if (!response.ok) {
        throw new Error(`Unable to fetch SPIFFS wasm from ${response.url}`);
      }
      console.info("[spiffs-wasm] Fallback fetch complete, status", response.status);
    }
  }

  console.info("[spiffs-wasm] Instantiating from ArrayBuffer fallback");
  const bytes = await response.arrayBuffer();
  const instance = await WebAssembly.instantiate(bytes, imports);
  wasmContext.memory = getExportedMemory(instance.instance.exports);
  console.info("[spiffs-wasm] instantiate(bytes) succeeded");
  return instance.instance.exports as unknown as SpiffsExports;
}

function parseListPayload(payload: string): SpiffsEntry[] {
  if (!payload) {
    return [];
  }
  return payload
    .split("\n")
    .filter((line) => line.length > 0)
    .map((line) => {
      const [rawName, rawType, rawSize] = line.split("\t");
      return {
        name: rawName ?? "",
        type: rawType === "dir" ? "dir" : "file",
        size: Number(rawSize ?? "0") || 0,
      };
    });
}

function normalizePath(input: string): string {
  const value = input.trim().replace(/\\/g, "/");
  const withoutRoot = value.replace(/^\/+/, "");
  if (!withoutRoot) {
    throw new Error('Path must point to a file (e.g. "docs/readme.txt")');
  }
  const collapsed = withoutRoot.replace(/\/{2,}/g, "/");
  return collapsed.endsWith("/") ? collapsed.slice(0, -1) : collapsed;
}

function validateSpiffsLayout(
  pageSize: number,
  blockSize: number,
  blockCount: number
): void {
  if (!Number.isFinite(pageSize) || pageSize <= 0) {
    throw new Error("pageSize must be a positive integer");
  }
  if (!Number.isFinite(blockSize) || blockSize <= 0) {
    throw new Error("blockSize must be a positive integer");
  }
  if (blockSize % pageSize !== 0) {
    throw new Error("blockSize must be a multiple of pageSize");
  }
  if (!Number.isFinite(blockCount) || blockCount <= 0) {
    throw new Error("blockCount must be a positive integer");
  }
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
      console.info(`[spiffs-wasm::fd_write fd=${fd}] ${text}`);
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

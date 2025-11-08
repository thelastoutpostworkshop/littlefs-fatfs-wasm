const DEFAULT_BLOCK_SIZE = 512;
const DEFAULT_BLOCK_COUNT = 512;
const DEFAULT_LOOKAHEAD_SIZE = 32;
const INITIAL_LIST_BUFFER = 4096;
const LFS_ERR_NOSPC = -28;

export type FileSource = string | ArrayBuffer | Uint8Array;

export interface LittleFSEntry {
  path: string;
  size: number;
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
  list(): LittleFSEntry[];
  addFile(path: string, data: FileSource): void;
  deleteFile(path: string): void;
}

interface LittleFSExports {
  memory: WebAssembly.Memory;
  lfsjs_init(blockSize: number, blockCount: number, lookaheadSize: number): number;
  lfsjs_format(): number;
  lfsjs_list(bufferPtr: number, bufferLen: number): number;
  lfsjs_add_file(pathPtr: number, dataPtr: number, dataLen: number): number;
  lfsjs_delete_file(pathPtr: number): number;
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
  return new LittleFSClient(exports);
}

class LittleFSClient implements LittleFS {
  private readonly exports: LittleFSExports;
  private heapU8: Uint8Array;
  private readonly encoder = new TextEncoder();
  private readonly decoder = new TextDecoder();
  private listBufferSize = INITIAL_LIST_BUFFER;

  constructor(exports: LittleFSExports) {
    this.exports = exports;
    this.heapU8 = new Uint8Array(this.exports.memory.buffer);
  }

  format(): void {
    const result = this.exports.lfsjs_format();
    this.assertOk(result, "format filesystem");
  }

  list(): LittleFSEntry[] {
    let capacity = this.listBufferSize;

    while (true) {
      const ptr = this.alloc(capacity);
      try {
        const used = this.exports.lfsjs_list(ptr, capacity);
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

  deleteFile(path: string): void {
    const normalizedPath = normalizePath(path);
    const pathPtr = this.allocString(normalizedPath);
    try {
      const result = this.exports.lfsjs_delete_file(pathPtr);
      this.assertOk(result, `delete file "${normalizedPath}"`);
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
      const [rawPath, rawSize] = line.split("\t");
      return {
        path: rawPath ?? "",
        size: Number(rawSize ?? "0") || 0,
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
  const clean = collapsed.endsWith("/") ? collapsed.slice(0, -1) : collapsed;
  return clean;
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

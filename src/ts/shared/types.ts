export type FileSource = string | ArrayBuffer | Uint8Array;
export type BinarySource = ArrayBuffer | Uint8Array;

export interface FileSystemUsage {
  capacityBytes: number;
  usedBytes: number;
  freeBytes: number;
}

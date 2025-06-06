declare namespace ArtifactBundler {
  type MemorySnapshotResult = {
    snapshot: Uint8Array;
    importedModulesList: string[];
  };

  const hasMemorySnapshot: () => boolean;
  const isEwValidating: () => boolean;
  const readMemorySnapshot: (
    offset: number,
    buf: Uint32Array | Uint8Array
  ) => void;
  const getMemorySnapshotSize: () => number;
  const disposeMemorySnapshot: () => void;
  const storeMemorySnapshot: (snap: MemorySnapshotResult) => void;
  const getPackage: (path: string) => Reader | null;
}

export default ArtifactBundler;

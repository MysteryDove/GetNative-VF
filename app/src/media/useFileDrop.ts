import { useEffect, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";

/**
 * Window-level file drag/drop hook (Tauri webview drag events). Returns true
 * while a drag hovers so pages can show a drop overlay. Only one page is
 * mounted at a time, so per-page subscription is safe.
 */
export function useFileDrop(onPaths: (paths: string[]) => void): boolean {
  const [dropActive, setDropActive] = useState(false);
  useEffect(() => {
    let disposed = false;
    let unlisten: (() => void) | undefined;
    getCurrentWebview()
      .onDragDropEvent((event) => {
        if (event.payload.type === "enter" || event.payload.type === "over") {
          setDropActive(true);
        } else if (event.payload.type === "leave") {
          setDropActive(false);
        } else if (event.payload.type === "drop") {
          setDropActive(false);
          onPaths(event.payload.paths);
        }
      })
      .then((stop) => {
        if (disposed) stop();
        else unlisten = stop;
      })
      .catch(() => undefined);
    return () => {
      disposed = true;
      unlisten?.();
    };
  }, [onPaths]);
  return dropActive;
}

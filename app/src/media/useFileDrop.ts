import { useEffect, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";

/**
 * Window-level file drag/drop hook (Tauri webview drag events). Returns true
 * while a drag hovers so pages can show a drop overlay. Keep-alive routes
 * pass enabled=false so only the visible page owns the webview subscription.
 */
export function useFileDrop(onPaths: (paths: string[]) => void, enabled = true): boolean {
  const [dropActive, setDropActive] = useState(false);
  useEffect(() => {
    if (!enabled) {
      setDropActive(false);
      return;
    }
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
  }, [enabled, onPaths]);
  return dropActive;
}

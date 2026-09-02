import { useEffect, useRef, useState } from "react";
import { getCurrentWebview } from "@tauri-apps/api/webview";

/** Tauri's UnlistenFn may return a promise that rejects if the listener is already gone. */
function releaseUnlisten(stop: (() => void | Promise<void>) | undefined): void {
  if (!stop) return;
  try {
    void Promise.resolve(stop()).catch(() => undefined);
  } catch {
    // Plugin threw synchronously (listeners[eventId] already missing).
  }
}

/**
 * Window-level file drag/drop hook (Tauri webview drag events). Returns true
 * while a drag hovers so pages can show a drop overlay. Keep-alive routes
 * pass enabled=false so only the visible page owns the webview subscription.
 */
export function useFileDrop(onPaths: (paths: string[]) => void, enabled = true): boolean {
  const [dropActive, setDropActive] = useState(false);
  const onPathsRef = useRef(onPaths);
  onPathsRef.current = onPaths;

  useEffect(() => {
    if (!enabled) {
      setDropActive(false);
      return;
    }
    let disposed = false;
    let unlisten: (() => void | Promise<void>) | undefined;
    getCurrentWebview()
      .onDragDropEvent((event) => {
        if (event.payload.type === "enter" || event.payload.type === "over") {
          setDropActive(true);
        } else if (event.payload.type === "leave") {
          setDropActive(false);
        } else if (event.payload.type === "drop") {
          setDropActive(false);
          onPathsRef.current(event.payload.paths);
        }
      })
      .then((stop) => {
        if (disposed) releaseUnlisten(stop);
        else unlisten = stop;
      })
      .catch(() => undefined);
    return () => {
      disposed = true;
      releaseUnlisten(unlisten);
    };
  }, [enabled]);
  return dropActive;
}

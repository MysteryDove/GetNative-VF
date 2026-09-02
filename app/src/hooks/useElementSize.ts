import { useEffect, useRef, useState } from "react";

/** Wait until a resize burst (nav column tween, window drag) stops. */
const RESIZE_SETTLE_MS = 48;

function roundedSize(width: number, height: number): { width: number; height: number } {
  return { width: Math.round(width), height: Math.round(height) };
}

function sameSize(
  a: { width: number; height: number },
  b: { width: number; height: number },
): boolean {
  return Math.round(a.width) === Math.round(b.width) && Math.round(a.height) === Math.round(b.height);
}

/** Content-box size of the attached element, tracked with a ResizeObserver. */
export function useElementSize<T extends HTMLElement = HTMLDivElement>() {
  const ref = useRef<T | null>(null);
  const [size, setSize] = useState({ width: 0, height: 0 });
  const sizeRef = useRef(size);
  sizeRef.current = size;

  useEffect(() => {
    const element = ref.current;
    if (!element) return;

    let settleTimer = 0;
    let pending = sizeRef.current;

    const commit = (next: { width: number; height: number }) => {
      const resolved = roundedSize(next.width, next.height);
      if (sameSize(sizeRef.current, resolved)) return;
      sizeRef.current = resolved;
      setSize(resolved);
    };

    const observer = new ResizeObserver((entries) => {
      const box = entries[0]?.contentRect;
      if (!box) return;
      pending = { width: box.width, height: box.height };
      // First layout: paint the plot immediately. Later bursts (grid tween)
      // settle so SVG/canvas does not realloc on every interpolated width.
      if (sizeRef.current.width === 0 && sizeRef.current.height === 0) {
        commit(pending);
        return;
      }
      window.clearTimeout(settleTimer);
      settleTimer = window.setTimeout(() => commit(pending), RESIZE_SETTLE_MS);
    });
    observer.observe(element);
    return () => {
      observer.disconnect();
      window.clearTimeout(settleTimer);
    };
  }, []);

  return [ref, size] as const;
}

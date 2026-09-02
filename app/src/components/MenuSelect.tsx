import { useEffect, useLayoutEffect, useRef, useState, type JSX } from "react";
import { createPortal } from "react-dom";
import { ChevronDown } from "lucide-react";

export type MenuSelectOption = {
  value: string;
  label: string;
};

/**
 * Single-select menu with the same open/close motion as RecipePicker.
 * The list is portaled so it is not clipped by pane overflow.
 */
export function MenuSelect({
  value,
  options,
  onChange,
  disabled,
  ariaLabel,
  title,
}: {
  value: string;
  options: MenuSelectOption[];
  onChange: (value: string) => void;
  disabled?: boolean;
  ariaLabel: string;
  title?: string;
}): JSX.Element {
  const [open, setOpen] = useState(false);
  const [menuBox, setMenuBox] = useState({
    top: 0,
    bottom: null as number | null,
    left: 0,
    width: 0,
    maxHeight: 240,
  });
  const rootRef = useRef<HTMLDivElement>(null);
  const menuRef = useRef<HTMLUListElement>(null);
  const selected = options.find((option) => option.value === value);
  const label = selected?.label ?? value;

  function placeMenu() {
    const trigger = rootRef.current?.querySelector("button");
    if (!trigger) return;
    const rect = trigger.getBoundingClientRect();
    const gap = 4;
    const below = window.innerHeight - rect.bottom - 8;
    const above = rect.top - 8;
    const openUp = below < 120 && above > below;
    const maxHeight = Math.max(96, Math.min(240, openUp ? above : below));
    setMenuBox({
      top: openUp ? 0 : rect.bottom + gap,
      bottom: openUp ? window.innerHeight - rect.top + gap : null,
      left: rect.left,
      width: rect.width,
      maxHeight,
    });
  }

  useLayoutEffect(() => {
    if (!open) return;
    placeMenu();
  }, [open, options.length, label]);

  useEffect(() => {
    if (!open) return;
    const onPointer = (event: PointerEvent) => {
      const target = event.target as Node;
      if (rootRef.current?.contains(target) || menuRef.current?.contains(target)) return;
      setOpen(false);
    };
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpen(false);
    };
    const onReposition = () => setOpen(false);
    document.addEventListener("pointerdown", onPointer);
    document.addEventListener("keydown", onKey);
    window.addEventListener("resize", onReposition);
    document.addEventListener("scroll", onReposition, true);
    return () => {
      document.removeEventListener("pointerdown", onPointer);
      document.removeEventListener("keydown", onKey);
      window.removeEventListener("resize", onReposition);
      document.removeEventListener("scroll", onReposition, true);
    };
  }, [open]);

  return (
    <div className={`menu-select${open ? " open" : ""}`} ref={rootRef}>
      <button
        type="button"
        className="menu-select-trigger"
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={ariaLabel}
        title={title ?? label}
        disabled={disabled}
        onClick={() => {
          if (disabled) return;
          setOpen((current) => !current);
        }}
      >
        <span>{label || "—"}</span>
        <ChevronDown size={14} />
      </button>
      {createPortal(
        <ul
          ref={menuRef}
          className={`menu-select-menu${open ? " open" : ""}${menuBox.bottom != null ? " open-up" : ""}`}
          role="listbox"
          aria-label={ariaLabel}
          aria-hidden={!open}
          inert={!open}
          style={{
            top: menuBox.bottom == null ? menuBox.top : "auto",
            bottom: menuBox.bottom ?? "auto",
            left: menuBox.left,
            width: menuBox.width,
            maxHeight: menuBox.maxHeight,
          }}
        >
          {options.map((option) => (
            <li key={option.value} className={option.value === value ? "selected" : ""}>
              <button
                type="button"
                role="option"
                aria-selected={option.value === value}
                className="menu-select-option"
                tabIndex={open ? 0 : -1}
                onClick={() => {
                  onChange(option.value);
                  setOpen(false);
                }}
              >
                {option.label}
              </button>
            </li>
          ))}
        </ul>,
        document.body,
      )}
    </div>
  );
}

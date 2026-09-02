import { useEffect, useRef, useState, type JSX, type MouseEvent } from "react";
import { ChevronDown, X } from "lucide-react";
import type { Translator } from "../i18n";
import type { Recipe } from "../project/types";

/**
 * Custom Recipe selector: current Recipe on the trigger, a menu of rows with
 * an optional per-row remove control. Native <select> cannot host an extra
 * clickable control inside an option.
 */
export function RecipePicker(props: {
  t: Translator;
  value: string;
  options: Recipe[];
  onChange: (id: string) => void;
  onRemove?: (id: string) => void;
  ariaLabel: string;
}): JSX.Element {
  const { t, value, options, onChange, onRemove, ariaLabel } = props;
  const [open, setOpen] = useState(false);
  const rootRef = useRef<HTMLDivElement>(null);
  const selected = options.find((recipe) => recipe.id === value);

  useEffect(() => {
    if (!open) return;
    const onPointer = (event: PointerEvent) => {
      if (!rootRef.current?.contains(event.target as Node)) setOpen(false);
    };
    const onKey = (event: KeyboardEvent) => {
      if (event.key === "Escape") setOpen(false);
    };
    document.addEventListener("pointerdown", onPointer);
    document.addEventListener("keydown", onKey);
    return () => {
      document.removeEventListener("pointerdown", onPointer);
      document.removeEventListener("keydown", onKey);
    };
  }, [open]);

  const label = selected
    ? `${selected.name} · ${t("recipe.revision", { revision: selected.revision })}`
    : "—";

  function handleRemove(event: MouseEvent<HTMLButtonElement>, recipe: Recipe) {
    event.preventDefault();
    event.stopPropagation();
    onRemove?.(recipe.id);
  }

  return (
    <div className={`recipe-picker${open ? " open" : ""}`} ref={rootRef}>
      <button
        type="button"
        className="recipe-picker-trigger"
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={ariaLabel}
        onClick={() => setOpen((current) => !current)}
      >
        <span>{label}</span>
        <ChevronDown size={14} />
      </button>
      <ul
        className="recipe-picker-menu"
        role="listbox"
        aria-label={ariaLabel}
        aria-hidden={!open}
        inert={!open}
      >
        {options.map((recipe) => {
          const optionLabel = `${recipe.name} · ${t("recipe.revision", { revision: recipe.revision })}`;
          return (
            <li key={recipe.id} className={recipe.id === value ? "selected" : ""}>
              <button
                type="button"
                role="option"
                aria-selected={recipe.id === value}
                className="recipe-picker-option"
                tabIndex={open ? 0 : -1}
                onClick={() => {
                  onChange(recipe.id);
                  setOpen(false);
                }}
              >
                {optionLabel}
              </button>
              {onRemove ? (
                <button
                  type="button"
                  className="recipe-picker-remove"
                  title={t("recipe.remove")}
                  aria-label={t("recipe.remove")}
                  tabIndex={open ? 0 : -1}
                  onPointerDown={(event) => event.stopPropagation()}
                  onMouseDown={(event) => event.stopPropagation()}
                  onClick={(event) => handleRemove(event, recipe)}
                >
                  <X size={12} />
                </button>
              ) : null}
            </li>
          );
        })}
      </ul>
    </div>
  );
}

import type { ReactNode } from "react";
import { toggleFilterValue, type FilterSelection } from "../engine/multiFilter";

/**
 * Multi-select filter row: "All" clears the selection, every other button
 * toggles its own value, so several entries can be shown together.
 */
export function MultiFilterGroup({
  label,
  allLabel,
  options,
  selected,
  onChange,
  onOptionContextMenu,
  trailing,
}: {
  label: string;
  allLabel: string;
  options: Array<{ value: string; label: ReactNode; title?: string }>;
  selected: FilterSelection;
  onChange: (selected: FilterSelection) => void;
  onOptionContextMenu?: (value: string) => void;
  trailing?: ReactNode;
}) {
  return (
    <div className="results-filter-group" role="group" aria-label={label}>
      <span className="results-filter-label">{label}</span>
      <div className="button-radio">
        <button
          type="button"
          aria-pressed={selected.length === 0}
          className={selected.length === 0 ? "active" : ""}
          onClick={() => onChange([])}
        >
          {allLabel}
        </button>
        {options.map((option) => {
          const active = selected.includes(option.value);
          return (
            <button
              key={option.value}
              type="button"
              aria-pressed={active}
              className={active ? "active" : ""}
              title={option.title}
              onClick={() => onChange(toggleFilterValue(selected, option.value))}
              onContextMenu={onOptionContextMenu ? (event) => {
                event.preventDefault();
                onOptionContextMenu(option.value);
              } : undefined}
            >
              {option.label}
            </button>
          );
        })}
      </div>
      {trailing}
    </div>
  );
}

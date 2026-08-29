import type { JSX } from "react";
import type { Translator } from "../i18n";
import type { Recipe } from "../project/types";

/**
 * Shared Recipe selector: a <select> with a disabled placeholder option while
 * no Recipe is current, options labelled `<name> · <revision>`, and a guard so
 * the placeholder's empty value never reaches `onChange`.
 */
export function RecipePicker(props: {
  t: Translator;
  value: string;
  options: Recipe[];
  onChange: (id: string) => void;
  ariaLabel: string;
}): JSX.Element {
  const { t, value, options, onChange, ariaLabel } = props;
  return (
    <select
      className="control-select"
      aria-label={ariaLabel}
      value={value}
      onChange={(event) => {
        if (event.target.value) onChange(event.target.value);
      }}
    >
      {!value ? (
        <option value="" disabled>
          —
        </option>
      ) : null}
      {options.map((recipe) => (
        <option key={recipe.id} value={recipe.id}>
          {recipe.name} · {t("recipe.revision", { revision: recipe.revision })}
        </option>
      ))}
    </select>
  );
}

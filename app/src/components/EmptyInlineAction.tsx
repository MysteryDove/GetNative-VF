import type { JSX } from "react";

/**
 * Shared empty-state inline block: a message (children) plus a
 * secondary-button that navigates the user to where they can fill the gap.
 */
export function EmptyInlineAction(props: {
  label: string;
  onClick: () => void;
  children: React.ReactNode;
}): JSX.Element {
  const { label, onClick, children } = props;
  return (
    <div className="empty-inline">
      {children}
      <button className="secondary-button" type="button" onClick={onClick}>
        {label}
      </button>
    </div>
  );
}

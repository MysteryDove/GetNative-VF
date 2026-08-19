import { X } from "lucide-react";
import { useEffect, useId, useRef } from "react";
import type { JSX } from "react";

/**
 * Shared modal shell: .modal-backdrop with click-to-close, a .modal-card
 * dialog (role="dialog" aria-modal, Escape to close), a .modal-header with
 * title and close button, and an optional .modal-actions footer.
 */
export function Modal(props: {
  onClose: () => void;
  title: string;
  /** Localized aria-label for the header close button (defaults to "Close"). */
  closeLabel?: string;
  labelledBy?: string;
  actions?: React.ReactNode;
  children: React.ReactNode;
}): JSX.Element {
  const { onClose, title, closeLabel, labelledBy, actions, children } = props;
  const generatedTitleId = useId();
  const titleId = labelledBy ?? generatedTitleId;
  const backdropPress = useRef(false);

  useEffect(() => {
    function onKeyDown(event: KeyboardEvent) {
      if (event.key === "Escape") onClose();
    }
    window.addEventListener("keydown", onKeyDown);
    return () => window.removeEventListener("keydown", onKeyDown);
  }, [onClose]);

  return (
    <div
      className="modal-backdrop"
      role="presentation"
      onPointerDown={(event) => {
        backdropPress.current = event.target === event.currentTarget;
      }}
      onPointerUp={(event) => {
        const shouldClose = backdropPress.current && event.target === event.currentTarget;
        backdropPress.current = false;
        if (shouldClose) onClose();
      }}
      onPointerCancel={() => {
        backdropPress.current = false;
      }}
    >
      <div
        className="modal-card"
        role="dialog"
        aria-modal="true"
        aria-labelledby={titleId}
        onClick={(event) => event.stopPropagation()}
      >
        <div className="modal-header">
          <h3 id={labelledBy ? undefined : generatedTitleId}>{title}</h3>
          <button className="icon-button" type="button" onClick={onClose} aria-label={closeLabel ?? "Close"}>
            <X size={16} />
          </button>
        </div>
        {children}
        {actions ? <div className="modal-actions">{actions}</div> : null}
      </div>
    </div>
  );
}

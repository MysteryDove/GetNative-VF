import type { ReactNode } from "react";
import { AlertTriangle } from "lucide-react";

export function BlockedState({
  title,
  body,
  action,
}: {
  title: string;
  body: string;
  action?: ReactNode;
}) {
  return (
    <div className="blocked-state" role="status">
      <div className="blocked-icon" aria-hidden="true">
        <AlertTriangle size={18} />
      </div>
      <div className="blocked-copy">
        <h2>{title}</h2>
        <p>{body}</p>
        {action}
      </div>
    </div>
  );
}

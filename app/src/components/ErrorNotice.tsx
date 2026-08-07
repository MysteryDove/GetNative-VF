import { AlertTriangle } from "lucide-react";
import type { Translator } from "../i18n";

export type UiError = {
  summary: string;
  detail?: string;
};

export function ErrorNotice({ error, t }: { error: UiError; t: Translator }) {
  return (
    <div className="inline-alert" role="alert">
      <div className="inline-alert-summary">
        <AlertTriangle size={15} />
        <span>{error.summary}</span>
      </div>
      {error.detail ? (
        <details>
          <summary>{t("common.details")}</summary>
          <code>{error.detail}</code>
        </details>
      ) : null}
    </div>
  );
}

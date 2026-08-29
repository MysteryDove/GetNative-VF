import { useState } from "react";

/** Shared result shape of the start*RunGroup executors. */
export type RunGroupSubmitResult =
  | { ok: true; submitted: number; failed: number }
  | { ok: false; reason: string };

/** Notice formatters supplied by the calling page (locale text lives there). */
export type RunGroupSubmitFormatters = {
  /** Success notice, e.g. t("analyze.runSubmitted", {...}). */
  submitted: (result: { submitted: number; failed: number }) => string;
  /** Failure notice, e.g. t("analyze.submitFailed", { detail }). */
  failed: (detail: string) => string;
};

/**
 * Shared submit lifecycle for run-group launches: guards against double
 * submission, clears/sets the notice, and maps ok/err results (and thrown
 * errors) to the page-supplied notice texts.
 */
export function useRunGroupSubmit(): {
  submitting: boolean;
  notice: string | null;
  submit: (
    fn: () => Promise<RunGroupSubmitResult>,
    formatters: RunGroupSubmitFormatters,
  ) => Promise<void>;
} {
  const [submitting, setSubmitting] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);

  async function submit(
    fn: () => Promise<RunGroupSubmitResult>,
    formatters: RunGroupSubmitFormatters,
  ): Promise<void> {
    if (submitting) return;
    setSubmitting(true);
    setNotice(null);
    try {
      const result = await fn();
      if (!result.ok) {
        setNotice(formatters.failed(result.reason));
        return;
      }
      setNotice(formatters.submitted(result));
    } catch (error) {
      setNotice(formatters.failed(String(error)));
    } finally {
      setSubmitting(false);
    }
  }

  return { submitting, notice, submit };
}

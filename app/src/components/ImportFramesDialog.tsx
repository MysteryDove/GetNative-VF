import { useMemo, useState } from "react";
import type { JSX } from "react";
import { ListPlus } from "lucide-react";
import type { Translator } from "../i18n";
import { parseFrameList } from "../media/frameList";
import { Modal } from "./Modal";

/**
 * Paste-a-list import for video samples (e.g. frames copied from vspreview's
 * scening list). Shows what the text resolves to before anything is added.
 */
export function ImportFramesDialog(props: {
  t: Translator;
  /** Frames in the selected stream, when known; bounds the accepted indices. */
  totalFrames: number | null;
  /** Frame indices of this source/stream that are already samples. */
  existingFrames: ReadonlySet<number>;
  onImport: (frames: number[]) => void;
  onClose: () => void;
}): JSX.Element {
  const { t, totalFrames, existingFrames, onImport, onClose } = props;
  const [text, setText] = useState("");
  const parsed = useMemo(() => parseFrameList(text, totalFrames), [text, totalFrames]);
  const fresh = useMemo(
    () => parsed.frames.filter((frame) => !existingFrames.has(frame)),
    [parsed, existingFrames],
  );
  const duplicateCount = parsed.frames.length - fresh.length;

  return (
    <Modal
      onClose={onClose}
      title={t("media.importFrames")}
      closeLabel={t("common.close")}
      actions={
        <>
          <button className="secondary-button" type="button" onClick={onClose}>
            {t("common.cancel")}
          </button>
          <button
            className="secondary-button primary-command"
            type="button"
            disabled={fresh.length === 0}
            onClick={() => onImport(fresh)}
          >
            <ListPlus size={14} />
            {t("media.importFramesConfirm", { count: String(fresh.length) })}
          </button>
        </>
      }
    >
      <p className="confirm-dialog-copy">{t("media.importFramesHint")}</p>
      <textarea
        className="frame-import-input"
        autoFocus
        spellCheck={false}
        rows={6}
        placeholder="953 1780 2031 2337"
        aria-label={t("media.importFrames")}
        value={text}
        onChange={(event) => setText(event.target.value)}
      />
      <ul className="frame-import-summary" aria-live="polite">
        {duplicateCount > 0 ? (
          <li>{t("media.importFramesDuplicates", { count: String(duplicateCount) })}</li>
        ) : null}
        {parsed.outOfRange.length > 0 ? (
          <li className="warning">
            {t("media.importFramesOutOfRange", {
              count: String(parsed.outOfRange.length),
              last: String((totalFrames ?? 1) - 1),
              frames: previewList(parsed.outOfRange),
            })}
          </li>
        ) : null}
        {parsed.invalid.length > 0 ? (
          <li className="warning">
            {t("media.importFramesInvalid", {
              count: String(parsed.invalid.length),
              tokens: previewList(parsed.invalid),
            })}
          </li>
        ) : null}
      </ul>
    </Modal>
  );
}

function previewList(items: Array<number | string>): string {
  const shown = items.slice(0, 8).join(", ");
  return items.length > 8 ? `${shown}, …` : shown;
}

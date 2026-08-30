import type { KeyboardEvent } from "react";
import {
  ChevronLeft,
  ChevronRight,
  ChevronsLeft,
  ChevronsRight,
  LoaderCircle,
  LocateFixed,
} from "lucide-react";
import type { Translator } from "../i18n";
import type { MediaFrameWindow } from "../media/service";
import type { SelectVideoFrame } from "../hooks/useMediaPreview";
import type { Sample, Source } from "../project/types";

/**
 * Video frame browser: frame/keyframe steppers, frame-number and timestamp
 * inputs, timeline scrubber, and the filmstrip of the decoded frame window.
 * Falls back to a timestamp-only seek bar while the frame index is unavailable.
 */
export function VideoFrameBrowser({
  t,
  source,
  frameWindow,
  previewBusy,
  indexBusy,
  frameInput,
  timeInput,
  scrubFrame,
  thumbnailUrls,
  sourceSamples,
  onFrameInputChange,
  onTimeInputChange,
  onScrubFrameChange,
  selectVideoFrame,
  onKeyDown,
}: {
  t: Translator;
  source: Source;
  frameWindow: MediaFrameWindow | null;
  previewBusy: boolean;
  indexBusy: boolean;
  frameInput: string;
  timeInput: string;
  scrubFrame: number;
  thumbnailUrls: Record<number, string>;
  sourceSamples: Sample[];
  onFrameInputChange: (value: string) => void;
  onTimeInputChange: (value: string) => void;
  onScrubFrameChange: (value: number) => void;
  selectVideoFrame: SelectVideoFrame;
  onKeyDown: (event: KeyboardEvent) => void;
}) {
  const currentFrame = frameWindow?.selected.frame_index ?? 0;
  const maxFrame = Math.max(0, (frameWindow?.total_frames ?? 1) - 1);

  function showNearbyTime() {
    const streamIndex = source.selectedStreamIndex ?? source.videoStreams[0]?.index;
    const requested = Number(timeInput);
    if (streamIndex === undefined || !Number.isFinite(requested) || requested < 0) return;
    const timestampSeconds = Math.min(requested, source.durationSeconds ?? requested);
    onTimeInputChange(timestampSeconds.toFixed(3));
    void selectVideoFrame(source, "timestamp", undefined, timestampSeconds);
  }

  return (
    <div
      className="frame-browser"
      tabIndex={frameWindow ? 0 : undefined}
      onKeyDown={onKeyDown}
    >
      {frameWindow ? (
        <>
          <p className="frame-keyboard-help">{t("media.keyboardHelp")}</p>
          <div className="frame-controls">
            <button
              className="icon-button"
              type="button"
              title={t("media.previousKeyframe")}
              aria-label={t("media.previousKeyframe")}
              disabled={previewBusy || !frameWindow.previous_keyframe}
              onClick={() => void selectVideoFrame(source, "previousKeyframe", currentFrame)}
            >
              <ChevronsLeft size={14} />
            </button>
            <button
              className="icon-button"
              type="button"
              title={t("media.previousFrame")}
              aria-label={t("media.previousFrame")}
              disabled={previewBusy || currentFrame <= 0}
              onClick={() => void selectVideoFrame(source, "frame", currentFrame - 1)}
            >
              <ChevronLeft size={14} />
            </button>
            <label>
              <span>{t("media.frameNumber")}</span>
              <input value={frameInput} inputMode="numeric" onChange={(event) => onFrameInputChange(event.target.value)} onKeyDown={(event) => {
                if (event.key === "Enter") void selectVideoFrame(source, "frame", Number(frameInput));
              }} />
            </label>
            <button
              className="icon-button"
              type="button"
              title={t("media.nextFrame")}
              aria-label={t("media.nextFrame")}
              disabled={previewBusy || currentFrame >= maxFrame}
              onClick={() => void selectVideoFrame(source, "frame", currentFrame + 1)}
            >
              <ChevronRight size={14} />
            </button>
            <button
              className="icon-button"
              type="button"
              title={t("media.nextKeyframe")}
              aria-label={t("media.nextKeyframe")}
              disabled={previewBusy || !frameWindow.next_keyframe}
              onClick={() => void selectVideoFrame(source, "nextKeyframe", currentFrame)}
            >
              <ChevronsRight size={14} />
            </button>
            <label>
              <span>{t("media.timestamp")}</span>
              <input value={timeInput} inputMode="decimal" onChange={(event) => onTimeInputChange(event.target.value)} onKeyDown={(event) => {
                if (event.key === "Enter") void selectVideoFrame(source, "timestamp", undefined, Number(timeInput));
              }} />
            </label>
            <button className="icon-button reveal-button" type="button" title={t("media.goToTime")} aria-label={t("media.goToTime")} onClick={() => void selectVideoFrame(source, "timestamp", undefined, Number(timeInput))}>
              <LocateFixed size={14} />
            </button>
          </div>
          <RangeScrubber
            min={0}
            max={maxFrame}
            step={1}
            value={scrubFrame}
            ariaLabel={t("media.timeline")}
            onChange={onScrubFrameChange}
            onCommit={(frame) => void selectVideoFrame(source, "frame", frame)}
          />
          <div className="filmstrip" aria-label={t("media.frameWindow")}>
            {frameWindow.frames.map((frame) => {
              const sampled = sourceSamples.some(
                (sample) =>
                  sample.streamIndex === source.selectedStreamIndex &&
                  sample.frameIndex === frame.frame_index,
              );
              return (
                <button
                  type="button"
                  key={frame.frame_index}
                  className={`${frame.frame_index === currentFrame ? "active" : ""} ${sampled ? "sampled" : ""}`}
                  aria-current={frame.frame_index === currentFrame ? "true" : undefined}
                  aria-label={`${t("media.frameNumber")} ${frame.frame_index}`}
                  onClick={() => void selectVideoFrame(source, "frame", frame.frame_index)}
                >
                  {thumbnailUrls[frame.frame_index] ? (
                    <img
                      className="filmstrip-thumbnail"
                      src={thumbnailUrls[frame.frame_index]}
                      alt=""
                      draggable={false}
                    />
                  ) : (
                    <span className="filmstrip-thumbnail-placeholder">
                      <LoaderCircle className="spin" size={13} />
                    </span>
                  )}
                  <span className="filmstrip-meta">
                    <strong>{frame.frame_index}</strong>
                    <span>{frame.picture_type ?? "-"}</span>
                  </span>
                </button>
              );
            })}
          </div>
        </>
      ) : (
        <>
          <div className="frame-controls">
            <label>
              <span>{t("media.timestamp")}</span>
              <input
                value={timeInput}
                inputMode="decimal"
                onChange={(event) => onTimeInputChange(event.target.value)}
                onKeyDown={(event) => {
                  if (event.key === "Enter") showNearbyTime();
                }}
              />
            </label>
            <button
              className="icon-button reveal-button"
              type="button"
              title={t("media.goToTime")}
              aria-label={t("media.goToTime")}
              onClick={() => showNearbyTime()}
            >
              <LocateFixed size={14} />
            </button>
            <span className="frame-index-status" role="status">
              {indexBusy ? <LoaderCircle className="spin" size={13} /> : null}
              {indexBusy ? t("media.indexingFrames") : t("media.indexUnavailable")}
            </span>
          </div>
          <RangeScrubber
            min={0}
            max={Math.max(0, source.durationSeconds ?? 0)}
            step={0.001}
            value={Math.min(Number(timeInput) || 0, source.durationSeconds ?? 0)}
            disabled={!source.durationSeconds}
            ariaLabel={t("media.timeline")}
            onChange={(seconds) => onTimeInputChange(seconds.toFixed(3))}
            onCommit={(seconds) => void selectVideoFrame(source, "timestamp", undefined, seconds)}
          />
        </>
      )}
    </div>
  );
}

function snapToStep(raw: number, min: number, max: number, step: number): number {
  const lo = Number.isFinite(min) ? min : 0;
  const hi = Number.isFinite(max) ? Math.max(lo, max) : lo;
  const clamped = Number.isFinite(raw) ? Math.min(hi, Math.max(lo, raw)) : lo;
  if (!(step > 0) || !Number.isFinite(step)) return clamped;
  return Math.min(hi, Math.max(lo, lo + Math.round((clamped - lo) / step) * step));
}

/** CSS slider instead of type=range: WebKitGTK paints GtkScale and can grab the pointer. */
function RangeScrubber({
  min,
  max,
  step,
  value,
  disabled,
  ariaLabel,
  onChange,
  onCommit,
}: {
  min: number;
  max: number;
  step: number;
  value: number;
  disabled?: boolean;
  ariaLabel: string;
  onChange: (value: number) => void;
  onCommit: (value: number) => void;
}) {
  const span = Math.max(max - min, 0);
  const current = snapToStep(value, min, max, step);
  const ratio = span === 0 ? 0 : (current - min) / span;

  function valueFromClientX(target: HTMLElement, clientX: number) {
    const rect = target.getBoundingClientRect();
    const t = rect.width <= 0 ? 0 : Math.min(1, Math.max(0, (clientX - rect.left) / rect.width));
    return snapToStep(min + t * span, min, max, step);
  }

  return (
    <div
      className="frame-timeline"
      role="slider"
      tabIndex={disabled ? -1 : 0}
      aria-label={ariaLabel}
      aria-valuemin={min}
      aria-valuemax={max}
      aria-valuenow={current}
      aria-disabled={disabled || undefined}
      style={{ ["--timeline-ratio" as string]: String(ratio) }}
      onPointerDown={(event) => {
        if (disabled || event.button !== 0) return;
        event.currentTarget.setPointerCapture(event.pointerId);
        onChange(valueFromClientX(event.currentTarget, event.clientX));
      }}
      onPointerMove={(event) => {
        if (disabled || !event.currentTarget.hasPointerCapture(event.pointerId)) return;
        onChange(valueFromClientX(event.currentTarget, event.clientX));
      }}
      onPointerUp={(event) => {
        if (disabled) return;
        const next = valueFromClientX(event.currentTarget, event.clientX);
        onChange(next);
        onCommit(next);
      }}
      onKeyDown={(event) => {
        if (disabled) return;
        let next: number | null = null;
        if (event.key === "ArrowLeft" || event.key === "ArrowDown") {
          next = snapToStep(current - step, min, max, step);
        } else if (event.key === "ArrowRight" || event.key === "ArrowUp") {
          next = snapToStep(current + step, min, max, step);
        } else if (event.key === "Home") {
          next = min;
        } else if (event.key === "End") {
          next = max;
        } else {
          return;
        }
        event.preventDefault();
        event.stopPropagation();
        onChange(next);
      }}
      onKeyUp={(event) => {
        if (disabled) return;
        if (
          event.key === "ArrowLeft" ||
          event.key === "ArrowDown" ||
          event.key === "ArrowRight" ||
          event.key === "ArrowUp" ||
          event.key === "Home" ||
          event.key === "End"
        ) {
          event.stopPropagation();
          onCommit(current);
        }
      }}
    >
      <span className="frame-timeline-track" aria-hidden="true">
        <span className="frame-timeline-fill" />
      </span>
      <span className="frame-timeline-thumb" aria-hidden="true" />
    </div>
  );
}

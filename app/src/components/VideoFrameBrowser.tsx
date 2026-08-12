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
          <input
            className="frame-timeline"
            type="range"
            min={0}
            max={maxFrame}
            step={1}
            value={scrubFrame}
            aria-label={t("media.timeline")}
            onChange={(event) => onScrubFrameChange(Number(event.target.value))}
            onPointerUp={(event) => void selectVideoFrame(
              source, "frame", Number(event.currentTarget.value),
            )}
            onKeyUp={(event) => void selectVideoFrame(
              source, "frame", Number(event.currentTarget.value),
            )}
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
          <input
            className="frame-timeline"
            type="range"
            min={0}
            max={Math.max(0, source.durationSeconds ?? 0)}
            step={0.001}
            value={Math.min(Number(timeInput) || 0, source.durationSeconds ?? 0)}
            disabled={!source.durationSeconds}
            aria-label={t("media.timeline")}
            onChange={(event) => onTimeInputChange(Number(event.target.value).toFixed(3))}
            onPointerUp={(event) => void selectVideoFrame(
              source, "timestamp", undefined,
              Number(event.currentTarget.value),
            )}
            onKeyUp={(event) => void selectVideoFrame(
              source, "timestamp", undefined,
              Number(event.currentTarget.value),
            )}
          />
        </>
      )}
    </div>
  );
}

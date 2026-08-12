import { describe, expect, it } from "vitest";
import type { Run } from "../project/types";
import { runActualBackend, runDecodeProvenance } from "./ResultsPage";

function runWithTelemetry(telemetry: Record<string, unknown>): Run {
  return { result: { telemetry } } as Run;
}

describe("runActualBackend", () => {
  it("reads the worker result telemetry without changing the project schema", () => {
    expect(runActualBackend(runWithTelemetry({
      backend: "cuda",
      cuda_device: "NVIDIA GeForce RTX 5080",
    }))).toEqual({ backend: "cuda", device: "NVIDIA GeForce RTX 5080" });
    expect(runActualBackend(runWithTelemetry({
      backend: "vulkan",
      vulkan_device: "Discrete Vulkan GPU",
    }))).toEqual({ backend: "vulkan", device: "Discrete Vulkan GPU" });
    expect(runActualBackend(runWithTelemetry({
      backend: "cpu",
      vulkan_device: "stale value",
    }))).toEqual({ backend: "cpu", device: undefined });
  });

  it("keeps older or malformed result payloads compatible", () => {
    expect(runActualBackend({ result: null } as Run)).toBeNull();
    expect(runActualBackend(runWithTelemetry({ backend: "auto" }))).toBeNull();
  });
});

describe("runDecodeProvenance", () => {
  it("reads zero-copy state and the final fallback reason", () => {
    const run = {
      result: {
        provenance: {
          decoder: "software",
          zero_copy: false,
          fallback_chain: [
            { reason: "unsupported profile" },
            { reason: "decoder failed at frame 12" },
          ],
        },
      },
    } as Run;
    expect(runDecodeProvenance(run)).toEqual({
      decoder: "software",
      zeroCopy: false,
      fallbackReason: "decoder failed at frame 12",
    });
  });

  it("keeps legacy results compatible", () => {
    expect(runDecodeProvenance({ result: { telemetry: {} } } as Run)).toBeNull();
  });
});

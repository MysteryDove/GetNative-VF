# Ralplan Consensus: Staged Pre-GPU Foundation

## Status

- Planning state: `complete`
- Consensus gate: `complete`
- Execution authorized by this record: Stage 0 only
- Stage 1 authorization: conditional on an exact `PROCEED_STAGE1` result
- Worktree: `/Users/owen/Documents/GetNative-VF-pre-gpu-stage1`
- Branch: `perf/pre-gpu-stage1`
- Frozen ancestor: `c320503d0b1f2356ada044f87467cc17665fd4fd`

## Planning artifacts

| Artifact | SHA-256 |
| --- | --- |
| `.omx/plans/prd-pre-gpu-foundation-staged.md` | `6c508ec2af5c1f6efda99149088af10863c9c3c032ea638705372e942c54a6c9` |
| `.omx/plans/test-spec-pre-gpu-foundation-stage-1.md` | `030161fd648dd29755c737e17f48ea65cc3c505fdd0914b8d120577eeefc9311` |

These exact artifacts were reviewed. Any content change invalidates this gate
and requires a new Architect then Critic review.

## Architect review

- Role: native `architect`
- Agent thread: `019fb3ee-f9f7-7963-a8e3-bcb826c2ab6b`
- Overall sequence: Architect review 8
- Verdict: `APPROVE`
- Evidence: the paired small-request formula and exhaustive revert disposition
  close the final two blockers; the private conditional design is executable
  without widening deferred host/GPU architecture.
- Local full review:
  `.omx/specs/ralplan-pre-gpu-foundation-staged-architect-review-3.md`

## Critic review

- Role: native `critic`
- Agent thread: `019fb3f7-18f0-7a63-ae8c-288204e6db56`
- Order: completed after the approving Architect review
- Verdict: `APPROVE`
- Evidence: scope, arithmetic, tests, pre-mortem, immutable artifacts, terminal
  outcomes, and worktree ownership are clear, complete, and verifiable.
- Local full review:
  `.omx/specs/ralplan-pre-gpu-foundation-staged-critic-review-1.md`

## Consensus gate

```text
ralplan_consensus_gate.complete: true
planning_artifacts.present: true
ralplan_architect_review.present: true
ralplan_architect_review.approving: true
ralplan_critic_review.present: true
ralplan_critic_review.approving: true
review_order: architect_then_critic
```

## Execution handoff

1. Implement and verify Stage 0 measurement only in `perf/pre-gpu-stage1`.
2. Record one valid 21-sample outcome:
   `PROCEED_STAGE1`, `STOP_AND_REDIRECT_GPU`, or `STAGE0_BLOCKED`.
3. Implement Stage 1 only when the outcome is exactly `PROCEED_STAGE1` and
   `median(plan_ms_i / metal_total_ms_i) >= 0.10`.
4. Stop after the Stage 1 disposition. No scheduler, persistent cache, packing,
   cancellation, CUDA, or Vulkan work is authorized here.

The lane must not modify production Metal kernels/runtime, ARM64 NEON, Metal
arena/cache work, Tauri/React, `main`, `perf/backend-hotpath`, or
`integration/perf-stack`.

## Verification boundary

- Target branch verified as `perf/pre-gpu-stage1`.
- Plan hashes match the worktree handoff.
- Local fixture `engine/bench/fixtures/6.2-1.png` verified against its checksum.
- Planning-only mechanical checks passed; no Stage 0 source implementation or
  21-sample decision run has occurred yet.

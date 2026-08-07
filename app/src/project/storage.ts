import { invoke } from "@tauri-apps/api/core";
import { openedToProjectState, projectStateToManifest } from "./normalize";
import type {
  ProjectCommandResult,
  ProjectState,
  RecentProjectEntry,
  RecoveryInfo,
} from "./types";

/**
 * Storage boundary for Project persistence.
 * Callers must not assume whether a Project is a single file or a future bundle.
 */
export type ProjectStorage = {
  createNamed(name: string): Promise<ProjectCommandResult>;
  createUntitled(): Promise<ProjectCommandResult>;
  open(path?: string): Promise<ProjectCommandResult>;
  save(
    state: ProjectState,
    options?: { pickPath?: boolean; dialogName?: string },
  ): Promise<ProjectCommandResult>;
  autosave(state: ProjectState): Promise<ProjectCommandResult>;
  listRecent(): Promise<RecentProjectEntry[]>;
  removeRecent(path: string): Promise<RecentProjectEntry[]>;
  reveal(path: string): Promise<void>;
  recoveryInfo(): Promise<RecoveryInfo>;
  recover(): Promise<ProjectCommandResult>;
  discardRecovery(): Promise<RecoveryInfo>;
};

export function autosaveStoragePath(state: ProjectState): string | null {
  return state.project.untitled ? null : state.project.storagePath;
}

function asResult(value: ProjectCommandResult): ProjectCommandResult {
  return {
    ok: value.ok,
    opened: value.opened ?? null,
    recent: value.recent ?? null,
    recovery: value.recovery ?? null,
    error: value.error ?? null,
    warnings: value.warnings ?? [],
  };
}

function requireOk(result: ProjectCommandResult, fallback: string): ProjectCommandResult {
  if (!result.ok) {
    throw new Error(result.error?.message ?? fallback);
  }
  return result;
}

export function createTauriProjectStorage(): ProjectStorage {
  return {
    async createNamed(name) {
      return asResult(
        await invoke<ProjectCommandResult>("project_create", {
          request: {
            name,
            path: null,
            pickPath: true,
          },
        }),
      );
    },
    async createUntitled() {
      return asResult(await invoke<ProjectCommandResult>("project_create_untitled"));
    },
    async open(path) {
      return asResult(
        await invoke<ProjectCommandResult>("project_open", {
          request: {
            path: path ?? null,
            pickPath: path ? false : true,
          },
        }),
      );
    },
    async save(state, options) {
      return asResult(
        await invoke<ProjectCommandResult>("project_save", {
          request: {
            path: state.project.untitled ? null : state.project.storagePath,
            manifest: projectStateToManifest(state),
            pickPath: options?.pickPath ?? (!state.project.storagePath || state.project.untitled),
            dialogName: options?.dialogName ?? null,
          },
        }),
      );
    },
    async autosave(state) {
      return asResult(
        await invoke<ProjectCommandResult>("project_autosave", {
          request: {
            path: autosaveStoragePath(state),
            manifest: projectStateToManifest(state),
            pickPath: false,
            dialogName: null,
          },
        }),
      );
    },
    async listRecent() {
      const result = requireOk(
        asResult(await invoke<ProjectCommandResult>("project_list_recent")),
        "recent Project list operation failed",
      );
      return result.recent ?? [];
    },
    async removeRecent(path) {
      const result = requireOk(asResult(
        await invoke<ProjectCommandResult>("project_remove_recent", {
          request: { path },
        }),
      ), "recent Project removal failed");
      return result.recent ?? [];
    },
    async reveal(path) {
      requireOk(
        asResult(
          await invoke<ProjectCommandResult>("project_reveal", {
            request: { path },
          }),
        ),
        "show Project in folder operation failed",
      );
    },
    async recoveryInfo() {
      const result = requireOk(
        asResult(await invoke<ProjectCommandResult>("project_recovery_info")),
        "Project recovery lookup failed",
      );
      return (
        result.recovery ?? {
          present: false,
          path: null,
          name: null,
          updated_at: null,
        }
      );
    },
    async recover() {
      return asResult(await invoke<ProjectCommandResult>("project_recover"));
    },
    async discardRecovery() {
      const result = requireOk(
        asResult(await invoke<ProjectCommandResult>("project_discard_recovery")),
        "Project recovery operation failed",
      );
      return (
        result.recovery ?? {
          present: false,
          path: null,
          name: null,
          updated_at: null,
        }
      );
    },
  };
}

export function applyOpenResult(
  result: ProjectCommandResult,
): { state: ProjectState } | { error: NonNullable<ProjectCommandResult["error"]> } {
  if (!result.ok || !result.opened) {
    return {
      error: result.error ?? {
        code: "io_error",
        message: "project operation failed",
      },
    };
  }
  return { state: openedToProjectState(result.opened) };
}

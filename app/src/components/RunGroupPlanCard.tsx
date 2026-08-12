import type { JSX } from "react";
import type { Translator } from "../i18n";

/**
 * Shared run-group plan preview: a heading, a help-copy summary line
 * (group type · member count · optional work estimate), an optional member
 * list truncated at `truncateAt` with a "+N more" tail, and an optional note
 * shown when the group spans multiple members. Members are pre-mapped by the
 * caller so each page keeps its own member rendering semantics.
 */
export function RunGroupPlanCard(props: {
  t: Translator;
  title: string;
  summary: string;
  workEstimate?: string | null;
  members?: Array<{ key: string; title: string; subtitle?: string }>;
  truncateAt?: number;
  multiMemberNote?: string | null;
}): JSX.Element {
  const { title, summary, workEstimate, members, multiMemberNote } = props;
  const truncateAt = props.truncateAt ?? 12;
  return (
    <div className="run-group-plan">
      <h3>{title}</h3>
      <p className="help-copy">
        {summary}
        {workEstimate ? (
          <>
            {" · "}
            {workEstimate}
          </>
        ) : null}
      </p>
      {members && members.length ? (
        <ul className="run-group-members">
          {members.slice(0, truncateAt).map((member) => (
            <li key={member.key}>
              <strong>{member.title}</strong>
              {member.subtitle ? <span>{member.subtitle}</span> : null}
            </li>
          ))}
          {members.length > truncateAt ? (
            <li className="muted">+{members.length - truncateAt}</li>
          ) : null}
        </ul>
      ) : null}
      {multiMemberNote ? <p className="help-copy">{multiMemberNote}</p> : null}
    </div>
  );
}

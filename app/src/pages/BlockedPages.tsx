import type { ReactNode } from "react";
import type { Translator } from "../i18n";
import { BlockedState } from "../components/BlockedState";

export function VerifyPage({ t }: { t: Translator }) {
  return (
    <PageFrame title={t("verify.title")}>
      <BlockedState title={t("verify.blockedTitle")} body={t("verify.blockedBody")} />
    </PageFrame>
  );
}

export function ResultsPage({ t }: { t: Translator }) {
  return (
    <PageFrame title={t("results.title")}>
      <BlockedState title={t("results.emptyTitle")} body={t("results.emptyBody")} />
    </PageFrame>
  );
}

export function SettingsPage({
  t,
  language,
  onLanguageChange,
}: {
  t: Translator;
  language: "zh-CN" | "en";
  onLanguageChange: (locale: "zh-CN" | "en") => void;
}) {
  return (
    <PageFrame title={t("settings.title")}>
      <section className="page-section">
        <label className="language-field block">
          <span>{t("language.label")}</span>
          <select
            value={language}
            onChange={(event) => onLanguageChange(event.target.value as "zh-CN" | "en")}
          >
            <option value="zh-CN">{t("language.zhCN")}</option>
            <option value="en">{t("language.en")}</option>
          </select>
        </label>
        <p className="help-copy">{t("settings.languageHelp")}</p>
      </section>
      <section className="page-section">
        <h3>{t("settings.persistence")}</h3>
        <p className="help-copy">{t("settings.persistenceHelp")}</p>
      </section>
    </PageFrame>
  );
}

function PageFrame({ title, children }: { title: string; children: ReactNode }) {
  return (
    <div className="page-panel">
      <div className="page-header">
        <h2>{title}</h2>
      </div>
      {children}
    </div>
  );
}

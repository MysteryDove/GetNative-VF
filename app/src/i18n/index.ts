import en from "./locales/en.json";
import zhCN from "./locales/zh-CN.json";

export type LocaleCode = "zh-CN" | "en";

export type MessageKey = keyof typeof zhCN;

const catalogs: Record<LocaleCode, Record<MessageKey, string>> = {
  "zh-CN": zhCN,
  en,
};

export const DEFAULT_LOCALE: LocaleCode = "en";

export function isLocaleCode(value: string): value is LocaleCode {
  return value === "zh-CN" || value === "en";
}

export function localeKeys(locale: LocaleCode = "zh-CN"): MessageKey[] {
  return Object.keys(catalogs[locale]) as MessageKey[];
}

/** Returns missing keys in `other` relative to the zh-CN catalog. */
export function missingLocaleKeys(other: LocaleCode): MessageKey[] {
  const required = localeKeys("zh-CN");
  const available = new Set(localeKeys(other));
  return required.filter((key) => !available.has(key));
}

export function assertLocaleParity(): void {
  const missingEn = missingLocaleKeys("en");
  const extraEn = localeKeys("en").filter((key) => !(key in catalogs["zh-CN"]));
  if (missingEn.length || extraEn.length) {
    throw new Error(
      `locale key parity failed; missing en: ${missingEn.join(", ") || "none"}; extra en: ${extraEn.join(", ") || "none"}`,
    );
  }
}

export function createTranslator(locale: LocaleCode) {
  const table = catalogs[locale] ?? catalogs[DEFAULT_LOCALE];
  return function t(key: MessageKey, vars?: Record<string, string | number>): string {
    let phrase = table[key] ?? catalogs[DEFAULT_LOCALE][key] ?? String(key);
    if (vars) {
      for (const [name, value] of Object.entries(vars)) {
        phrase = phrase.split(`{${name}}`).join(String(value));
      }
    }
    return phrase;
  };
}

export type Translator = ReturnType<typeof createTranslator>;

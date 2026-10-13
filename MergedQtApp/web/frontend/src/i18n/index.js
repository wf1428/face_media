/**
 * @file index.js
 * @brief 沿用原界面路由、语言或展示配置，真实业务限于 V1 接口。
 * @author Du lin
 * @date 2026-10-09
 */
/**
 * i18n 国际化入口
 * 功能说明：创建 vue-i18n 实例，静态注册 zh/en/vi/th 四语语言包，
 *          并同步 Vant 组件库语言、<html lang> 与 document.title。
 * 设计说明：本 H5 文案体量小且离线托管，四语全部静态注册（非动态 import），
 *          保证首屏与切换瞬间无“语言包未注册”空窗；语言选择持久化到 localStorage。
 */
import { createI18n } from 'vue-i18n'
import { Locale as VantLocale } from 'vant'
import vantZh from 'vant/es/locale/lang/zh-CN.mjs'
import vantEn from 'vant/es/locale/lang/en-US.mjs'
import vantVi from 'vant/es/locale/lang/vi-VN.mjs'
import vantTh from 'vant/es/locale/lang/th-TH.mjs'

import zh from './locales/zh.json'
import en from './locales/en.json'
import vi from './locales/vi.json'
import th from './locales/th.json'

/** localStorage 键名 */
const STORAGE_KEY = 'yc_qt_language'

/**
 * 支持的语言清单
 * vantLang：Vant 组件语言包名；htmlLang：<html lang>；timeLocale：toLocaleTimeString 区域
 */
export const SUPPORT_LANGS = [
  { value: 'zh', labelKey: 'lang.zh', vantLang: 'zh-CN', vantPack: vantZh, htmlLang: 'zh-CN', timeLocale: 'zh-CN' },
  { value: 'en', labelKey: 'lang.en', vantLang: 'en-US', vantPack: vantEn, htmlLang: 'en', timeLocale: 'en-US' },
  { value: 'vi', labelKey: 'lang.vi', vantLang: 'vi-VN', vantPack: vantVi, htmlLang: 'vi-VN', timeLocale: 'vi-VN' },
  { value: 'th', labelKey: 'lang.th', vantLang: 'th-TH', vantPack: vantTh, htmlLang: 'th-TH', timeLocale: 'th-TH' }
]

/**
 * 浏览器语言检测：将 navigator.language(s) 映射到支持的语言
 * 优先级：精确前缀匹配（zh/en/vi/th），无法识别则回退 null
 * @returns {string|null} zh/en/vi/th 或 null
 */
function detectBrowserLang() {
  const navLangs = (navigator.languages && navigator.languages.length)
    ? navigator.languages
    : [navigator.language || navigator.userLanguage || '']
  for (const raw of navLangs) {
    const lower = String(raw).toLowerCase()
    if (lower.startsWith('zh')) return 'zh'
    if (lower.startsWith('en')) return 'en'
    if (lower.startsWith('vi')) return 'vi'
    if (lower.startsWith('th')) return 'th'
  }
  return null
}

/**
 * 初始语言决策：用户已保存偏好 > 浏览器语言自动检测 > 中文兜底
 * @returns {string} zh/en/vi/th
 */
function resolveInitialLang() {
  const saved = localStorage.getItem(STORAGE_KEY)
  if (SUPPORT_LANGS.some((l) => l.value === saved)) return saved
  return detectBrowserLang() || 'zh'
}

const i18n = createI18n({
  legacy: false,
  locale: resolveInitialLang(),
  fallbackLocale: 'zh',
  messages: { zh, en, vi, th },
  globalInjection: true
})

/**
 * 按语言同步外部环境：Vant 语言包、html lang、文档标题
 * @param {string} lang 语言码 zh/en/vi/th
 */
function syncEnv(lang) {
  const conf = SUPPORT_LANGS.find((l) => l.value === lang) || SUPPORT_LANGS[0]
  VantLocale.use(conf.vantLang, conf.vantPack)
  document.documentElement.lang = conf.htmlLang
  document.title = i18n.global.t('login.title')
}

/**
 * 切换语言并持久化
 * @param {string} lang 语言码 zh/en/vi/th
 */
export function setLanguage(lang) {
  if (!SUPPORT_LANGS.some((l) => l.value === lang)) return
  localStorage.setItem(STORAGE_KEY, lang)
  i18n.global.locale.value = lang
  syncEnv(lang)
}

/**
 * 获取当前时间格式化区域（供 mock 等非组件模块使用）
 * @returns {string} BCP 47 区域码
 */
export function currentTimeLocale() {
  const lang = i18n.global.locale.value
  const conf = SUPPORT_LANGS.find((l) => l.value === lang)
  return conf ? conf.timeLocale : 'zh-CN'
}

// 启动即同步一次（语言包已静态注册，直接生效）
syncEnv(i18n.global.locale.value)

export default i18n

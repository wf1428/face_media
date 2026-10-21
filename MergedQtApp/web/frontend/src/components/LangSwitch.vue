<!--
 @file LangSwitch.vue
 @brief 保留原语言选择入口与语言偏好持久化。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <!-- 语言切换器：globe 图标 + 当前语言名 -> ActionSheet 选择 zh/en/vi/th -->
  <span class="lang-switch" :class="{ 'is-text': showLabel }" @click="show = true">
    <svg
      class="language-icon"
      :width="size"
      :height="size"
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      stroke-width="1.7"
      aria-hidden="true"
    >
      <circle cx="12" cy="12" r="9" />
      <ellipse cx="12" cy="12" rx="4" ry="9" />
      <path d="M3 12h18M5 6.5h14M5 17.5h14" />
    </svg>
    <span v-if="showLabel" class="lang-switch-text">{{ currentLabel }}</span>
  </span>
  <van-action-sheet
    v-model:show="show"
    :title="t('lang.title')"
    :actions="actions"
    :cancel-text="t('common.cancel')"
    closeable
    @select="onSelect"
    @cancel="show = false"
  />
</template>

<script setup>
/**
 * 语言切换组件
 * 功能：弹出 ActionSheet 供选择中/英/越/泰，选择后经 i18n.setLanguage 即时切换
 */
import { ref, computed } from 'vue'
import { useI18n } from 'vue-i18n'
import { SUPPORT_LANGS, setLanguage } from '../i18n'

const { t, locale } = useI18n()

const props = defineProps({
  // 图标尺寸（px）
  size: { type: [String, Number], default: 22 },
  // 是否在图标旁显示当前语言名称
  showLabel: { type: Boolean, default: true },
})

const show = ref(false)

/** @brief 随当前语言更新对应的本地名称。 */
const currentLabel = computed(() => {
  const conf = SUPPORT_LANGS.find((l) => l.value === locale.value)
  return conf ? t(conf.labelKey) : ''
})

/** @brief 根据语言状态生成菜单动作项和选中色。 */
const actions = computed(() =>
  SUPPORT_LANGS.map((l) => ({
    name: t(l.labelKey),
    value: l.value,
    color: locale.value === l.value ? 'var(--primary)' : '',
  })),
)

/** @brief 选择语言后关闭弹层 */
function onSelect(item) {
  setLanguage(item.value)
  show.value = false
}
</script>

<style scoped>
.lang-switch {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  color: var(--text-2);
  padding: 4px;
  cursor: pointer;
  user-select: none;
}
.language-icon {
  display: block;
  flex-shrink: 0;
}
/* 带文字时做成浅底胶囊，更像可点入口 */
.lang-switch.is-text {
  gap: 4px;
  padding: 5px 12px;
  border-radius: 999px;
  background: var(--card, #fff);
  box-shadow: 0 1px 4px rgba(0, 0, 0, 0.08);
  font-size: 14px;
  line-height: 1;
}
.lang-switch-text {
  white-space: nowrap;
}
</style>

<!--
 @file PassTimePicker.vue
 @brief 使用 Vant 滑动列选择人员通行时间或日期时间，确认后才修改外部草稿。
 @author Du lin
 @date 2026-10-23
-->
<template>
  <van-popup :show="show" position="bottom" round @update:show="emit('update:show', $event)">
    <div class="pass-time-pop">
      <div class="pop-title">{{ title }}</div>
      <van-picker-group v-if="dateTime" :tabs="[t('common.date'), t('common.time')]" :show-toolbar="false">
        <van-date-picker ref="datePicker" class="pass-date-wheel" v-model="dateValues" :min-date="minDate" :max-date="maxDate" />
        <van-time-picker ref="timePicker" class="pass-clock-wheel" v-model="timeValues" :columns-type="['hour', 'minute']" />
      </van-picker-group>
      <van-time-picker v-else ref="timePicker" v-model="timeValues" :show-toolbar="false" :columns-type="['hour', 'minute']" />
      <div class="picker-actions">
        <van-button block round plain @click="emit('update:show', false)">{{ t('common.cancel') }}</van-button>
        <van-button block round type="primary" @click="confirm">{{ t('common.confirm') }}</van-button>
      </div>
    </div>
  </van-popup>
</template>

<script setup>
import { ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'

/** @description show 控制弹窗，modelValue 为 HH:mm 或本地 yyyy-MM-dd HH:mm:ss；dateTime 启用日期列。 */
const props = defineProps({ show: Boolean, modelValue: { type: String, default: '' }, title: String, dateTime: Boolean })
/** @description 仅确认时提交 modelValue；取消、点击遮罩只关闭弹窗。 */
const emit = defineEmits(['update:show', 'update:modelValue'])
const { t } = useI18n()
const datePicker = ref(null), timePicker = ref(null)
const dateValues = ref([]), timeValues = ref(['08', '00'])
const minDate = new Date(1900, 0, 1), maxDate = new Date(2199, 11, 31)

/**
 * 从持久化值复制独立草稿；含时区的 ISO 时间先转换为浏览器本地时间。
 * @param {boolean} open 是否打开选择器。
 */
function prepare(open) {
  if (!open) return
  const pad = (n) => String(n).padStart(2, '0')
  if (props.dateTime) {
    const value = props.modelValue.trim()
    const parsed = new Date(value.replace(' ', 'T'))
    const date = value && Number.isFinite(parsed.getTime()) ? parsed : new Date()
    dateValues.value = [String(date.getFullYear()), pad(date.getMonth() + 1), pad(date.getDate())]
    timeValues.value = [pad(date.getHours()), pad(date.getMinutes())]
  } else timeValues.value = /^([01]\d|2[0-3]):[0-5]\d$/.test(props.modelValue) ? props.modelValue.split(':') : ['08', '00']
}
watch(() => props.show, prepare)

/** @brief 停止滚动惯性并提交已选值，日期时间按设备接受的本地秒精度格式保存。 */
function confirm() {
  const time = timePicker.value?.confirm()?.selectedValues || timeValues.value
  const date = props.dateTime ? (datePicker.value?.confirm()?.selectedValues || dateValues.value) : []
  emit('update:modelValue', props.dateTime ? `${date.join('-')} ${time.join(':')}:00` : time.join(':'))
  emit('update:show', false)
}
</script>

<style scoped>
.pass-time-pop { padding: var(--page-pad); }
.pop-title { font-size: 18px; font-weight: 600; text-align: center; margin-bottom: 12px; }
.picker-actions { display: flex; gap: 12px; margin-top: 12px; }
</style>

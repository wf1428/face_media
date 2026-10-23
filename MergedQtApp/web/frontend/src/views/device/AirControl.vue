<!--
 @file AirControl.vue
 @brief 保留空调视觉控件，禁用全部设备控制，不发送业务请求。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="page">
    <UnsupportedPanel>
      <div class="header">
        <div class="title">{{ $t('air.title') }}</div>
      </div>

      <!-- 主控面板 -->
      <div class="air-panel">
        <div class="temp-row">
          <div class="temp-main">{{ air.temperature }}<span class="unit">℃</span></div>
          <div class="temp-sub">
            <span>{{ $t('air.roomTemp') }} {{ air.currentTemp }}℃</span>
            <span>{{ $t('air.humidity') }} {{ air.humidity }}%</span>
          </div>
        </div>

        <div class="brand">{{ air.brand || '—' }} · {{ statusTextOf(air.status) }}</div>

        <div class="power-row">
          <span class="power-label">{{ $t('air.power') }}</span>
          <van-switch disabled :model-value="air.airOn === 1" size="26" />
        </div>

        <div class="temp-control">
          <span class="ctrl-label">{{ $t('air.setTemp') }}</span>
          <div class="temp-btns">
            <van-button disabled round plain size="small">-</van-button>
            <span class="temp-val">{{ air.temperature }}℃</span>
            <van-button disabled round plain size="small">+</van-button>
          </div>
        </div>
      </div>

      <!-- 模式 -->
      <van-cell-group inset :title="$t('air.mode')">
        <div class="mode-grid">
          <div
            v-for="m in modeOptions"
            :key="m.value"
            class="mode-item"
            :class="{ active: air.mode === m.value }"
          >
            <van-icon :name="m.icon" size="24" />
            <span>{{ $t(m.labelKey) }}</span>
          </div>
        </div>
      </van-cell-group>

      <!-- 风速 -->
      <van-cell-group inset :title="$t('air.fanSpeed')">
        <div class="mode-grid">
          <div
            v-for="f in funOptions"
            :key="f.value"
            class="mode-item"
            :class="{ active: air.fun === f.value }"
          >
            <span>{{ $t(f.labelKey) }}</span>
          </div>
        </div>
      </van-cell-group>

      <!-- 循环定时 -->
      <van-cell-group inset :title="$t('air.loopTimer')">
        <van-cell :title="$t('air.loopSwitch')" center>
          <template #right-icon>
            <van-switch disabled :model-value="air.isLoopOn === 1" size="20" />
          </template>
        </van-cell>
      </van-cell-group>

      <!-- 周定时计划 -->
      <van-cell-group inset :title="$t('air.weekPlan')">
        <van-cell
          v-for="w in weekList"
          :key="w.key"
          :title="$t(w.labelKey)"
          :value="planValue(air.plan[w.key])"
          is-link
        />
      </van-cell-group>

      <!-- 周计划编辑 -->
      <van-popup v-model:show="showPlan" position="bottom" round>
        <div class="plan-pop">
          <div class="time-title">{{ $t(planTarget.labelKey) }}</div>
          <van-cell :title="$t('common.enable')" center>
            <template #right-icon>
              <van-switch disabled v-model="planEnabled" size="20" />
            </template>
          </van-cell>
          <van-cell
            :title="$t('elevator.onTime')"
            :value="planForm.timeOn === 'null' ? $t('common.notSet') : planForm.timeOn"
            is-link
          />
          <van-cell
            :title="$t('elevator.offTime')"
            :value="planForm.timeOFF === 'null' ? $t('common.notSet') : planForm.timeOFF"
            is-link
          />
          <van-button disabled type="primary" block round style="margin-top: 12px">{{
            $t('common.save')
          }}</van-button>
        </div>
      </van-popup>

      <!-- 时间选择 -->
      <van-popup v-model:show="showPlanTime" position="bottom" round>
        <div class="plan-pop">
          <div class="time-title">
            {{ planTimeTarget === 'timeOn' ? $t('elevator.onTime') : $t('elevator.offTime') }}
          </div>
          <van-time-picker
            disabled
            v-model="planTimeValue"
            title=""
            :columns-type="['hour', 'minute']"
          />
          <van-button disabled type="primary" block round style="margin-top: 12px">{{
            $t('common.confirm')
          }}</van-button>
        </div>
      </van-popup>
    </UnsupportedPanel>
  </div>
</template>

<script setup>
import { reactive, ref, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'

const { t } = useI18n()

const air = reactive({
  airOn: 0,
  isLoopOn: 0,
  mode: '',
  brand: '',
  fun: '',
  temperature: '—',
  currentTemp: '—',
  humidity: '—',
  status: '',
  plan: {},
})

// 模式/风速/星期选项：存 i18n 键（labelKey），模板 t() 渲染，切换语言即时更新
const modeOptions = computed(() => [
  { value: 'cool', labelKey: 'air.cool', icon: 'snowflake-o' },
  { value: 'heat', labelKey: 'air.heat', icon: 'fire-o' },
  { value: 'fan', labelKey: 'air.fan', icon: 'award-o' },
])
const funOptions = computed(() => [
  { value: 'low', labelKey: 'air.low' },
  { value: 'high', labelKey: 'air.high' },
])

const weekList = computed(() => [
  { key: 'week1', labelKey: 'air.week1' },
  { key: 'week2', labelKey: 'air.week2' },
  { key: 'week3', labelKey: 'air.week3' },
  { key: 'week4', labelKey: 'air.week4' },
  { key: 'week5', labelKey: 'air.week5' },
  { key: 'week6', labelKey: 'air.week6' },
  { key: 'week7', labelKey: 'air.week7' },
  { key: 'week8', labelKey: 'air.everyday' },
])

const showPlan = ref(false)
const planTarget = ref({ key: 'week1', labelKey: 'air.week1' })
const planEnabled = ref(false)
const planForm = reactive({
  timeOn: 'null',
  timeOFF: 'null',
  mode: 'cool',
  fun: 'low',
  temperature: 25,
})

const showPlanTime = ref(false)
const planTimeTarget = ref('timeOn')
const planTimeValue = ref(['08', '00'])

/**
 * @brief 空调状态文案（渲染时按当前语言取值）
 * @param {string} status 页面空调状态值。
 * @returns {string} 当前语言的空调状态文本，未知状态显示占位符。
 */
function statusTextOf(status) {
  const keyMap = { running: 'air.running', standby: 'air.standby', off: 'air.off' }
  return keyMap[status] ? t(keyMap[status]) : '—'
}

/**
 * @brief 保留空调状态加载入口；当前版本不接入控制业务。
 */
async function load() {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留空调补丁提交入口，当前版本未接入实际执行。
 * @param {object} patch 预留的空调状态补丁，当前版本不发送。
 */
async function commit(patch) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留空调开关操作入口；当前版本不下发命令。
 * @param {boolean} v 开关目标状态；当前空调入口未接入实际执行。
 */
function setAirOn(v) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留温度步进操作入口；当前版本不下发命令。
 * @param {number} d 温度调整增量；当前入口未接入实际执行。
 */
function stepTemp(d) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留空调模式选择入口；当前版本不下发命令。
 * @param {string} m 目标空调模式；当前入口未接入实际执行。
 */
function setMode(m) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留风速选择入口；当前版本不下发命令。
 * @param {string} f 目标风速；当前入口未接入实际执行。
 */
function setFun(f) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留循环方式选择入口；当前版本不下发命令。
 * @param {boolean} v 开关目标状态；当前空调入口未接入实际执行。
 */
function setLoop(v) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 将定时计划转换为页面显示文本。
 * @param {object} p 当前人员记录或页面定时计划。
 * @returns {string} 计划起止时间文本，未设置时返回占位提示。
 */
function planValue(p) {
  if (!p || p.timeOn === 'null' || p.timeOFF === 'null') return t('common.notSet')
  return `${p.timeOn} ~ ${p.timeOFF}`
}

/**
 * @brief 保留定时计划编辑入口；当前版本不接入。
 * @param {string} w 待编辑的计划名称。
 */
function openPlan(w) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留定时时间选择入口；当前版本不接入。
 * @param {string} target 待编辑的时间字段或计划字段名称。
 */
function openPlanTime(target) {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留定时时间确认入口；当前版本不接入。
 */
function confirmPlanTime() {
  /* 本版本仅保留控件，业务处理未接入。 */
}

/**
 * @brief 保留定时计划保存入口；当前版本不下发命令。
 */
async function savePlan() {
  /* 本版本仅保留控件，业务处理未接入。 */
}

import UnsupportedPanel from '../../components/UnsupportedPanel.vue'
</script>

<style scoped>
.page {
  padding-bottom: 40px;
}
.header {
  padding: var(--page-pad) var(--page-pad) 0;
}
.title {
  font-size: 20px;
  font-weight: 600;
  margin-bottom: var(--gap);
}

.air-panel {
  margin: var(--gap) var(--page-pad);
  padding: 24px var(--page-pad);
  background: linear-gradient(160deg, #eefbf4, #ffffff);
  border-radius: var(--radius-panel);
  box-shadow: var(--shadow-card);
  text-align: center;
}
.temp-row {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 24px;
}
.temp-main {
  font-size: 64px;
  font-weight: 200;
  line-height: 1;
  color: var(--primary);
}
.unit {
  font-size: 20px;
  color: var(--text-2);
  margin-left: 2px;
}
.temp-sub {
  display: flex;
  flex-direction: column;
  gap: 6px;
  color: var(--text-3);
  font-size: 13px;
}
.brand {
  margin-top: 10px;
  color: var(--text-3);
  font-size: 13px;
}
.power-row {
  margin-top: 18px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 10px;
}
.power-label {
  color: var(--text-2);
  font-size: 14px;
}
.temp-control {
  margin-top: 16px;
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 16px;
}
.ctrl-label {
  color: var(--text-2);
  font-size: 13px;
}
.temp-btns {
  display: flex;
  align-items: center;
  gap: 12px;
}
.temp-val {
  font-size: 20px;
  min-width: 44px;
  text-align: center;
}

.mode-grid {
  display: flex;
  gap: 12px;
  padding: 14px;
  flex-wrap: wrap;
}
.mode-item {
  flex: 1;
  min-width: 80px;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 6px;
  padding: 14px 8px;
  border-radius: var(--radius-sm);
  background: var(--bg);
  color: var(--text-2);
  font-size: 14px;
}
.mode-item.active {
  background: #e6f7ef;
  color: var(--primary);
}

.plan-pop {
  padding: var(--page-pad);
}
.time-title {
  font-size: 18px;
  font-weight: 600;
  text-align: center;
  margin-bottom: 16px;
}
</style>

<!--
 @file ElevatorControl.vue
 @brief 保留电梯视觉控件，禁用全部设备控制，不发送业务请求。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="page">
    <UnsupportedPanel>
    <div class="header">
      <div class="title">{{ $t('elevator.title') }}</div>
    </div>

    <!-- 主板电源控制 -->
    <van-cell-group inset :title="$t('elevator.boardPower')">
      <van-cell :title="$t('elevator.onTime')" :value="fmt().onTime" is-link />
      <van-cell :title="$t('elevator.offTime')" :value="fmt().offTime" is-link />
    </van-cell-group>
    <div class="actions-row">
      <van-button disabled size="small" round type="primary">{{ $t('elevator.submitBoard') }}</van-button>
      <van-button disabled size="small" round plain type="danger">{{ $t('elevator.rebootNow') }}</van-button>
    </div>

    <!-- 楼层开放 / 限行 -->
    <van-cell-group inset :title="$t('elevator.floorLimit')">
      <van-cell v-for="(f, i) in floorLimit.floors" :key="i" :title="floorLabel(f.floor)" center>
        <template #right-icon>
          <van-tag :type="f.status === 'open' ? 'success' : 'default'" round>
            {{ f.status === 'open' ? $t('elevator.open') : $t('elevator.limit') }}
          </van-tag>
          <van-switch disabled
            :model-value="f.status === 'open'"
            size="20"
           
          />
        </template>
      </van-cell>
      <van-cell :title="$t('elevator.addFloor')" is-link icon="plus" />
    </van-cell-group>
    <div class="actions-row">
      <van-button disabled size="small" round type="primary">{{ $t('elevator.submitFloor') }}</van-button>
    </div>

    <!-- 定时梯控 -->
    <van-cell-group inset :title="$t('elevator.scheduleTitle', { n: floorSchedule.schedules.length })">
      <van-cell v-for="(s, i) in floorSchedule.schedules" :key="s.hash || i" :title="scheduleTitle(s)">
        <template #right-icon>
          <van-icon name="cross" />
        </template>
      </van-cell>
      <van-cell :title="$t('elevator.addSchedule')" is-link icon="plus" />
    </van-cell-group>
    <div class="actions-row">
      <van-button disabled size="small" round type="primary">{{ $t('elevator.saveDispatch') }}</van-button>
    </div>

    <!-- 特殊模式 -->
    <van-cell-group inset :title="$t('elevator.specialMode')">
      <van-cell :title="$t('elevator.masterSwitch')" center>
        <template #right-icon><van-switch disabled v-model="mode.masterSwitch" size="20" /></template>
      </van-cell>
      <van-cell
        v-for="m in modeItems"
        :key="m.key"
        :title="$t(m.labelKey)"
        :label="$t(m.descKey)"
        center
      >
        <template #right-icon>
          <van-switch disabled
            v-model="mode[m.key]"
            size="20"
            :disabled="!mode.masterSwitch"
           
          />
        </template>
      </van-cell>
    </van-cell-group>

    <!-- 屏幕休眠 -->
    <van-cell-group inset :title="$t('elevator.screenSleep')">
      <van-cell :title="$t('elevator.sleepRule')" :label="$t('elevator.sleepTip')" center>
        <template #right-icon>
          <van-switch disabled v-model="sleep.enable" size="20" />
        </template>
      </van-cell>
    </van-cell-group>

    <!-- 时间选择 -->
    <van-popup v-model:show="showTime" position="bottom" round>
      <div class="time-pop">
        <div class="time-title">{{ timeTarget === 'onTime' ? $t('elevator.onTime') : $t('elevator.offTime') }}</div>
        <van-time-picker disabled v-model="timeValue" title="" :columns-type="['hour', 'minute']" />
        <van-button disabled type="primary" block round>{{ $t('common.confirm') }}</van-button>
      </div>
    </van-popup>

    <!-- 添加楼层 -->
    <van-popup v-model:show="showFloorAdd" position="bottom" round>
      <div class="floor-pop">
        <div class="time-title">{{ $t('elevator.addFloor') }}</div>
        <van-stepper disabled v-model="floorAddNum" integer min="-8" max="120" />
        <div class="err" v-if="floorAddNum === 0">{{ $t('elevator.floorCannotZero') }}</div>
        <div class="label">{{ floorLabel(floorAddNum) }}</div>
        <van-button disabled type="primary" block round>{{ $t('elevator.confirmOpen') }}</van-button>
      </div>
    </van-popup>

    <!-- 添加定时规则 -->
    <van-popup v-model:show="showScheduleAdd" position="bottom" round>
      <div class="schedule-pop">
        <div class="time-title">{{ $t('elevator.addSchedule') }}</div>
        <div class="schedule-body">
          <div class="row">
            <span class="row-label">{{ $t('elevator.action') }}</span>
            <van-radio-group disabled v-model="scheduleForm.action" direction="horizontal">
              <van-radio :name="1">{{ $t('elevator.open') }}</van-radio>
              <van-radio :name="0">{{ $t('elevator.limit') }}</van-radio>
            </van-radio-group>
          </div>
          <van-field disabled v-model="scheduleForm.time" :label="$t('elevator.time')" placeholder="HH:mm" />
          <div class="row">
            <span class="row-label">{{ $t('elevator.weekday') }}</span>
            <van-checkbox-group disabled v-model="scheduleForm.weekDays" direction="horizontal">
              <van-checkbox v-for="(d, idx) in weekOptions" :key="idx" :name="d.value" shape="square">
                {{ $t(d.labelKey) }}
              </van-checkbox>
            </van-checkbox-group>
          </div>
          <van-field disabled v-model="scheduleForm.floorText" :label="$t('elevator.floor')" :placeholder="$t('elevator.floorPlaceholder')" />
        </div>
        <van-button disabled type="primary" block round>{{ $t('common.confirm') }}</van-button>
      </div>
    </van-popup>
  </UnsupportedPanel>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'

const { t } = useI18n()

const showTime = ref(false)
const timeTarget = ref('onTime')
const timeValue = ref(['08', '00'])

const showFloorAdd = ref(false)
const floorAddNum = ref(1)

const showScheduleAdd = ref(false)
const scheduleForm = reactive({ action: 1, time: '08:00', weekDays: [1], floorText: '3' })
// 星期选项：存 i18n 键，模板 t() 渲染
const weekOptions = computed(() => [
  { value: 1, labelKey: 'common.weekShort.mon' }, { value: 2, labelKey: 'common.weekShort.tue' },
  { value: 3, labelKey: 'common.weekShort.wed' }, { value: 4, labelKey: 'common.weekShort.thu' },
  { value: 5, labelKey: 'common.weekShort.fri' }, { value: 6, labelKey: 'common.weekShort.sat' },
  { value: 7, labelKey: 'common.weekShort.sun' }
])

const board = reactive({ onTime: '08:00', offTime: '20:00' })
const floorLimit = reactive({ floors: [] })
const floorSchedule = reactive({ schedules: [] })
const mode = reactive({ masterSwitch: false, delay: false, parking: false, driver: false, independent: false, ext1: false, ext2: false, ext3: false, ext4: false })
const sleep = reactive({ enable: true })

// 特殊模式项：label/desc 均存 i18n 键
const modeItems = computed(() => [
  { key: 'delay', labelKey: 'elevator.delay', descKey: 'elevator.delayDesc' },
  { key: 'parking', labelKey: 'elevator.parking', descKey: 'elevator.parkingDesc' },
  { key: 'driver', labelKey: 'elevator.driver', descKey: 'elevator.driverDesc' },
  { key: 'independent', labelKey: 'elevator.vip', descKey: 'elevator.vipDesc' },
  { key: 'ext1', labelKey: 'elevator.ext1', descKey: 'elevator.extDesc' },
  { key: 'ext2', labelKey: 'elevator.ext2', descKey: 'elevator.extDesc' },
  { key: 'ext3', labelKey: 'elevator.ext3', descKey: 'elevator.extDesc' },
  { key: 'ext4', labelKey: 'elevator.ext4', descKey: 'elevator.extDesc' }
])

/** 主板时间展示 */
function fmt() { /* 本版本仅保留控件，业务处理未接入。 */ }

/** 楼层显示：负楼层 B，正楼层 n 楼（参数插值，随语言切换） */
function floorLabel(f) {
  if (f < 0) return t('common.floorBasement', { n: Math.abs(f) })
  return t('common.floorN', { n: f })
}

/** 定时规则摘要：动作 + 时间 + 星期 + 楼层 */
function scheduleTitle(s) { /* 本版本仅保留控件，业务处理未接入。 */ }

async function load() { /* 本版本仅保留控件，业务处理未接入。 */ }

function openTime(target) { /* 本版本仅保留控件，业务处理未接入。 */ }

function confirmTime() { /* 本版本仅保留控件，业务处理未接入。 */ }

async function saveBoard() { /* 本版本仅保留控件，业务处理未接入。 */ }

async function doReboot() { /* 本版本仅保留控件，业务处理未接入。 */ }

function setFloorStatus(f, open) { /* 本版本仅保留控件，业务处理未接入。 */ }

async function saveFloorLimit() { /* 本版本仅保留控件，业务处理未接入。 */ }

function openFloorAdd() { /* 本版本仅保留控件，业务处理未接入。 */ }

function confirmFloorAdd() { /* 本版本仅保留控件，业务处理未接入。 */ }

function openScheduleAdd() { /* 本版本仅保留控件，业务处理未接入。 */ }

function confirmScheduleAdd() { /* 本版本仅保留控件，业务处理未接入。 */ }

function removeSchedule(i) { /* 本版本仅保留控件，业务处理未接入。 */ }

async function saveSchedule() { /* 本版本仅保留控件，业务处理未接入。 */ }

async function onModeChange() { /* 本版本仅保留控件，业务处理未接入。 */ }

async function onSleepChange() { /* 本版本仅保留控件，业务处理未接入。 */ }



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
.actions-row {
  display: flex;
  gap: 10px;
  padding: var(--gap) var(--page-pad);
}
.time-pop,
.floor-pop,
.schedule-pop {
  padding: var(--page-pad);
}
.time-title {
  font-size: 18px;
  font-weight: 600;
  text-align: center;
  margin-bottom: 16px;
}
.floor-pop {
  display: flex;
  flex-direction: column;
  gap: 16px;
  align-items: center;
}
.err {
  color: var(--danger);
  font-size: 12px;
}
.label {
  color: var(--text-2);
}
.schedule-body {
  margin-bottom: 16px;
}
.row {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 8px 16px;
}
.row-label {
  color: var(--text-2);
  font-size: 14px;
  flex-shrink: 0;
}
</style>
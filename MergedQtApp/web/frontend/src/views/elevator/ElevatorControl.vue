<!-- @file ElevatorControl.vue | 真实本机梯控状态；按实际能力开启操作。 -->
<template>
  <div class="page">
    <van-notice-bar v-if="loadError" :text="loadError" />
    <van-notice-bar text="楼层和模式读取设备保存的配置；发送状态以本地执行结果为准。" />
    <van-notice-bar text="定时规则与平台共用，按设备本地时间执行；定时开关机和信号休眠暂不可用。" />
    <div class="header">
      <div class="title">{{ $t('elevator.title') }}</div>
    </div>

    <!-- 主板电源控制 -->
    <van-cell-group inset :title="$t('elevator.boardPower')">
      <van-cell
        :title="$t('elevator.onTime')"
        :value="fmt().onTime"
        is-link
        :disabled="!capabilities.boardPower"
        @click="openTime('onTime')"
      />
      <van-cell
        :title="$t('elevator.offTime')"
        :value="fmt().offTime"
        is-link
        :disabled="!capabilities.boardPower"
        @click="openTime('offTime')"
      />
    </van-cell-group>
    <div class="actions-row">
      <van-button
        size="small"
        round
        type="primary"
        :disabled="!capabilities.boardPower || busy"
        @click="saveBoard"
        >{{ $t('elevator.submitBoard') }}</van-button
      >
      <van-button size="small" round plain type="danger" :disabled="busy" @click="doReboot">{{
        $t('elevator.rebootNow')
      }}</van-button>
    </div>

    <!-- 楼层开放 / 限行 -->
    <van-cell-group inset :title="$t('elevator.floorLimit')">
      <div class="floor-legend">{{ t('elevator.floorSelectionHint') }}</div>
      <div class="floor-grid">
        <van-checkbox
          v-for="f in floorLimit.floors"
          :key="f.floor"
          class="floor-choice"
          :model-value="f.status === 'open'"
          :aria-label="floorLabel(f.floor)"
          shape="round"
          icon-size="20"
          :disabled="!capabilities.elevatorWrite || busy"
          @update:model-value="(v) => setFloorStatus(f, v)"
          >{{ floorLabel(f.floor) }}</van-checkbox
        >
      </div>
      <van-cell
        :title="$t('elevator.addFloor')"
        is-link
        icon="plus"
        :disabled="!capabilities.elevatorWrite || busy"
        @click="openFloorAdd"
      />
    </van-cell-group>
    <div class="actions-row">
      <van-button
        size="small"
        round
        type="primary"
        :disabled="!capabilities.elevatorWrite || busy"
        @click="saveFloorLimit"
        >{{ $t('elevator.submitFloor') }}</van-button
      >
    </div>

    <!-- 定时梯控 -->
    <van-cell-group
      inset
      :title="$t('elevator.scheduleTitle', { n: floorSchedule.schedules.length })"
    >
      <van-cell
        v-for="(s, i) in floorSchedule.schedules"
        :key="s.hash || i"
        :title="scheduleTitle(s)"
      >
        <template #right-icon>
          <van-icon
            name="cross"
            :aria-disabled="!capabilities.floorScheduleWrite || busy"
            @click="removeSchedule(i)"
          />
        </template>
      </van-cell>
      <van-cell
        :title="$t('elevator.addSchedule')"
        is-link
        icon="plus"
        :disabled="!capabilities.floorScheduleWrite || busy"
        @click="openScheduleAdd"
      />
    </van-cell-group>
    <div class="actions-row">
      <van-button
        size="small"
        round
        type="primary"
        :disabled="!capabilities.floorScheduleWrite || busy"
        @click="saveSchedule"
        >{{ $t('elevator.saveDispatch') }}</van-button
      >
    </div>

    <!-- 特殊模式 -->
    <van-cell-group inset :title="$t('elevator.specialMode')">
      <van-cell :title="$t('elevator.masterSwitch')" center>
        <template #right-icon
          ><van-switch
            v-model="mode.masterSwitch"
            :disabled="!capabilities.elevatorWrite || busy"
            size="20"
            @update:model-value="onModeChange"
        /></template>
      </van-cell>
      <van-cell
        v-for="m in modeItems"
        :key="m.key"
        :title="$t(m.labelKey)"
        :label="$t(m.descKey)"
        center
      >
        <template #right-icon>
          <van-switch
            v-model="mode[m.key]"
            size="20"
            :disabled="!capabilities.elevatorWrite || busy || !mode.masterSwitch"
            @update:model-value="onModeChange"
          />
        </template>
      </van-cell>
    </van-cell-group>

    <!-- 屏幕休眠 -->
    <van-cell-group inset :title="$t('elevator.screenSleep')">
      <van-cell :title="$t('elevator.sleepRule')" :label="$t('elevator.sleepTip')" center>
        <template #right-icon>
          <van-switch
            v-model="sleep.enable"
            :disabled="!capabilities.screenSleep || busy"
            size="20"
            @update:model-value="onSleepChange"
          />
        </template>
      </van-cell>
    </van-cell-group>

    <!-- 时间选择 -->
    <van-popup v-model:show="showTime" position="bottom" round>
      <div class="time-pop">
        <div class="time-title">
          {{ $t(timeTarget === 'schedule' ? 'elevator.time' : timeTarget === 'onTime' ? 'elevator.onTime' : 'elevator.offTime') }}
        </div>
        <van-time-picker
          ref="timePicker"
          v-model="timeValue"
          :show-toolbar="false"
          :columns-type="['hour', 'minute']"
          @confirm="confirmTime"
        />
        <van-button type="primary" block round @click="timePicker?.confirm()">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-popup>

    <!-- 添加楼层 -->
    <van-popup v-model:show="showFloorAdd" position="bottom" round>
      <div class="floor-pop">
        <div class="time-title">{{ $t('elevator.addFloor') }}</div>
        <van-stepper v-model="floorAddNum" integer min="-8" max="120" />
        <div class="err" v-if="floorAddNum === 0">{{ $t('elevator.floorCannotZero') }}</div>
        <div class="label">{{ floorLabel(floorAddNum) }}</div>
        <van-button type="primary" block round @click="confirmFloorAdd">{{
          $t('elevator.confirmOpen')
        }}</van-button>
      </div>
    </van-popup>

    <!-- 添加定时规则 -->
    <van-popup v-model:show="showScheduleAdd" position="bottom" round>
      <div class="schedule-pop">
        <div class="time-title">{{ $t('elevator.addSchedule') }}</div>
        <div class="schedule-body">
          <div class="row">
            <span class="row-label">{{ $t('elevator.action') }}</span>
            <van-radio-group v-model="scheduleForm.action" direction="horizontal">
              <van-radio :name="1">{{ $t('elevator.open') }}</van-radio>
              <van-radio :name="0">{{ $t('elevator.limit') }}</van-radio>
            </van-radio-group>
          </div>
          <van-field
            :model-value="scheduleForm.time"
            :label="$t('elevator.time')"
            readonly
            is-link
            :disabled="!capabilities.floorScheduleWrite || busy"
            @click="openTime('schedule')"
          />
          <div class="row">
            <span class="row-label">{{ $t('elevator.weekday') }}</span>
            <van-checkbox-group v-model="scheduleForm.weekDays" direction="horizontal">
              <van-checkbox
                v-for="(d, idx) in weekOptions"
                :key="idx"
                :name="d.value"
                shape="square"
              >
                {{ $t(d.labelKey) }}
              </van-checkbox>
            </van-checkbox-group>
          </div>
          <van-field
            v-model="scheduleForm.floorText"
            :label="$t('elevator.floor')"
            :placeholder="$t('elevator.floorPlaceholder')"
          />
        </div>
        <van-button type="primary" block round @click="confirmScheduleAdd">{{
          $t('common.confirm')
        }}</van-button>
      </div>
    </van-popup>
  </div>
</template>

<script setup>
import { ref, reactive, computed, onMounted } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'

const { t } = useI18n()
const busy = ref(false)
const capabilities = ref({})
const loadError = ref('')

const showTime = ref(false)
const timePicker = ref(null)
const timeTarget = ref('onTime')
const timeValue = ref(['08', '00'])

const showFloorAdd = ref(false)
const floorAddNum = ref(1)

const showScheduleAdd = ref(false)
const scheduleForm = reactive({ action: 1, time: '08:00', weekDays: [1], floorText: '3' })
// 星期选项：存 i18n 键，模板 t() 渲染
const weekOptions = computed(() => [
  { value: 1, labelKey: 'common.weekShort.mon' },
  { value: 2, labelKey: 'common.weekShort.tue' },
  { value: 3, labelKey: 'common.weekShort.wed' },
  { value: 4, labelKey: 'common.weekShort.thu' },
  { value: 5, labelKey: 'common.weekShort.fri' },
  { value: 6, labelKey: 'common.weekShort.sat' },
  { value: 7, labelKey: 'common.weekShort.sun' },
])

const board = reactive({ onTime: '—', offTime: '—' })
const floorLimit = reactive({ floors: [] })
const floorSchedule = reactive({ schedules: [] })
const baseSchedules = ref([])
const mode = reactive({
  masterSwitch: false,
  delay: false,
  parking: false,
  driver: false,
  independent: false,
  ext1: false,
  ext2: false,
  ext3: false,
  ext4: false,
})
const sleep = reactive({ enable: false })

// 特殊模式项：label/desc 均存 i18n 键
const modeItems = computed(() => [
  { key: 'delay', labelKey: 'elevator.delay', descKey: 'elevator.delayDesc' },
  { key: 'parking', labelKey: 'elevator.parking', descKey: 'elevator.parkingDesc' },
  { key: 'driver', labelKey: 'elevator.driver', descKey: 'elevator.driverDesc' },
  { key: 'independent', labelKey: 'elevator.vip', descKey: 'elevator.vipDesc' },
  { key: 'ext1', labelKey: 'elevator.ext1', descKey: 'elevator.extDesc' },
  { key: 'ext2', labelKey: 'elevator.ext2', descKey: 'elevator.extDesc' },
  { key: 'ext3', labelKey: 'elevator.ext3', descKey: 'elevator.extDesc' },
  { key: 'ext4', labelKey: 'elevator.ext4', descKey: 'elevator.extDesc' },
])

/**
 * @brief 读取页面主板开关机时间草稿。
 * 主板时间展示
 * @returns {object} 页面草稿中的 onTime 和 offTime。
 */
function fmt() {
  return { onTime: board.onTime, offTime: board.offTime }
}

/**
 * @brief 按当前语言生成地上或地下楼层标签。
 * 楼层显示：负楼层 B，正楼层 n 楼（参数插值，随语言切换）
 * @param {number} f 业务楼层号，地下层为负数。
 * @returns {string} 按当前语言生成的楼层文案。
 */
function floorLabel(f) {
  if (f < 0) return t('common.floorBasement', { n: Math.abs(f) })
  return t('common.floorN', { n: f })
}

/**
 * @brief 将动作、时间、星期和楼层组合为定时规则摘要。
 * 定时规则摘要：动作 + 时间 + 星期 + 楼层
 * @param {object} s 包含动作、时间、星期和楼层的定时规则。
 * @returns {string} 用于定时规则列表及删除确认的摘要。
 */
function scheduleTitle(s) {
  const shortKeys = [
    'common.weekShort.mon',
    'common.weekShort.tue',
    'common.weekShort.wed',
    'common.weekShort.thu',
    'common.weekShort.fri',
    'common.weekShort.sat',
    'common.weekShort.sun',
  ]
  const days = (s.weekDays || []).map((d) => t(shortKeys[d - 1])).join(t('common.scheduleJoin'))
  const act = s.action === 1 ? t('elevator.open') : t('elevator.limit')
  const weeks = t('common.weekParen', { days })
  return t('elevator.scheduleSummary', {
    action: act,
    time: s.time,
    weeks,
    floors: (s.floors || []).join(','),
  })
}

/**
 * @brief 加载当前页面需要的实际设备资料和能力状态。
 */
async function load() {
  loadError.value = ''
  try {
    capabilities.value = await api.capabilities()
    const [floors, schedules, currentMode] = await Promise.all([
      api.floorLimitGet(),
      api.floorScheduleGet(),
      api.elevatorModeGet(),
    ])
    floorLimit.floors = floors.map((f) => ({
      floor: f.floor,
      status: f.status === 1 ? 'open' : 'limit',
    }))
    floorSchedule.schedules = schedules.deviceSchedules || []
    baseSchedules.value = JSON.parse(JSON.stringify(floorSchedule.schedules))
    Object.assign(mode, currentMode)
  } catch (error) {
    loadError.value = error.message
  }
}

/**
 * @brief 按能力打开滚动时间选择器，确认前仅修改独立的时间草稿。
 * @param {string} target 待编辑的时间字段或计划字段名称。
 */
function openTime(target) {
  if (busy.value) return
  if (target === 'schedule' ? !capabilities.value.floorScheduleWrite : !capabilities.value.boardPower) return
  timeTarget.value = target
  const value = target === 'schedule' ? scheduleForm.time : target === 'onTime' ? board.onTime : board.offTime
  timeValue.value = /^([01]\d|2[0-3]):[0-5]\d$/.test(value) ? value.split(':') : ['08', '00']
  showTime.value = true
}

/**
 * @brief 将当前选择的时间写回页面草稿。
 * @param {object} args 时间选择器确认事件，selectedValues 为两位小时和分钟字符串。
 * @param {string[]} args.selectedValues 滚动控件已确认的小时和分钟。
 */
function confirmTime({ selectedValues }) {
  const val = selectedValues.join(':')
  if (timeTarget.value === 'schedule') scheduleForm.time = val
  else if (timeTarget.value === 'onTime') board.onTime = val
  else board.offTime = val
  showTime.value = false
}

/**
 * @brief 提示定时开关机能力尚未接入，不伪造执行成功。
 */
async function saveBoard() {
  showToast('本设备尚未接入定时开关机')
}

/**
 * @brief 确认后提交本地重启请求，并显示受理结果。
 */
async function doReboot() {
  try {
    await showConfirmDialog({
      title: t('elevator.rebootConfirmTitle'),
      message: t('elevator.rebootConfirm'),
    })
    busy.value = true
    await api.linuxReboot()
    showToast(t('elevator.rebootSent'))
  } catch (error) {
    if (error?.message) showToast(error.message)
  } finally {
    busy.value = false
  }
}

/**
 * @brief 更新页面中指定楼层的开放草稿。
 * @param {object} f 页面中的楼层条目，将直接修改其 status 草稿。
 * @param {boolean} open 是否在页面草稿中将该楼层设为开放。
 */
function setFloorStatus(f, open) {
  f.status = open ? 'open' : 'limit'
}

/**
 * @brief 提交楼层草稿并刷新设备实际保存的状态。
 */
async function saveFloorLimit() {
  if (busy.value || !capabilities.value.elevatorWrite) return
  busy.value = true
  try {
    await api.floorLimitSet({ floors: floorLimit.floors })
    showToast('配置已保存，指令已发送')
  } catch (error) {
    showToast(error.message)
  } finally {
    busy.value = false
    await load()
  }
}

/**
 * @brief 在梯控可写时打开楼层选择面板。
 */
function openFloorAdd() {
  if (!capabilities.value.elevatorWrite || busy.value) return
  floorAddNum.value = 1
  showFloorAdd.value = true
}

/**
 * @brief 校验楼层范围与重复项后加入页面草稿。
 */
function confirmFloorAdd() {
  if (floorAddNum.value === 0) {
    showToast(t('elevator.floorCannotZero'))
    return
  }
  if (floorLimit.floors.some((f) => f.floor === floorAddNum.value)) {
    showToast(t('elevator.floorExists'))
    return
  }
  floorLimit.floors.push({ floor: floorAddNum.value, status: 'open' })
  floorLimit.floors.sort((a, b) => a.floor - b.floor)
  showFloorAdd.value = false
}

/**
 * @brief 仅在具备定时规则写入能力时打开新增面板。
 */
function openScheduleAdd() {
  if (!capabilities.value.floorScheduleWrite || busy.value) return
  scheduleForm.action = 1
  scheduleForm.time = '08:00'
  scheduleForm.weekDays = [1]
  scheduleForm.floorText = '3'
  showScheduleAdd.value = true
}

/**
 * @brief 校验时刻和楼层输入后记录页面中的定时规则草稿。
 */
function confirmScheduleAdd() {
  if (!capabilities.value.floorScheduleWrite || busy.value) return
  const floors = String(scheduleForm.floorText)
    .split(/[,，\s]+/)
    .map((s) => Number(s))
    .filter((n) => n !== 0 && Number.isInteger(n))
  if (!floors.length || floors.some((n) => n < -8 || n > 120) || !scheduleForm.weekDays.length) {
    showToast(t('elevator.enterValidFloor'))
    return
  }
  floorSchedule.schedules.push({
    action: scheduleForm.action,
    time: scheduleForm.time,
    weekDays: [...scheduleForm.weekDays],
    floors,
    hash: 'web-' + crypto.randomUUID(),
  })
  showScheduleAdd.value = false
}

/**
 * @brief 具备可写能力时移除当前页面的指定定时规则。
 * @param {number} i 待移除规则在页面数组中的索引。
 */
async function removeSchedule(i) {
  if (!capabilities.value.floorScheduleWrite || busy.value) return
  const rule = floorSchedule.schedules[i]
  if (!rule) return
  if (!baseSchedules.value.some((item) => item.hash === rule.hash)) {
    floorSchedule.schedules.splice(i, 1)
    return
  }
  try {
    await showConfirmDialog({ title: t('common.delete'), message: scheduleTitle(rule) })
    busy.value = true
    const result = await api.floorScheduleDelete({ hashList: [rule.hash], baseSchedules: baseSchedules.value })
    // 保留页面尚未保存的新增草稿，只移除已确认删除的规则。
    floorSchedule.schedules = floorSchedule.schedules.filter((item) => item.hash !== rule.hash)
    baseSchedules.value = result.deviceSchedules
    showToast(t('common.deleted'))
  } catch (error) {
    if (error?.message) showToast(error.message)
  } finally { busy.value = false }
}

/**
 * @brief 提交完整定时规则与读取快照，保存后同步页面中的规则和版本基线。
 */
async function saveSchedule() {
  if (!capabilities.value.floorScheduleWrite || busy.value) return
  busy.value = true
  try {
    const result = await api.floorScheduleSet({ schedules: floorSchedule.schedules, baseSchedules: baseSchedules.value })
    floorSchedule.schedules = result.deviceSchedules
    baseSchedules.value = JSON.parse(JSON.stringify(result.deviceSchedules))
    showToast(t('common.saved'))
  } catch (error) { showToast(error.message) }
  finally { busy.value = false }
}

/**
 * @brief 提交特殊模式开关，随后重新读取保存结果。
 */
async function onModeChange() {
  if (busy.value || !capabilities.value.elevatorWrite) return
  busy.value = true
  try {
    await api.elevatorModeSet({ ...mode })
    showToast('配置已保存，指令已发送')
  } catch (error) {
    showToast(error.message)
  } finally {
    busy.value = false
    await load()
  }
}

/**
 * @brief 提示信号休眠能力尚未接入。
 */
async function onSleepChange() {
  showToast('本设备尚未接入无信号休眠')
}

onMounted(load)
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
.floor-legend {
  padding: 12px 16px 4px;
  color: var(--text-3);
  font-size: 12px;
}
.floor-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(70px, 1fr));
  gap: 14px 8px;
  padding: 16px;
}
.floor-choice {
  min-height: 28px;
  font-size: 13px;
}
.floor-choice :deep(.van-checkbox__label) {
  margin-left: 6px;
  white-space: nowrap;
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

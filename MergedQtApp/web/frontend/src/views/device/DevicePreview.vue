<!--
 @file DevicePreview.vue
 @brief 设备基础标识展示；本机实时预览、截图与概览。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="page">
    <div class="header">
      <div class="actions">
        <LangSwitch :size="20" />
        <div class="chips">
          <span
            v-for="s in PREVIEW_INTERVALS"
            :key="s"
            class="chip"
            :class="{ active: s === interval }"
            @click="changeInterval(s)"
          >
            {{ s }}s
          </span>
        </div>
        <van-button size="small" round type="primary" :loading="refreshing" @click="refresh">{{
          $t('common.refresh')
        }}</van-button>
      </div>
    </div>

    <div class="info-card">
      <div class="info-name">{{ devInfo.deviceName || '--' }}</div>
      <div class="info-rows">
        <div class="info-row">
          <span class="info-label">{{ $t('login.deviceId') }}</span>
          <span class="info-value">{{ devInfo.deviceId || '--' }}</span>
        </div>
        <div class="info-row">
          <span class="info-label">{{ $t('preview.wifiHotspot') }}</span>
          <span class="info-value">{{ devInfo.wifiName || '--' }}</span>
        </div>
      </div>
    </div>

    <div class="overview-card">
      <div class="ov-head">
        <span class="ov-title">{{ $t('preview.overview') }}</span>
        <span class="ov-ip">{{ overview.deviceIp || '--' }}</span>
      </div>
      <div class="ov-grid">
        <div class="ov-item">
          <div class="ov-num">{{ overview.personCount ?? '--' }}</div>
          <div class="ov-label">{{ $t('preview.personCount') }}</div>
        </div>
        <div class="ov-item">
          <div class="ov-num">{{ overview.faceCount ?? '--' }}</div>
          <div class="ov-label">{{ $t('preview.face') }}</div>
        </div>
        <div class="ov-item">
          <div class="ov-num">{{ overview.icCount ?? '--' }}</div>
          <div class="ov-label">{{ $t('preview.icCard') }}</div>
        </div>
        <div class="ov-item">
          <div class="ov-num">{{ overview.qrCount ?? '--' }}</div>
          <div class="ov-label">{{ $t('preview.qrCode') }}</div>
        </div>
      </div>
      <div class="ov-storage">
        <div class="ov-storage-head">
          <span class="ov-label">{{ $t('preview.storage') }}</span>
          <span class="ov-storage-text"
            >{{ storageNumber(overview.storage?.used) }} /
            {{ storageNumber(overview.storage?.total) }} GiB</span
          >
        </div>
        <van-progress
          :percentage="overview.storage?.usedPercent ?? 0"
          :show-pivot="false"
          stroke-width="8"
          color="var(--primary)"
          track-color="#eef1f6"
        />
      </div>
    </div>

    <div class="screen-card">
      <img v-if="image" :src="image" class="screen" :alt="$t('preview.screenAlt')" />
      <van-empty v-else :description="$t('preview.noScreenshot')" />
    </div>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'
import { PREVIEW_INTERVALS } from '../../config'
import LangSwitch from '../../components/LangSwitch.vue'

const { t } = useI18n()
const image = ref('')
const interval = ref(5)
const overview = ref({})
const devInfo = ref({})
let timer = null
let stopped = false
const refreshing = ref(false)

/** @brief 容量只保留两位小数，缺失或非有限值继续显示未知。 */
function storageNumber(value) {
  return typeof value === 'number' && Number.isFinite(value) ? value.toFixed(2) : '--'
}

/** @brief 加载设备信息（设备名称/设备号/WIFI 热点） */
async function loadDeviceInfo() {
  try {
    devInfo.value = await api.getDeviceInfo()
  } catch (e) {
    // 设备信息失败不阻塞截图
  }
}

/** @brief 加载设备概览（IP/人员/人脸/IC/二维码/存储） */
async function loadOverview() {
  try {
    const data = await api.deviceOverview()
    if (!stopped) overview.value = data
  } catch (error) {
    if (!stopped) showToast(error.message)
  }
}

/** @brief 立即刷新一次截图 */
async function refresh() {
  if (stopped || refreshing.value) return
  refreshing.value = true
  try {
    const result = await api.screenshot()
    if (!stopped) image.value = result.base64Image || ''
  } catch (error) {
    if (!stopped) showToast(error.message || t('preview.screenshotFailed'))
  } finally {
    refreshing.value = false
  }
}

/** @brief 切换轮询间隔 */
function changeInterval(s) {
  interval.value = s
  restart()
}

/** @brief 按所选间隔刷新概览与当前界面截图。 */
function restart() {
  if (timer) clearInterval(timer)
  timer = window.setInterval(() => {
    refresh()
    loadOverview()
  }, interval.value * 1000)
}

onMounted(() => {
  loadDeviceInfo()
  loadOverview()
  refresh()
  restart()
})
onBeforeUnmount(() => {
  stopped = true
  if (timer) clearInterval(timer)
})
</script>

<style scoped>
.page {
  padding: var(--page-pad);
}
.header {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  margin-bottom: var(--gap);
}
.actions {
  display: flex;
  align-items: center;
  gap: 10px;
}
.chips {
  display: flex;
  gap: 6px;
}
.chip {
  padding: 4px 10px;
  font-size: 12px;
  color: var(--text-2);
  background: var(--card);
  border-radius: var(--radius-pill);
}
.chip.active {
  color: #fff;
  background: var(--primary);
}
.info-card {
  background: linear-gradient(135deg, #00b368, #00c76e);
  border-radius: var(--radius-panel);
  box-shadow: var(--shadow-card);
  padding: 18px 16px;
  margin-bottom: var(--gap);
}
.info-name {
  font-size: 24px;
  font-weight: 700;
  color: #fff;
  letter-spacing: 0.5px;
}
.info-rows {
  margin-top: 12px;
  display: flex;
  flex-direction: column;
  gap: 6px;
}
.info-row {
  display: flex;
  align-items: center;
  gap: 10px;
}
.info-label {
  font-size: 12px;
  color: rgba(255, 255, 255, 0.8);
  min-width: 56px;
}
.info-value {
  font-size: 14px;
  font-weight: 600;
  color: #fff;
  background: rgba(255, 255, 255, 0.18);
  padding: 3px 12px;
  border-radius: var(--radius-pill);
}
.overview-card {
  background: var(--card);
  border-radius: var(--radius-panel);
  box-shadow: var(--shadow-card);
  padding: 14px 16px;
  margin-bottom: var(--gap);
}
.ov-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 12px;
}
.ov-title {
  font-size: 15px;
  font-weight: 600;
}
.ov-ip {
  font-size: 12px;
  color: var(--text-2);
  background: #eef1f6;
  padding: 3px 10px;
  border-radius: var(--radius-pill);
}
.ov-grid {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 8px;
  margin-bottom: 14px;
}
.ov-item {
  background: #f7f9fc;
  border-radius: var(--radius-sm);
  padding: 12px 4px;
  text-align: center;
}
.ov-num {
  font-size: 20px;
  font-weight: 600;
  line-height: 1.2;
}
.ov-label {
  font-size: 12px;
  color: var(--text-3);
  margin-top: 4px;
}
.ov-storage-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 6px;
}
.ov-storage-text {
  font-size: 12px;
  color: var(--text-2);
}
.screen-card {
  background: var(--card);
  border-radius: var(--radius-panel);
  box-shadow: var(--shadow-card);
  padding: 10px;
}
.screen {
  width: 100%;
  border-radius: var(--radius-sm);
  display: block;
  aspect-ratio: 4 / 3;
  object-fit: cover;
  background: #1f1f1f;
}
</style>

<!--
 @file DevicePreview.vue
 @brief 设备基础标识展示；实时预览、截图和轮询未接入。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="page">
    <div class="header">
      <div class="title">{{ $t('preview.title') }}</div>
      <div class="actions">
        <LangSwitch :size="20" />
        <div class="chips">
          <span
            v-for="s in PREVIEW_INTERVALS"
            :key="s"
            class="chip"
            :class="{ active: s === interval }"
            aria-disabled="true"
          >
            {{ s }}s
          </span>
        </div>
        <van-button size="small" round type="primary" disabled>{{ $t('common.refresh') }}</van-button>
        <van-icon name="setting-o" class="settings" @click="goSettings" />
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

    <UnsupportedPanel>
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
          <span class="ov-storage-text">{{ overview.storage?.used ?? '--' }} / {{ overview.storage?.total ?? '--' }} GB</span>
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
  </UnsupportedPanel>
  </div>
</template>

<script setup>
import { ref, onMounted, onBeforeUnmount } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'
import { PREVIEW_INTERVALS } from '../../config'
import LangSwitch from '../../components/LangSwitch.vue'

const router = useRouter()
const { t } = useI18n()
const image = ref('')
const interval = ref(10)
const overview = ref({})
const devInfo = ref({})
let timer = null

/** 加载设备信息（设备名称/设备号/WIFI 热点） */
async function loadDeviceInfo() {
  try {
    devInfo.value = await api.getDeviceInfo()
  } catch (e) {
    // 设备信息失败不阻塞截图
  }
}

/** 加载设备概览（IP/人员/人脸/IC/二维码/存储） */
async function loadOverview() { /* 原控件保留，本版本不执行该业务。 */ }

/** 立即刷新一次截图 */
async function refresh() { /* 原控件保留，本版本不执行该业务。 */ }

/** 切换轮询间隔 */
function changeInterval(s) { /* 原控件保留，本版本不执行该业务。 */ }

function goSettings() {
  router.push('/device/settings')
}

function restart() { /* 原控件保留，本版本不执行该业务。 */ }

onMounted(loadDeviceInfo)



import UnsupportedPanel from '../../components/UnsupportedPanel.vue'
</script>

<style scoped>
.page {
  padding: var(--page-pad);
}
.header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: var(--gap);
}
.title {
  font-size: 20px;
  font-weight: 600;
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
.settings {
  font-size: 22px;
  color: var(--text-2);
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
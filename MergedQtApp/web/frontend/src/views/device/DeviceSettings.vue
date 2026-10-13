<!--
 @file DeviceSettings.vue
 @brief 保留设备设置界面，本版本所有修改控件禁用。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="page">
    <van-nav-bar :title="$t('settings.title')" left-arrow @click-left="router.back()" />
    <UnsupportedPanel>

    <van-cell-group inset :title="$t('settings.deviceInfo')">
      <van-field disabled :model-value="deviceId" :label="$t('settings.deviceId')" readonly />
      <van-field disabled v-model="form.deviceName" :label="$t('settings.deviceName')" :placeholder="$t('settings.deviceNamePlaceholder')" />
    </van-cell-group>

    <van-cell-group inset :title="$t('settings.wifi')">
      <div class="cell-tip">{{ $t('settings.wifiTip') }}</div>
      <van-field disabled v-model="form.wifiName" :label="$t('settings.hotspotName')" :placeholder="$t('settings.hotspotNamePlaceholder')" />
      <van-field disabled
        v-model="form.wifiPassword"
        :type="showWifiPwd ? 'text' : 'password'"
        :label="$t('settings.hotspotPassword')"
        :placeholder="$t('settings.hotspotPasswordPlaceholder')"
        :right-icon="showWifiPwd ? 'closed-eye' : 'eye-o'"
       
      />
    </van-cell-group>

    <van-cell-group inset :title="$t('settings.loginPassword')">
      <div class="cell-tip">{{ $t('settings.loginPasswordTip') }}</div>
      <van-field disabled v-model="pwd.oldPassword" type="password" :label="$t('settings.oldPassword')" :placeholder="$t('settings.oldPasswordPlaceholder')" />
      <van-field disabled v-model="pwd.newPassword" type="password" :label="$t('settings.newPassword')" :placeholder="$t('settings.newPasswordPlaceholder')" />
      <van-field disabled v-model="pwd.confirmPassword" type="password" :label="$t('settings.confirmPassword')" :placeholder="$t('settings.confirmPasswordPlaceholder')" />
    </van-cell-group>

    <van-cell-group inset :title="$t('settings.pkgImport')">
      <div class="cell-tip">{{ $t('settings.pkgTip') }}</div>
      <input disabled ref="pkgRef" type="file" accept=".zip,.tar,.gz" class="file-input" />
      <van-cell
        :title="pkgFile ? pkgFile.name : $t('settings.selectPkg')"
        :label="pkgFile ? formatPkgSize(pkgFile.size) : $t('settings.pkgHint')"
        icon="archive-o"
        is-link
       
      />
      <div v-if="pkgFile" class="pending-bar">
        <van-button disabled round block type="primary" :loading="pkgUploading">{{ $t('common.uploadToDevice') }}</van-button>
      </div>
    </van-cell-group>

    <div class="footer">
      <van-button disabled type="primary" block round :loading="saving">{{ $t('settings.saveSettings') }}</van-button>
    </div>
  </UnsupportedPanel>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'
import { useAppStore } from '../../store'

const router = useRouter()
const store = useAppStore()
const { t } = useI18n()

const deviceId = ref('')
const showWifiPwd = ref(false)
const saving = ref(false)
const form = reactive({ deviceName: '', wifiName: '', wifiPassword: '' })
const pwd = reactive({ oldPassword: '', newPassword: '', confirmPassword: '' })

const pkgRef = ref(null)
const pkgFile = ref(null)
const pkgUploading = ref(false)



async function save() { /* 本版本仅保留控件，业务处理未接入。 */ }

/** 选择配置压缩包 */
function pickPkg() { /* 本版本仅保留控件，业务处理未接入。 */ }

/** 校验并记录所选压缩包（文件名须含 ycest） */
function onPkgChange(e) { /* 本版本仅保留控件，业务处理未接入。 */ }

/** 格式化文件大小 */
function formatPkgSize(bytes) {
  if (!bytes && bytes !== 0) return '-'
  const mb = bytes / 1024 / 1024
  if (mb >= 1) return mb.toFixed(1) + 'MB'
  return Math.max(1, Math.round(bytes / 1024)) + 'KB'
}

/** 上传配置包到设备 */
async function onPkgUpload() { /* 本版本仅保留控件，业务处理未接入。 */ }

import UnsupportedPanel from '../../components/UnsupportedPanel.vue'
</script>

<style scoped>
.page {
  padding-bottom: 80px;
}
.cell-tip {
  padding: 8px 16px;
  font-size: 12px;
  color: var(--text-3);
}
.file-input {
  display: none;
}
.pending-bar {
  padding: var(--page-pad);
}
.footer {
  padding: var(--page-pad);
}
</style>
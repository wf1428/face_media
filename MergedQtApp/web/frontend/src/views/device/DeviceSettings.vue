<!--
 @file DeviceSettings.vue
 @brief 实际设备信息、热点名称与本地网页登录密码设置。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="page">
    <van-nav-bar :title="$t('settings.title')" left-arrow @click-left="router.back()" />
    <van-cell-group inset class="settings-options">
      <van-cell
        :title="t('network.title')"
        icon="exchange"
        is-link
        @click="router.push('/home/network')"
      />
    </van-cell-group>
    <van-cell-group inset :title="$t('settings.deviceInfo')">
      <van-field :model-value="deviceId || '—'" :label="$t('settings.deviceId')" readonly />
      <van-field
        readonly
        :model-value="form.deviceName || '—'"
        :label="$t('settings.deviceName')"
        :placeholder="$t('settings.deviceNamePlaceholder')"
      />
    </van-cell-group>

    <van-cell-group inset :title="$t('settings.wifi')">
      <div class="cell-tip">{{ $t('settings.wifiTip') }}</div>
      <van-field
        :disabled="!loaded || saving || restartPending"
        v-model="form.wifiName"
        :label="$t('settings.hotspotName')"
        :placeholder="$t('settings.hotspotNamePlaceholder')"
      />
      <van-field
        :disabled="!loaded || saving || restartPending"
        v-model="form.wifiPassword"
        :type="showWifiPwd ? 'text' : 'password'"
        :label="$t('settings.hotspotPassword')"
        :placeholder="$t('settings.hotspotPasswordPlaceholder')"
        :right-icon="showWifiPwd ? 'closed-eye' : 'eye-o'"
        name="hotspot-new-password"
        autocomplete="new-password"
        @click-right-icon="showWifiPwd = !showWifiPwd"
      />
      <div class="cell-tip">{{ t('settings.hotspotPasswordHint') }}</div>
    </van-cell-group>

    <div class="cell-tip" role="status">{{ $t('settings.wifiRestartWarning') }}</div>
    <div v-if="restartPending" class="restart-notice" role="status">
      {{ $t('settings.wifiSavedReconnect', { ssid: form.deviceName }) }}
    </div>
    <div class="footer">
      <van-button
        type="primary"
        block
        round
        :loading="saving"
        :disabled="!loaded || restartPending"
        @click="save"
      >
        {{ $t('settings.saveSettings') }}
      </van-button>
    </div>

    <van-cell-group inset :title="$t('settings.loginPassword')">
      <div class="cell-tip">{{ $t('settings.loginPasswordTip') }}</div>
      <van-field
        :disabled="changingPassword"
        v-model="pwd.oldPassword"
        type="password"
        :label="$t('settings.oldPassword')"
        :placeholder="$t('settings.oldPasswordPlaceholder')"
      />
      <van-field
        :disabled="changingPassword"
        v-model="pwd.newPassword"
        type="password"
        :label="$t('settings.newPassword')"
        placeholder="8–128"
      />
      <van-field
        :disabled="changingPassword"
        v-model="pwd.confirmPassword"
        type="password"
        :label="$t('settings.confirmPassword')"
        :placeholder="$t('settings.confirmPasswordPlaceholder')"
      />
    </van-cell-group>
    <div class="footer">
      <van-button type="primary" block round :loading="changingPassword" @click="changePassword">
        修改登录密码
      </van-button>
    </div>

    <van-cell-group inset :title="$t('settings.pkgImport')">
      <input
        ref="pkgRef"
        type="file"
        accept=".zip,.tar,.gz,.tgz,.rar"
        class="file-input"
        :disabled="pkgUploading || Boolean(pkgTaskId)"
        @change="onPkgChange"
      />
      <van-cell
        :title="pkgFile ? pkgFile.name : $t('settings.selectPkg')"
        :label="pkgFile ? formatPkgSize(pkgFile.size) : $t('settings.pkgHint')"
        icon="description"
        is-link
        :disabled="pkgUploading || Boolean(pkgTaskId)"
        @click="pickPkg"
      />
      <div v-if="pkgFile" class="pending-bar">
        <van-button
          round
          block
          type="primary"
          :disabled="Boolean(pkgTaskId)"
          :loading="pkgUploading"
          @click="onPkgUpload"
          >{{ $t('common.uploadToDevice') }}</van-button
        >
      </div>
      <div v-if="pkgUploading" class="pending-bar">
        <van-progress :percentage="pkgProgress" />
      </div>
      <div v-if="pkgMessage" class="cell-tip" role="status">{{ pkgMessage }}</div>
      <div v-if="pkgTaskId && !pkgUploading" class="pending-bar">
        <van-button block @click="resumePkg">{{ t('settings.pkgQuery') }}</van-button>
        <van-button block @click="cancelPkg">{{ t('settings.pkgCancel') }}</van-button>
      </div>
    </van-cell-group>
  </div>
</template>

<script setup>
import { ref, reactive, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast, showConfirmDialog } from 'vant'
import { api } from '../../api'
import { useAppStore } from '../../store'

const router = useRouter()
const store = useAppStore()
const { t } = useI18n()

const deviceId = ref('')
const showWifiPwd = ref(false)
const saving = ref(false)
const loaded = ref(false)
const restartPending = ref(false)
const form = reactive({ deviceName: '', wifiName: '', wifiPassword: '' })
const pwd = reactive({ oldPassword: '', newPassword: '', confirmPassword: '' })
const changingPassword = ref(false)

/** @brief 验证原登录密码并更新当前账号，成功后要求重新登录。 */
async function changePassword() {
  if (!pwd.oldPassword || pwd.newPassword.length < 8 || pwd.newPassword.length > 128) {
    showToast('请输入原密码，新密码需为8至128个字符')
    return
  }
  if (pwd.newPassword !== pwd.confirmPassword) {
    showToast('两次输入的新密码不一致')
    return
  }
  changingPassword.value = true
  try {
    await api.changePassword({ oldPassword: pwd.oldPassword, newPassword: pwd.newPassword })
    store.clear()
    showToast('密码已修改，请重新登录')
    await router.replace('/login')
  } catch (error) {
    showToast(error.message || '修改密码失败')
  } finally {
    changingPassword.value = false
    pwd.oldPassword = pwd.newPassword = pwd.confirmPassword = ''
  }
}

const pkgRef = ref(null)
const pkgFile = ref(null)
const pkgUploading = ref(false)
const pkgProgress = ref(0)
const pkgMessage = ref('')
const pkgTaskId = ref(sessionStorage.getItem('ycest-resource-task') || '')

onMounted(async () => {
  if (pkgTaskId.value) resumePkg()
  try {
    const info = await api.getDeviceInfo()
    deviceId.value = info.deviceId
    form.deviceName = info.wifiName
    form.wifiName = info.wifiName
    restartPending.value = Boolean(info.restartPending)
    store.setDeviceInfo(info)
    loaded.value = true
    if (info.wifiUpdateError) showToast(info.wifiUpdateError)
  } catch (error) {
    showToast(error.message || t('settings.readFailed'))
  }
})

/** @brief 保存热点名称和可选新密码，提示重激活后使用新设置重新连接。 */
async function save() {
  const ssid = form.wifiName
  if (
    !ssid ||
    ssid !== ssid.trim() ||
    new TextEncoder().encode(ssid).length > 32 ||
    /[\u0000-\u001f\u007f]/.test(ssid)
  ) {
    showToast(t('settings.wifiInvalid'))
    return
  }
  const password = form.wifiPassword
  if (password && !/^[\x20-\x7e]{8,63}$/.test(password) && !/^[0-9a-fA-F]{64}$/.test(password)) {
    showToast(t('settings.hotspotPasswordInvalid'))
    return
  }
  if (ssid === form.deviceName && !password) {
    showToast(t('common.saved'))
    return
  }
  try {
    await showConfirmDialog({
      title: t('settings.wifi'),
      message: t('settings.wifiRestartWarning'),
      confirmButtonText: t('common.confirm'),
      cancelButtonText: t('common.cancel'),
    })
  } catch {
    return
  }
  saving.value = true
  try {
    const info = await api.saveWifiSsid(ssid, password)
    form.deviceName = info.wifiName
    form.wifiName = info.wifiName
    form.wifiPassword = ''
    store.setDeviceInfo(info)
    restartPending.value = Boolean(info.restartPending)
    showToast({
      message: restartPending.value
        ? t('settings.wifiSavedReconnect', { ssid: info.wifiName })
        : t('common.saved'),
      duration: 3000,
    })
  } catch (error) {
    showToast(error.message || t('common.saveFailed'))
  } finally {
    saving.value = false
  }
}

/** @brief 选择配置压缩包 */
function pickPkg() {
  if (!pkgUploading.value && !pkgTaskId.value) pkgRef.value?.click()
}

/** @brief 校验压缩包类型及大小；内部 Ycest 目录由设备解压时校验。 */
function onPkgChange(e) {
  const file = e.target.files?.[0]
  e.target.value = ''
  if (!file) return
  if (
    !/\.(zip|tar|gz|tgz|rar)$/i.test(file.name) ||
    file.size <= 0 ||
    file.size > 512 * 1024 * 1024
  ) {
    showToast(t('settings.pkgInvalid'))
    return
  }
  pkgFile.value = file
  pkgMessage.value = ''
}

/** @brief 格式化文件大小 */
function formatPkgSize(bytes) {
  if (!bytes && bytes !== 0) return '-'
  const mb = bytes / 1024 / 1024
  if (mb >= 1) return mb.toFixed(1) + 'MB'
  return Math.max(1, Math.round(bytes / 1024)) + 'KB'
}

/** @brief 传输和解压阶段分别展示；只在目录替换完成后报告成功。 */
async function onPkgUpload() {
  if (!pkgFile.value || pkgUploading.value || pkgTaskId.value) return
  try {
    await showConfirmDialog({ title: t('settings.pkgImport'), message: t('settings.pkgConfirm') })
  } catch {
    return
  }
  pkgUploading.value = true
  pkgProgress.value = 0
  try {
    const result = await api.uploadResourcePackage(pkgFile.value, packageProgress)
    packageDone(result)
  } catch (error) {
    pkgTaskId.value = sessionStorage.getItem('ycest-resource-task') || ''
    pkgMessage.value = error.message || t('common.uploadFailed')
    showToast(pkgMessage.value)
  } finally {
    pkgUploading.value = false
  }
}

/** @brief 显示上传字节进度，99% 阶段等待设备解压和替换。 */
function packageProgress(event) {
  pkgTaskId.value = event.id
  pkgProgress.value = event.percent
  pkgMessage.value =
    event.state === 'RECEIVING' ? t('settings.pkgUploading') : t('settings.pkgApplying')
}

/** @brief 处理设备返回的已替换目录清单，并释放前端任务预约。 */
function packageDone(result) {
  if (result.state !== 'READY') throw new Error(t('settings.pkgIncomplete'))
  pkgMessage.value = t('settings.pkgDone')
  pkgMessage.value +=
    ' ' + (result.restartScheduled ? t('settings.pkgRestarting') : t('settings.pkgRestartFailed'))
  pkgFile.value = null
  pkgTaskId.value = ''
  sessionStorage.removeItem('ycest-resource-task')
  showToast(pkgMessage.value)
}

/** @brief 刷新或请求中断后查询原任务，避免重复上传和重复替换。 */
async function resumePkg() {
  if (!pkgTaskId.value || pkgUploading.value) return
  pkgUploading.value = true
  try {
    packageDone(await api.waitResourcePackage(pkgTaskId.value, packageProgress))
  } catch (error) {
    pkgMessage.value = error.message
  } finally {
    pkgUploading.value = false
  }
}

/** @brief 仅取消允许取消的任务；目录替换期间由服务端拒绝取消。 */
async function cancelPkg() {
  try {
    await api.cancelUpload(pkgTaskId.value)
    pkgTaskId.value = ''
    pkgFile.value = null
    sessionStorage.removeItem('ycest-resource-task')
    pkgMessage.value = ''
  } catch (error) {
    showToast(error.message)
  }
}
</script>

<style scoped>
.page {
  padding-bottom: 80px;
}
.settings-options {
  margin-top: 16px;
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
.restart-notice {
  margin: var(--page-pad);
  padding: 12px;
  color: var(--primary);
  background: #eef6ff;
  border-radius: 8px;
  overflow-wrap: anywhere;
}
</style>

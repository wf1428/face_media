<!--
 @file Login.vue
 @brief 出厂用户名及密码登录，以服务端 Cookie 维护会话。
 @author Du lin
 @date 2026-10-09
-->
<template>
  <div class="login">
    <div class="lang-entry">
      <LangSwitch :size="22" />
    </div>

    <div class="brand">
      <div class="brand-name">{{ $t('login.title') }}</div>
      <div class="brand-sub">{{ deviceName || (deviceId ? $t('login.deviceX', { id: deviceId }) : $t('login.offlineDevice')) }}</div>
    </div>

    <div class="card">
      <van-cell-group :border="false">
        <van-field :label="$t('login.deviceId')" :model-value="deviceId || '—'" readonly />
        <van-field v-if="deviceName" :label="$t('login.deviceName')" :model-value="deviceName" readonly />
        <van-field v-model="username" label="用户名" placeholder="输入出厂设置的网页用户名" autocomplete="username" clearable />
        <van-field
          v-model="password"
          type="password"
          :label="$t('login.password')"
          :placeholder="$t('login.passwordPlaceholder')"
          clearable
        />
      </van-cell-group>
    </div>

    <van-button
      class="submit"
      type="primary"
      block
      round
      :loading="loading"
      @click="onLogin"
    >
      {{ $t('login.enter') }}
    </van-button>

  </div>
</template>

<script setup>
import { ref, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'
import { useAppStore } from '../../store'
import LangSwitch from '../../components/LangSwitch.vue'

const router = useRouter()
const store = useAppStore()
const { t } = useI18n()

const deviceId = ref('')
const deviceName = ref('')
const password = ref('')
const username = ref('')
const loading = ref(false)

onMounted(async () => {
  try {
    const info = await api.getDeviceInfo()
    deviceId.value = info.deviceId
    deviceName.value = info.deviceName
    store.setDeviceInfo(info)
  } catch (e) {
    // 获取设备信息失败不阻塞登录
  }
})

async function onLogin() {
  if (!password.value) {
    showToast(t('login.enterPassword'))
    return
  }
  loading.value = true
  try {
    const info = await api.verifyPassword({ username: username.value.trim(), password: password.value })
    store.setSession(info)
    password.value = ''
    showToast({ type: 'success', message: t('login.loginSuccess') })
    router.replace('/home/preview')
  } catch (e) {
    showToast({ type: 'fail', message: e.message || t('login.loginFailed') })
  } finally {
    loading.value = false
  }
}
</script>

<style scoped>
.login {
  min-height: 100vh;
  padding: 0 var(--page-pad);
  display: flex;
  flex-direction: column;
}
.lang-entry {
  position: absolute;
  top: 12px;
  right: 12px;
}
.brand {
  text-align: center;
  padding: 96px 0 40px;
}
.brand-name {
  font-size: 28px;
  font-weight: 600;
  color: var(--text-1);
}
.brand-sub {
  margin-top: 8px;
  font-size: 14px;
  color: var(--text-3);
}
.card {
  background: var(--card);
  border-radius: var(--radius-panel);
  box-shadow: var(--shadow-card);
  overflow: hidden;
}
.submit {
  margin-top: 24px;
}
.mock-tip {
  margin-top: 20px;
  text-align: center;
  font-size: 12px;
  color: var(--accent);
}
</style>

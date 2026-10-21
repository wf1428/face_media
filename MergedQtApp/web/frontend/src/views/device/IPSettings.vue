<!--
 @file IPSettings.vue
 @brief 有线网络 DHCP/静态设置，刷新以设备 net_cfg.ini 保存值为准。
 @author Du lin
 @date 2026-10-20
-->

<template>
  <div class="page">
    <van-nav-bar
      :title="t('network.title')"
      left-arrow
      @click-left="router.replace('/home/device')"
    >
      <template #right>
        <van-button size="small" plain :disabled="loading || saving" @click="load">
          {{ t('common.refresh') }}
        </van-button>
      </template>
    </van-nav-bar>
    <div class="tip">{{ t('network.tip') }}</div>
    <van-notice-bar v-if="readError" :text="readError" />
    <van-cell-group inset>
      <van-field :model-value="form.iface || '—'" :label="t('network.iface')" readonly />
      <div class="mode-row">
        <div>{{ t('network.mode') }}</div>
        <van-radio-group v-model="form.dhcp" direction="horizontal" :disabled="!loaded || saving">
          <van-radio :name="true" :aria-label="t('network.dhcp')">{{
            t('network.dhcp')
          }}</van-radio>
          <van-radio :name="false" :aria-label="t('network.static')">{{
            t('network.static')
          }}</van-radio>
        </van-radio-group>
      </div>
      <van-field
        v-model="form.ip"
        :label="t('network.ip')"
        :placeholder="form.dhcp ? '—' : '192.168.1.100'"
        :disabled="!loaded || saving || form.dhcp"
        autocomplete="off"
        name="network-ip"
      />
      <van-field
        v-model="form.netmask"
        :label="t('network.netmask')"
        :placeholder="form.dhcp ? '—' : '255.255.255.0'"
        :disabled="!loaded || saving || form.dhcp"
        autocomplete="off"
        name="network-netmask"
      />
      <van-field
        v-model="form.gateway"
        :label="t('network.gateway')"
        :placeholder="form.dhcp ? '—' : t('common.optional')"
        :disabled="!loaded || saving || form.dhcp"
        autocomplete="off"
        name="network-gateway"
      />
      <van-field
        v-model="form.dns"
        :label="t('network.dns')"
        :placeholder="form.dhcp ? '—' : t('network.dnsHint')"
        :disabled="!loaded || saving || form.dhcp"
        autocomplete="off"
        name="network-dns"
      />
    </van-cell-group>
    <div class="tip">{{ form.dhcp ? t('network.dhcpHint') : t('network.staticHint') }}</div>
    <div v-if="status" class="status" role="status">{{ status }}</div>
    <div class="footer">
      <van-button
        type="primary"
        block
        round
        :loading="saving"
        :disabled="!loaded || loading"
        @click="save"
        >{{ t('network.apply') }}</van-button
      >
    </div>
  </div>
</template>

<script setup>
import { reactive, ref, onMounted } from 'vue'
import { useRouter } from 'vue-router'
import { useI18n } from 'vue-i18n'
import { showToast } from 'vant'
import { api } from '../../api'

const router = useRouter()
const { t } = useI18n()
const form = reactive({ iface: '', dhcp: true, ip: '', netmask: '', gateway: '', dns: '' })
const loaded = ref(false)
const loading = ref(false)
const saving = ref(false)
const status = ref('')
const readError = ref('')

/** @brief 进入、刷新及保存后都从设备配置重新读取，不使用浏览器缓存值。 */
async function load() {
  loading.value = true
  if (!saving.value) status.value = ''
  readError.value = ''
  try {
    const config = await api.getNetworkConfig()
    Object.assign(form, config)
    loaded.value = true
  } catch (error) {
    loaded.value = false
    readError.value = error.message || t('network.readFailed')
  } finally {
    loading.value = false
  }
}

/** @brief 严格解析点分十进制 IPv4，避免无效输入进入设备应用流程。 */
function ipv4(value) {
  if (!/^(0|[1-9]\d{0,2})(\.(0|[1-9]\d{0,2})){3}$/.test(value)) return null
  const parts = value.split('.').map(Number)
  return parts.every((n) => n <= 255)
    ? parts.reduce((number, n) => ((number << 8) | n) >>> 0, 0)
    : null
}

/** @brief 先校验静态字段，再提交异步任务；只有实际应用结果才显示成功。 */
async function save() {
  if (!loaded.value || saving.value) return
  const data = { dhcp: form.dhcp }
  if (!form.dhcp) {
    for (const field of ['ip', 'netmask', 'gateway', 'dns']) data[field] = form[field].trim()
    const ip = ipv4(data.ip)
    const mask = ipv4(data.netmask)
    const inverse = mask == null ? 0 : ~mask >>> 0
    if (
      ip == null ||
      mask == null ||
      mask === 0 ||
      (inverse & (inverse + 1)) !== 0 ||
      (data.gateway && ipv4(data.gateway) == null) ||
      data.dns
        .split(/[ ,]+/)
        .filter(Boolean)
        .some((server) => ipv4(server) == null)
    ) {
      showToast(t('network.invalid'))
      return
    }
  }
  saving.value = true
  status.value = t('network.applying')
  try {
    const result = await api.saveNetworkConfig(data)
    if (!result.saved) throw new Error(t('network.saveFailed'))
    await load()
    status.value = result.waitingForDhcp ? t('network.waiting') : t('network.saved')
    showToast(status.value)
  } catch (error) {
    status.value = error.message || t('network.saveFailed')
    showToast(status.value)
  } finally {
    saving.value = false
  }
}

onMounted(load)
</script>

<style scoped>
.page {
  padding-bottom: 80px;
}
.tip {
  padding: 12px 16px;
  color: var(--text-3);
  font-size: 13px;
  line-height: 1.6;
}
.mode-row {
  padding: 14px 16px;
  font-size: 14px;
}
.mode-row > div:first-child {
  margin-bottom: 12px;
}
.mode-row :deep(.van-radio-group) {
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
}
.footer {
  padding: var(--page-pad);
}
.status {
  margin: 16px;
  padding: 12px;
  border-radius: 8px;
  background: #eef6ff;
  font-size: 13px;
  line-height: 1.6;
}
</style>

<!--
 @file App.vue
 @brief 根页面与会话失效回到登录入口。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <router-view />
</template>

<script setup>
import { onMounted, onBeforeUnmount } from 'vue'
import { useRouter } from 'vue-router'
import { useAppStore } from './store'

const router = useRouter(),
  store = useAppStore()

/**
 * @brief 会话失效统一回登录页，停止旧请求。
 */
function expired() {
  store.clear()
  router.replace('/login')
}
onMounted(() => window.addEventListener('device-session-expired', expired))
onBeforeUnmount(() => window.removeEventListener('device-session-expired', expired))
// 应用根组件：仅承载路由出口，全局主题见 styles/theme.scss
</script>

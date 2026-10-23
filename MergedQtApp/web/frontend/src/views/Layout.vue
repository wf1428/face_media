<!--
 @file Layout.vue
 @brief 保留原页面布局及全部底部导航入口。
 @author Du lin
 @date 2026-10-09
-->

<template>
  <div class="layout">
    <div class="content">
      <router-view />
    </div>

    <van-tabbar v-model="active" @change="onChange">
      <van-tabbar-item name="preview" icon="photo-o">{{ $t('tab.preview') }}</van-tabbar-item>
      <van-tabbar-item name="person" icon="friends-o">{{ $t('tab.person') }}</van-tabbar-item>
      <van-tabbar-item name="elevator" icon="setting-o">{{ $t('tab.elevator') }}</van-tabbar-item>
      <van-tabbar-item name="air" icon="fire-o">{{ $t('tab.air') }}</van-tabbar-item>
      <van-tabbar-item name="video" icon="video-o">{{ $t('tab.video') }}</van-tabbar-item>
      <van-tabbar-item name="device" icon="setting-o">{{ $t('tab.device') }}</van-tabbar-item>
    </van-tabbar>
  </div>
</template>

<script setup>
import { ref, watch } from 'vue'
import { useRoute, useRouter } from 'vue-router'

const route = useRoute()
const router = useRouter()

const tabPath = {
  preview: '/home/preview',
  person: '/home/person',
  elevator: '/home/elevator',
  air: '/home/air',
  video: '/home/video',
  device: '/home/device',
}

// 路由名 -> Tab 名映射
const nameToTab = {
  DevicePreview: 'preview',
  PersonList: 'person',
  ElevatorControl: 'elevator',
  AirControl: 'air',
  VideoManage: 'video',
  DeviceSettings: 'device',
  IPSettings: 'device',
}

const active = ref(nameToTab[route.name] || 'preview')

watch(
  () => route.name,
  (n) => {
    if (n && nameToTab[n]) active.value = nameToTab[n]
  },
)

/**
 * @brief 切换底部导航对应的页面路由。
 * @param {string} name 选中的导航项名称。
 */
function onChange(name) {
  router.replace(tabPath[name])
}
</script>

<style scoped>
.layout {
  min-height: 100vh;
  display: flex;
  flex-direction: column;
}
.content {
  flex: 1;
  padding-bottom: 60px;
}
</style>

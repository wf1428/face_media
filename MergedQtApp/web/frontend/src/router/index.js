/**
 * @file index.js
 * @brief 沿用原界面路由、语言或展示配置，真实业务限于 V1 接口。
 * @author Du lin
 * @date 2026-10-09
 */
import { createRouter, createWebHashHistory } from 'vue-router'
import { useAppStore } from '../store'

const routes = [
  { path: '/', redirect: '/login' },
  {
    path: '/login',
    name: 'Login',
    component: () => import('../views/auth/Login.vue')
  },
  {
    path: '/home',
    component: () => import('../views/Layout.vue'),
    redirect: '/home/preview',
    children: [
      {
        path: 'preview',
        name: 'DevicePreview',
        component: () => import('../views/device/DevicePreview.vue')
      },
      {
        path: 'person',
        name: 'PersonList',
        component: () => import('../views/person/PersonList.vue')
      },
      {
        path: 'elevator',
        name: 'ElevatorControl',
        component: () => import('../views/elevator/ElevatorControl.vue')
      },
      {
        path: 'air',
        name: 'AirControl',
        component: () => import('../views/device/AirControl.vue')
      },
      {
        path: 'video',
        name: 'VideoManage',
        component: () => import('../views/video/VideoManage.vue')
      }
    ]
  },
  {
    // 人员详情独立路由（不展示底部 Tabbar）
    path: '/person/:id',
    name: 'PersonDetail',
    component: () => import('../views/person/PersonDetail.vue')
  },
  {
    // 设备设置独立路由（不展示底部 Tabbar）
    path: '/device/settings',
    name: 'DeviceSettings',
    component: () => import('../views/device/DeviceSettings.vue')
  },
  { path: '/:pathMatch(.*)*', redirect: '/login' }
]

const router = createRouter({
  history: createWebHashHistory(),
  routes
})

// 全局守卫：未登录跳转登录页
router.beforeEach(async (to) => {
  const store = useAppStore()
  if (to.name !== 'Login' && !store.authenticated && !await store.restore()) {
    return { name: 'Login' }
  }
  return true
})

export default router

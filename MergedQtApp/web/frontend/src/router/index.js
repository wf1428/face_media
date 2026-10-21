/**
 * @file index.js
 * @brief 注册设备管理页面路由，并通过现有会话入口恢复登录状态。
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

    /** @brief 按需加载该路由对应的页面组件。 */
    component: () => import('../views/auth/Login.vue'),
  },
  {
    path: '/home',

    /** @brief 按需加载该路由对应的页面组件。 */
    component: () => import('../views/Layout.vue'),
    redirect: '/home/preview',
    children: [
      {
        path: 'network',
        name: 'IPSettings',
        /** @brief 从设置菜单进入有线 IPv4 配置页面。 */
        component: () => import('../views/device/IPSettings.vue'),
      },
      {
        path: 'device',
        name: 'DeviceSettings',

        /** @brief 按路由按需加载设备设置页面组件。 */
        component: () => import('../views/device/DeviceSettings.vue'),
      },
      {
        path: 'preview',
        name: 'DevicePreview',

        /** @brief 按需加载该路由对应的页面组件。 */
        component: () => import('../views/device/DevicePreview.vue'),
      },
      {
        path: 'person',
        name: 'PersonList',

        /** @brief 按需加载该路由对应的页面组件。 */
        component: () => import('../views/person/PersonList.vue'),
      },
      {
        path: 'elevator',
        name: 'ElevatorControl',

        /** @brief 按需加载该路由对应的页面组件。 */
        component: () => import('../views/elevator/ElevatorControl.vue'),
      },
      {
        path: 'air',
        name: 'AirControl',

        /** @brief 按需加载该路由对应的页面组件。 */
        component: () => import('../views/device/AirControl.vue'),
      },
      {
        path: 'video',
        name: 'VideoManage',

        /** @brief 按需加载该路由对应的页面组件。 */
        component: () => import('../views/video/VideoManage.vue'),
      },
    ],
  },
  {
    // 人员详情独立路由（不展示底部 Tabbar）
    path: '/person/:id',
    name: 'PersonDetail',

    /** @brief 按需加载该路由对应的页面组件。 */
    component: () => import('../views/person/PersonDetail.vue'),
  },
  {
    // 保留原入口，统一进入底部设备菜单。
    path: '/device/settings',
    redirect: '/home/device',
  },
  { path: '/:pathMatch(.*)*', redirect: '/login' },
]

const router = createRouter({
  history: createWebHashHistory(),
  routes,
})

// 全局守卫：未登录跳转登录页
router.beforeEach(async (to) => {
  const store = useAppStore()
  if (to.name !== 'Login' && !store.authenticated && !(await store.restore())) {
    return { name: 'Login' }
  }
  return true
})

export default router

/**
 * @file main.js
 * @brief 注册原 Vue/Vant 前端组件，所有业务由独立服务接口提供。
 * @author Du lin
 * @date 2026-10-09
 */
import { createApp } from 'vue'
import { createPinia } from 'pinia'
import Vant from 'vant'
import 'vant/lib/index.css'

import App from './App.vue'
import router from './router'
import i18n from './i18n'
import './styles/theme.scss'

const app = createApp(App)

app.use(createPinia())
app.use(router)
app.use(i18n)
app.use(Vant)

app.mount('#app')
import { createApp } from 'vue'
import './style.css'
import App from './App.vue'
import router from './router'
import { createPinia } from 'pinia'
import { useAuthStore } from '@/stores/auth'

// 创建Vue应用实例
const app = createApp(App)

const pinia = createPinia()
app.use(pinia)

// 使用路由
app.use(router)

useAuthStore().init()

// 挂载应用
app.mount('#app')

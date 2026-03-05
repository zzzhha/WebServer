import { createRouter, createWebHistory } from 'vue-router'
import HomePage from '@/pages/HomePage.vue'
import WelcomePage from '@/pages/WelcomePage.vue'
import LoginPage from '@/pages/LoginPage.vue'
import RegisterPage from '@/pages/RegisterPage.vue'
import PicturePage from '@/pages/PicturePage.vue'
import VideoPage from '@/pages/VideoPage.vue'

// 定义路由配置
const routes = [
  {
    path: '/',
    name: 'home',
    component: HomePage,
  },
  { path: '/index.html', name: 'index', component: HomePage },
  { path: '/welcome.html', name: 'welcome', component: WelcomePage },
  { path: '/login.html', name: 'login', component: LoginPage },
  { path: '/register.html', name: 'register', component: RegisterPage },
  { path: '/picture.html', name: 'picture', component: PicturePage },
  { path: '/video.html', name: 'video', component: VideoPage },
]

// 创建路由实例
const router = createRouter({
  history: createWebHistory(),
  routes,
})

export default router

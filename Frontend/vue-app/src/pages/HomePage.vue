<script setup lang="ts">
import { storeToRefs } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import UserDropdownMenu from '@/components/UserDropdownMenu.vue'

const auth = useAuthStore()
const { username, token } = storeToRefs(auth)
</script>

<template>
  <header>
    <a href="index.html" class="logo">我的小窝</a>

    <div class="auth-controls">
      <a v-if="!(username && token)" href="login.html" class="auth-btn">登录</a>
      <span v-if="!(username && token)" class="divider">|</span>
      <a v-if="!(username && token)" href="register.html" class="auth-btn">注册</a>
      <UserDropdownMenu v-else :username="username || ''" />
    </div>
  </header>

  <div class="main-content">
    <h1 class="section-title">内容导航</h1>

    <div class="content-grid">
      <div class="content-card">
        <div class="card-image">🖼️</div>
        <div class="card-content">
          <h2 class="card-title">图片浏览</h2>
          <p class="card-desc">查看和下载各种精美图片，支持图片放大查看功能</p>
          <a href="picture.html" class="card-btn">进入图片页面</a>
        </div>
      </div>

      <div class="content-card">
        <div class="card-image">🎥</div>
        <div class="card-content">
          <h2 class="card-title">视频欣赏</h2>
          <p class="card-desc">浏览和观看精彩视频内容，支持视频下载</p>
          <a href="video.html" class="card-btn">进入视频页面</a>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 15px 20px;
  background-color: rgba(255, 255, 255, 0.8);
  border-radius: 10px;
  margin-bottom: 50px;
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
}

.logo {
  font-size: 1.8rem;
  color: #333;
  font-weight: bold;
  text-decoration: none;
}

.auth-controls {
  display: flex;
  gap: 15px;
  align-items: center;
}

.auth-btn {
  padding: 8px 16px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 1rem;
}

.auth-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.divider {
  color: #999;
}

.main-content {
  max-width: 1000px;
  margin: 0 auto;
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 40px;
}

.section-title {
  font-size: 2.2rem;
  color: #333;
  text-align: center;
  margin-bottom: 20px;
}

.content-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(300px, 1fr));
  gap: 30px;
  width: 100%;
}

.content-card {
  background-color: rgba(255, 255, 255, 0.8);
  border-radius: 15px;
  overflow: hidden;
  box-shadow: 0 4px 15px rgba(0, 0, 0, 0.1);
  transition: transform 0.3s ease;
}

.content-card:hover {
  transform: translateY(-5px);
}

.card-image {
  width: 100%;
  height: 200px;
  background-color: #eee;
  display: flex;
  justify-content: center;
  align-items: center;
  font-size: 3rem;
  color: #999;
}

.card-content {
  padding: 20px;
  text-align: center;
}

.card-title {
  font-size: 1.5rem;
  color: #333;
  margin-bottom: 15px;
}

.card-desc {
  color: #555;
  margin-bottom: 20px;
}

.card-btn {
  display: inline-block;
  padding: 10px 20px;
  background-color: #d2b48c;
  color: #333;
  border-radius: 8px;
  text-decoration: none;
  transition: all 0.3s ease;
}

.card-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

@media (max-width: 768px) {
  header {
    flex-direction: column;
    gap: 15px;
  }

  .section-title {
    font-size: 1.8rem;
  }

  .content-grid {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 480px) {
  .logo {
    font-size: 1.5rem;
  }

  .auth-controls {
    flex-wrap: wrap;
    justify-content: center;
  }

  .card-image {
    height: 150px;
  }
}
</style>

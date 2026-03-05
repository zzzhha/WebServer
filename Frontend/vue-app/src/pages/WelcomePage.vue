<script setup lang="ts">
import { useBodyClass } from '@/composables/useBodyClass'

useBodyClass('page-centered')

function goRegister() {
  window.location.href = 'register.html'
}

function goLogin() {
  window.location.href = 'login.html'
}
</script>

<template>
  <header>
    <div class="auth-buttons">
      <button class="btn" @click="goRegister">注册</button>
      <span class="divider">|</span>
      <button class="btn" @click="goLogin">登录</button>
    </div>
  </header>

  <main>
    <h1 class="welcome-title">欢迎来到我的小窝</h1>
    <p class="welcome-english">WELCOME</p>
    <a href="index.html" class="jump-btn">跳转至主页</a>
  </main>
</template>

<style scoped>
:global(body.page-centered) {
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
}

header {
  position: fixed;
  top: 20px;
  right: 20px;
}

.auth-buttons {
  display: flex;
  align-items: center;
  gap: 15px;
}

.btn {
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

.btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.btn:active {
  transform: scale(0.95);
}

.divider {
  font-weight: bold;
  font-size: 1.2rem;
}

main {
  text-align: center;
  max-width: 800px;
  width: 100%;
}

.welcome-title {
  font-size: 3.5rem;
  margin-bottom: 20px;
  color: #333;
}

.welcome-english {
  font-size: 2.5rem;
  margin-bottom: 40px;
  color: #555;
  font-weight: bold;
}

.jump-btn {
  padding: 12px 24px;
  font-size: 1.2rem;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
}

.jump-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.jump-btn:active {
  transform: scale(0.95);
}

@media (max-width: 768px) {
  .welcome-title {
    font-size: 2.5rem;
  }

  .welcome-english {
    font-size: 1.8rem;
  }

  .jump-btn {
    font-size: 1rem;
    padding: 10px 20px;
  }

  header {
    top: 10px;
    right: 10px;
  }

  .btn {
    padding: 6px 12px;
    font-size: 0.9rem;
  }

  .divider {
    font-size: 1rem;
  }
}

@media (max-width: 480px) {
  .welcome-title {
    font-size: 2rem;
  }

  .welcome-english {
    font-size: 1.5rem;
  }

  .auth-buttons {
    flex-direction: column;
    gap: 10px;
  }

  .divider {
    display: none;
  }
}
</style>

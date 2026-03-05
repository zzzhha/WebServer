<script setup lang="ts">
import { ref } from 'vue'
import { useBodyClass } from '@/composables/useBodyClass'
import { useAuthStore } from '@/stores/auth'

useBodyClass('page-centered')

const auth = useAuthStore()

const username = ref('')
const password = ref('')
const errorText = ref('')
const successText = ref('')
const submitting = ref(false)

function hideAllMessages() {
  errorText.value = ''
  successText.value = ''
}

async function onSubmit() {
  hideAllMessages()

  if (username.value.trim() === '') {
    errorText.value = '用户名不能为空'
    return
  }

  if (password.value.length < 6) {
    errorText.value = '密码长度至少6位'
    return
  }

  submitting.value = true
  try {
    const r = await auth.login({ username: username.value.trim(), password: password.value })
    if (r.ok) {
      const suffix = r.requestId ? ` (request_id=${r.requestId})` : ''
      successText.value = (r.message || '登录成功！即将跳转到首页...') + suffix
      window.setTimeout(() => {
        window.location.href = 'index.html'
      }, 2000)
    } else {
      errorText.value = r.message || '登录失败，请稍后重试'
    }
  } finally {
    submitting.value = false
  }
}
</script>

<template>
  <header>
    <a href="welcome.html" class="home-link">← 返回欢迎页</a>
  </header>

  <div class="form-container">
    <h2 class="form-title">登录</h2>

    <div class="message error-message" v-show="!!errorText">{{ errorText }}</div>
    <div class="message success-message" v-show="!!successText">{{ successText }}</div>

    <form class="login-form" @submit.prevent="onSubmit">
      <div class="form-group">
        <label for="username" class="form-label">用户名</label>
        <input id="username" v-model="username" type="text" class="form-input" required @input="hideAllMessages" />
      </div>

      <div class="form-group">
        <label for="password" class="form-label">密码</label>
        <input id="password" v-model="password" type="password" class="form-input" required @input="hideAllMessages" />
      </div>

      <button type="submit" class="btn" :disabled="submitting">登录</button>

      <div class="register-link">还没有账号？<a href="register.html">立即注册</a></div>
    </form>
  </div>
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
  left: 20px;
}

.home-link {
  color: #333;
  text-decoration: none;
  font-size: 1.2rem;
  transition: all 0.3s ease;
}

.home-link:hover {
  color: #c09871;
}

.form-container {
  background-color: rgba(255, 255, 255, 0.8);
  padding: 40px 30px;
  border-radius: 15px;
  box-shadow: 0 4px 15px rgba(0, 0, 0, 0.1);
  width: 100%;
  max-width: 400px;
}

.form-title {
  text-align: center;
  margin-bottom: 30px;
  color: #333;
  font-size: 2rem;
}

.form-group {
  margin-bottom: 20px;
}

.form-label {
  display: block;
  margin-bottom: 8px;
  color: #555;
  font-weight: 500;
}

.form-input {
  width: 100%;
  padding: 12px 15px;
  border: 1px solid #ddd;
  border-radius: 8px;
  font-size: 1rem;
  transition: border-color 0.3s ease;
}

.form-input:focus {
  outline: none;
  border-color: #d2b48c;
}

.btn {
  width: 100%;
  padding: 12px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  font-size: 1rem;
  cursor: pointer;
  transition: all 0.3s ease;
}

.btn:hover {
  background-color: #c09871;
  transform: translateY(-2px);
}

.btn:active {
  transform: translateY(0);
}

.register-link {
  text-align: center;
  margin-top: 20px;
  color: #555;
}

.register-link a {
  color: #d2b48c;
  text-decoration: none;
  font-weight: 500;
  transition: color 0.3s ease;
}

.register-link a:hover {
  color: #c09871;
}

.message {
  text-align: center;
  margin-bottom: 15px;
  padding: 10px;
  border-radius: 8px;
}

.error-message {
  color: #e74c3c;
  background-color: rgba(231, 76, 60, 0.1);
}

.success-message {
  color: #27ae60;
  background-color: rgba(39, 174, 96, 0.1);
}

@media (max-width: 480px) {
  .form-container {
    padding: 30px 20px;
  }

  .form-title {
    font-size: 1.8rem;
  }
}
</style>

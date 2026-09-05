<script setup lang="ts">
import { onMounted, ref } from 'vue'
import { useBodyClass } from '@/composables/useBodyClass'
import { useAuthStore } from '@/stores/auth'
import { httpRequestJson } from '@/api/httpClient'

useBodyClass('page-centered')

const auth = useAuthStore()

const username = ref('')
const password = ref('')
const confirmPassword = ref('')
const errorText = ref('')
const successText = ref('')
const submitting = ref(false)

const captchaToken = ref('')
const captchaPrompt = ref('')
const captchaAnswer = ref('')

const RESERVED_USERNAMES = ['admin', 'administrator', 'root', 'system', 'sys', 'test', 'guest', 'www', 'operator', 'webmaster', 'user', 'users', 'info', 'support', 'service']

function hideAllMessages() {
  errorText.value = ''
  successText.value = ''
}

async function loadCaptcha() {
  captchaAnswer.value = ''
  const r = await httpRequestJson<{ success: boolean; data?: { token?: string; prompt?: string } }>('/api/captcha', { retry: 0 })
  if (r.ok && r.body?.data?.token && r.body.data.prompt) {
    captchaToken.value = r.body.data.token
    captchaPrompt.value = r.body.data.prompt
  } else {
    captchaPrompt.value = ''
    captchaToken.value = ''
    errorText.value = '验证码加载失败，请刷新重试'
  }
}

function isReserved(username: string) {
  return RESERVED_USERNAMES.includes(username.trim().toLowerCase())
}

async function onSubmit() {
  hideAllMessages()

  if (username.value.trim() === '') {
    errorText.value = '用户名不能为空'
    return
  }

  if (isReserved(username.value)) {
    errorText.value = '该用户名不可用，请换一个'
    return
  }

  if (password.value.length < 6) {
    errorText.value = '密码长度至少6位'
    return
  }

  if (password.value !== confirmPassword.value) {
    errorText.value = '两次输入的密码不一致'
    return
  }

  if (!captchaToken.value || captchaAnswer.value.trim() === '') {
    errorText.value = '请填写验证码'
    return
  }

  submitting.value = true
  try {
    const r = await auth.register({
      username: username.value.trim(),
      password: password.value,
      captchaToken: captchaToken.value,
      captchaAnswer: captchaAnswer.value.trim(),
    })
    if (r.ok) {
      const suffix = r.requestId ? ` (request_id=${r.requestId})` : ''
      successText.value = (r.message || '注册成功！即将跳转到登录页...') + suffix
      window.setTimeout(() => {
        window.location.href = 'login.html'
      }, 2000)
    } else {
      errorText.value = r.message || '注册失败，请确认后重试'
      // 失败后刷新验证码（一次性 token 已消费）
      await loadCaptcha()
    }
  } finally {
    submitting.value = false
  }
}

onMounted(loadCaptcha)
</script>

<template>
  <header>
    <a href="welcome.html" class="home-link">← 返回欢迎页</a>
  </header>

  <div class="form-container">
    <h2 class="form-title">注册</h2>

    <div class="message success-message" v-show="!!successText">{{ successText }}</div>
    <div class="message error-message" v-show="!!errorText">{{ errorText }}</div>

    <form class="register-form" @submit.prevent="onSubmit">
      <div class="form-group">
        <label for="username" class="form-label">用户名</label>
        <input id="username" v-model="username" type="text" class="form-input" required @input="hideAllMessages" />
      </div>

      <div class="form-group">
        <label for="password" class="form-label">密码</label>
        <input id="password" v-model="password" type="password" class="form-input" required @input="hideAllMessages" />
        <p class="password-hint">密码长度至少6位</p>
      </div>

      <div class="form-group">
        <label for="confirmPassword" class="form-label">确认密码</label>
        <input
          id="confirmPassword"
          v-model="confirmPassword"
          type="password"
          class="form-input"
          required
          @input="hideAllMessages"
        />
      </div>

      <div class="form-group">
        <label for="captcha" class="form-label">验证码</label>
        <div class="captcha-row">
          <span class="captcha-prompt" @click="loadCaptcha" title="点击刷新">{{ captchaPrompt || '加载中…' }}</span>
          <input
            id="captcha"
            v-model="captchaAnswer"
            type="text"
            class="form-input captcha-input"
            placeholder="请输入答案"
            maxlength="4"
            autocomplete="off"
            required
            @input="hideAllMessages"
          />
        </div>
        <p class="password-hint">点击算式可刷新验证码</p>
      </div>

      <button type="submit" class="btn" :disabled="submitting">注册</button>

      <div class="login-link">已有账号？<a href="login.html">立即登录</a></div>
    </form>
  </div>
</template>

<style scoped>
:global(body.page-centered) {
  display: flex;
  flex-direction: column;
  justify-content: center;
  align-items: center;
  min-height: 100vh;
  margin: 0;
  padding: 20px;
  box-sizing: border-box;
}

:global(body.page-centered #app) {
  width: min(92vw, 400px);
}

header {
  position: fixed;
  top: 20px;
  left: 20px;
  z-index: 100;
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
  max-width: none;
  min-height: 450px;
  box-sizing: border-box;
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
  box-sizing: border-box;
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
  box-sizing: border-box;
}

.btn:hover {
  background-color: #c09871;
  transform: translateY(-2px);
}

.btn:active {
  transform: translateY(0);
}

.login-link {
  text-align: center;
  margin-top: 20px;
  color: #555;
}

.login-link a {
  color: #d2b48c;
  text-decoration: none;
  font-weight: 500;
  transition: color 0.3s ease;
}

.login-link a:hover {
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

.password-hint {
  font-size: 0.8rem;
  color: #999;
  margin-top: 5px;
}

.captcha-row {
  display: flex;
  align-items: center;
  gap: 10px;
}

.captcha-prompt {
  flex-shrink: 0;
  min-width: 130px;
  background-color: #efe6d4;
  color: #333;
  border-radius: 8px;
  padding: 12px 14px;
  font-size: 1.05rem;
  text-align: center;
  cursor: pointer;
  user-select: none;
  letter-spacing: 1px;
  transition: background-color 0.2s ease;
}

.captcha-prompt:hover {
  background-color: #e2d2b5;
}

.captcha-input {
  flex: 1;
  min-width: 0;
}

/* 移动端适配 */
@media (max-width: 480px) {
  .form-container {
    padding: 30px 20px;
    margin: 0 10px;
  }
  
  .form-title {
    font-size: 1.8rem;
  }
  
  .form-input {
    padding: 10px 12px;
  }
  
  .btn {
    padding: 10px;
  }
  
  /* 键盘弹出时的处理 */
  @media (max-height: 500px) {
    :global(body.page-centered) {
      justify-content: flex-start;
      padding-top: 80px;
    }
    
    .form-container {
      max-height: 80vh;
      overflow-y: auto;
    }
  }
}
</style>

import { defineStore } from 'pinia'
import { computed, ref } from 'vue'
import { handleHttpError, httpRequestJson, toFormUrlEncoded } from '@/api/httpClient'

type AuthLoginResponse = {
  success: boolean
  message?: string
  status?: number
  data?: {
    token?: string
  }
}

type AuthRegisterResponse = {
  success: boolean
  message?: string
  status?: number
}

function isTokenValid(token: string): boolean {
  try {
    const payload = JSON.parse(atob(token.split('.')[1]))
    return payload.exp * 1000 > Date.now()
  } catch {
    return false
  }
}

export const useAuthStore = defineStore('auth', () => {
  const username = ref<string | null>(null)
  const token = ref<string | null>(null)

  const isLoggedIn = computed(() => Boolean(username.value && token.value))

  function hydrateFromStorage() {
    const storedToken = localStorage.getItem('jwt_token')
    if (storedToken && isTokenValid(storedToken)) {
      username.value = localStorage.getItem('username')
      token.value = storedToken
    } else {
      clear()
    }
  }

  function persistToStorage() {
    if (username.value) localStorage.setItem('username', username.value)
    if (token.value) localStorage.setItem('jwt_token', token.value)
  }

  function clear() {
    username.value = null
    token.value = null
    localStorage.removeItem('username')
    localStorage.removeItem('jwt_token')
    localStorage.removeItem('refresh_token')
    localStorage.removeItem('csrf_token')
  }

  function init() {
    hydrateFromStorage()
    window.addEventListener('storage', hydrateFromStorage)
  }

  async function login(input: { username: string; password: string }) {
    const timestamp = Date.now().toString()
    const nonce = Math.random().toString(36).substring(2, 15)
    const r = await httpRequestJson<AuthLoginResponse>('/login', {
      method: 'POST',
      data: toFormUrlEncoded({
        username: input.username,
        password: input.password,
        timestamp,
        nonce,
      }),
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
      },
      retry: 1,
    })

    if (r.ok && r.body?.success && r.body.data?.token) {
      username.value = input.username
      token.value = r.body.data.token
      // 存储refresh_token（如果响应中包含）
      if (r.body.data.refresh_token) {
        localStorage.setItem('refresh_token', r.body.data.refresh_token)
      }
      persistToStorage()
      // 检查并存储CSRF Token
      const csrfToken = localStorage.getItem('csrf_token')
      if (!csrfToken) {
        // 这里应该从响应头或响应体中获取CSRF Token
        // 暂时使用一个简单的实现
        const newCsrfToken = Math.random().toString(36).substring(2, 15)
        localStorage.setItem('csrf_token', newCsrfToken)
      }
      return { ok: true as const, message: r.body.message || '登录成功', requestId: r.requestId }
    }

    const msg = handleHttpError({ status: r.status, requestId: r.requestId, body: r.body })
    return { ok: false as const, message: msg, requestId: r.requestId }
  }

  async function register(input: { username: string; password: string }) {
    const timestamp = Date.now().toString()
    const nonce = Math.random().toString(36).substring(2, 15)
    const r = await httpRequestJson<AuthRegisterResponse>('/register', {
      method: 'POST',
      data: toFormUrlEncoded({
        username: input.username,
        password: input.password,
        timestamp,
        nonce,
      }),
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
      },
      retry: 1,
    })

    if (r.ok && r.body?.success) {
      return { ok: true as const, message: r.body.message || '注册成功', requestId: r.requestId }
    }

    const msg = handleHttpError({ status: r.status, requestId: r.requestId, body: r.body })
    return { ok: false as const, message: msg, requestId: r.requestId }
  }

  return {
    username,
    token,
    isLoggedIn,
    init,
    clear,
    login,
    register,
  }
})


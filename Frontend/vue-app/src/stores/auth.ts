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

export const useAuthStore = defineStore('auth', () => {
  const username = ref<string | null>(null)
  const token = ref<string | null>(null)

  const isLoggedIn = computed(() => Boolean(username.value && token.value))

  function hydrateFromStorage() {
    username.value = localStorage.getItem('username')
    token.value = localStorage.getItem('jwt_token')
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
  }

  function init() {
    hydrateFromStorage()
    window.addEventListener('storage', hydrateFromStorage)
  }

  async function login(input: { username: string; password: string }) {
    const r = await httpRequestJson<AuthLoginResponse>('/login', {
      method: 'POST',
      data: toFormUrlEncoded({
        username: input.username,
        password: input.password,
      }),
      headers: {
        'Content-Type': 'application/x-www-form-urlencoded',
      },
      retry: 1,
    })

    if (r.ok && r.body?.success && r.body.data?.token) {
      username.value = input.username
      token.value = r.body.data.token
      persistToStorage()
      return { ok: true as const, message: r.body.message || '登录成功', requestId: r.requestId }
    }

    const msg = handleHttpError({ status: r.status, requestId: r.requestId, body: r.body })
    return { ok: false as const, message: msg, requestId: r.requestId }
  }

  async function register(input: { username: string; password: string }) {
    const r = await httpRequestJson<AuthRegisterResponse>('/register', {
      method: 'POST',
      data: toFormUrlEncoded({
        username: input.username,
        password: input.password,
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


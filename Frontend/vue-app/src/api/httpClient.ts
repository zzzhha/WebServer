import axios, { type AxiosRequestConfig, type AxiosResponse } from 'axios'
import { useToastStore } from '@/stores/toast'

let isRefreshing = false
let refreshSubscribers: ((token: string) => void)[] = []

export type HttpResult<T> = {
  ok: boolean
  status: number
  requestId: string
  body: T | null
}

const client = axios.create({
  baseURL: '',
  timeout: 10_000,
  validateStatus: () => true,
})

client.interceptors.request.use((config) => {
  const token = localStorage.getItem('jwt_token') || ''
  if (!token) return config
  const headers = (config.headers ?? {}) as any
  config.headers = headers
  if (!headers.Authorization && !headers.authorization) {
    headers.Authorization = `Bearer ${token}`
  }
  // 添加CSRF Token保护
  const csrfToken = localStorage.getItem('csrf_token')
  if (csrfToken) {
    headers['X-CSRF-Token'] = csrfToken
  }
  return config
})

// 添加响应拦截器处理Token刷新
client.interceptors.response.use(
  (response) => response,
  async (error) => {
    const originalRequest = error.config
    
    // 处理401错误（Token过期）
    if (error.response?.status === 401 && !originalRequest._retry) {
      if (isRefreshing) {
        // 等待token刷新完成
        return new Promise((resolve) => {
          refreshSubscribers.push((token: string) => {
            originalRequest.headers.Authorization = `Bearer ${token}`
            resolve(client(originalRequest))
          })
        })
      }
      
      originalRequest._retry = true
      isRefreshing = true
      
      try {
        // 尝试刷新token
        const refreshToken = localStorage.getItem('refresh_token')
        if (!refreshToken) {
          throw new Error('No refresh token')
        }
        
        const response = await client.post('/refresh-token', {
          refresh_token: refreshToken
        })
        
        if (response.status === 200 && response.data?.success && response.data?.data?.token) {
          const newToken = response.data.data.token
          localStorage.setItem('jwt_token', newToken)
          
          // 通知所有等待的请求
          refreshSubscribers.forEach(cb => cb(newToken))
          refreshSubscribers = []
          
          // 更新当前请求的token并重新发送
          originalRequest.headers.Authorization = `Bearer ${newToken}`
          return client(originalRequest)
        } else {
          throw new Error('Token refresh failed')
        }
      } catch (refreshError) {
        // 刷新失败，清除token并跳转到登录页
        localStorage.removeItem('jwt_token')
        localStorage.removeItem('refresh_token')
        localStorage.removeItem('username')
        localStorage.removeItem('csrf_token')
        
        if (!window.location.pathname.endsWith('/login.html')) {
          window.location.href = 'login.html'
        }
        
        return Promise.reject(refreshError)
      } finally {
        isRefreshing = false
      }
    }
    
    return Promise.reject(error)
  }
)

function getRequestId(resp: AxiosResponse) {
  const v = resp.headers['x-request-id']
  if (!v) return ''
  return Array.isArray(v) ? v[0] : String(v)
}

function sleep(ms: number) {
  return new Promise((r) => window.setTimeout(r, ms))
}

export function toFormUrlEncoded(input: Record<string, string>) {
  const p = new URLSearchParams()
  Object.entries(input).forEach(([k, v]) => p.append(k, v))
  return p
}

export async function httpRequestJson<T>(
  url: string,
  config: Omit<AxiosRequestConfig, 'url'> & { retry?: number } = {},
): Promise<HttpResult<T>> {
  const retry = config.retry ?? 0
  const attemptOnce = async () => {
    const resp = await client.request<T>({ ...config, url })
    const requestId = getRequestId(resp)
    return {
      ok: resp.status >= 200 && resp.status < 300,
      status: resp.status,
      requestId,
      body: resp.data ?? null,
    } satisfies HttpResult<T>
  }

  for (let i = 0; i <= retry; i += 1) {
    try {
      const r = await attemptOnce()
      if (!r.ok && (r.status === 502 || r.status === 503 || r.status === 504)) {
        if (i < retry) {
          await sleep(250 * (i + 1))
          continue
        }
      }
      return r
    } catch (e) {
      if (i < retry) {
        await sleep(250 * (i + 1))
        continue
      }
    }
  }

  const toast = useToastStore()
  toast.push('服务器连接失败，请检查网络', 2200)
  return { ok: false, status: 0, requestId: '', body: null }
}

export function handleHttpError(input: {
  status: number
  requestId: string
  body: any
}) {
  const toast = useToastStore()
  const suffix = input.requestId ? ` (request_id=${input.requestId})` : ''
  const msg = (input.body && input.body.message) || '请求失败'

  if (input.status === 401) {
    if (!location.pathname.endsWith('/login.html') && !location.pathname.endsWith('login.html')) {
      toast.push('未授权，请先登录' + suffix, 2000)
      window.setTimeout(() => {
        window.location.href = 'login.html'
      }, 400)
    }
    return msg
  }

  if (input.status === 403) {
    toast.push('禁止访问' + suffix, 2000)
    window.setTimeout(() => {
      window.location.href = '403.html'
    }, 400)
    return msg
  }

  if (input.status === 404) {
    toast.push('资源不存在' + suffix, 2000)
    window.setTimeout(() => {
      window.location.href = '404.html'
    }, 400)
    return msg
  }

  if (input.status >= 500) {
    toast.push('服务器错误' + suffix, 2000)
    window.setTimeout(() => {
      window.location.href = 'error.html'
    }, 400)
    return msg
  }

  toast.push(`${msg}${suffix}`, 2200)
  return msg
}

import axios, { type AxiosRequestConfig, type AxiosResponse } from 'axios'
import { useToastStore } from '@/stores/toast'

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
  return config
})

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

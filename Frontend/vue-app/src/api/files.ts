import { httpRequestJson } from '@/api/httpClient'

export type ApiEnvelope<T> = {
  success: boolean
  message?: string
  data?: T
}

export type FileItem = {
  folder: string
  name: string
  size: number
  mimeType: string
  updatedAt: string
  url: string
  downloadUrl: string
  thumbState?: 'none' | 'ok' | 'fail'
  thumbWidth?: number
  posterState?: 'none' | 'ok' | 'fail'
}

export type ListFilesParams = {
  page?: number
  pageSize?: number
  sort?: 'name' | 'size' | 'mtime'
  order?: 'asc' | 'desc'
  q?: string
}

export type PagedFiles = {
  files: FileItem[]
  total: number
  page: number
  pageSize: number
  totalPages: number
}

const EMPTY_PAGE: PagedFiles = { files: [], total: 0, page: 1, pageSize: 50, totalPages: 0 }

export async function listFiles(folder: 'images' | 'video' | 'uploads', params: ListFilesParams = {}) {
  const qs = new URLSearchParams({ folder })
  if (params.page && params.page > 1) qs.set('page', String(params.page))
  if (params.pageSize && params.pageSize !== 50) qs.set('pageSize', String(params.pageSize))
  if (params.sort) qs.set('sort', params.sort)
  if (params.order) qs.set('order', params.order)
  if (params.q) qs.set('q', params.q)

  const r = await httpRequestJson<ApiEnvelope<PagedFiles>>(`/api/files?${qs.toString()}`, { retry: 1 })
  if (r.ok && r.body?.success && r.body.data) {
    return { ok: true as const, data: r.body.data, requestId: r.requestId }
  }
  return {
    ok: false as const,
    data: { ...EMPTY_PAGE, page: params.page ?? 1, pageSize: params.pageSize ?? 50 },
    requestId: r.requestId,
  }
}

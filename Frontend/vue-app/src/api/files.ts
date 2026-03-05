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
}

export async function listFiles(folder: 'images' | 'video' | 'uploads') {
  const r = await httpRequestJson<ApiEnvelope<{ files: FileItem[] }>>(`/api/files?folder=${folder}`, { retry: 1 })
  if (r.ok && r.body?.success && r.body.data) {
    return { ok: true as const, files: r.body.data.files, requestId: r.requestId }
  }
  return { ok: false as const, files: [] as FileItem[], requestId: r.requestId }
}


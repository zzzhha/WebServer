import { httpRequestJson, toFormUrlEncoded, type HttpResult } from '@/api/httpClient'
import type { ApiEnvelope } from '@/api/files'

type UploadInitData = {
  uploadId: string
  chunkSize: number
  uploadedParts: number[]
}

type UploadCompleteData = {
  folder: string
  name: string
  url: string
  downloadUrl: string
}

export type UploadProgress = {
  uploadedBytes: number
  totalBytes: number
  percent: number
}

export async function resumableUpload(options: {
  file: File
  folder: 'images' | 'video' | 'uploads'
  onProgress?: (p: UploadProgress) => void
  signal?: AbortSignal
}) {
  const key = `upload:${options.folder}:${options.file.name}:${options.file.size}`
  const cached = safeJsonParse(localStorage.getItem(key)) as { uploadId?: string } | null
  const initBody = toFormUrlEncoded({
    fileName: options.file.name,
    fileSize: String(options.file.size),
    chunkSize: String(1024 * 1024),
    folder: options.folder,
    uploadId: cached?.uploadId || '',
  })

  const init = await httpRequestJson<ApiEnvelope<UploadInitData>>('/api/uploads/init', {
    method: 'POST',
    data: initBody,
    headers: {
      'Content-Type': 'application/x-www-form-urlencoded',
    },
    retry: 1,
    signal: options.signal,
  })

  if (!isOkEnvelope(init)) return init

  const uploadId = init.body.data.uploadId
  const chunkSize = init.body.data.chunkSize || 1024 * 1024
  const uploaded = new Set(init.body.data.uploadedParts || [])

  localStorage.setItem(key, JSON.stringify({ uploadId }))

  const partCount = Math.ceil(options.file.size / chunkSize)
  let uploadedBytes = 0
  for (let i = 0; i < partCount; i += 1) {
    if (uploaded.has(i)) {
      const start = i * chunkSize
      const end = Math.min(options.file.size, start + chunkSize)
      uploadedBytes += end - start
    }
  }
  options.onProgress?.({
    uploadedBytes,
    totalBytes: options.file.size,
    percent: options.file.size ? Math.floor((uploadedBytes / options.file.size) * 100) : 0,
  })

  for (let partNo = 0; partNo < partCount; partNo += 1) {
    if (options.signal?.aborted) {
      return init
    }
    if (uploaded.has(partNo)) continue

    const start = partNo * chunkSize
    const end = Math.min(options.file.size, start + chunkSize)
    const buf = await options.file.slice(start, end).arrayBuffer()

    const put = await httpRequestJson<ApiEnvelope<{ partNo: number; size: number }>>(
      `/api/uploads/${encodeURIComponent(uploadId)}/parts/${partNo}`,
      {
        method: 'PUT',
        data: buf,
        headers: {
          'Content-Type': 'application/octet-stream',
        },
        retry: 1,
        signal: options.signal,
      },
    )

    if (!isOkEnvelope(put)) return put
    uploaded.add(partNo)
    uploadedBytes += end - start
    options.onProgress?.({
      uploadedBytes,
      totalBytes: options.file.size,
      percent: options.file.size ? Math.floor((uploadedBytes / options.file.size) * 100) : 0,
    })
  }

  const done = await httpRequestJson<ApiEnvelope<UploadCompleteData>>(
    `/api/uploads/${encodeURIComponent(uploadId)}/complete`,
    {
      method: 'POST',
      retry: 1,
      signal: options.signal,
    },
  )

  if (isOkEnvelope(done)) {
    localStorage.removeItem(key)
  }
  return done
}

function isOkEnvelope<T>(r: HttpResult<ApiEnvelope<T>>) {
  return r.ok && !!r.body && r.body.success && !!r.body.data
}

function safeJsonParse(input: string | null) {
  if (!input) return null
  try {
    return JSON.parse(input)
  } catch {
    return null
  }
}


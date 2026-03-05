const base = process.env.BACKEND_ORIGIN || 'http://0.0.0.0:5050'

async function getJson(url, init) {
  const resp = await fetch(url, init)
  const text = await resp.text()
  let body = null
  try {
    body = JSON.parse(text)
  } catch {
    body = null
  }
  return { ok: resp.ok, status: resp.status, body, text }
}

async function main() {
  const r = await getJson(`${base}/api/files?folder=images`)
  if (!r.ok) {
    console.error('smoke failed: /api/files', r.status, r.text.slice(0, 200))
    process.exit(1)
  }
  if (!r.body || r.body.success !== true || !r.body.data || !Array.isArray(r.body.data.files)) {
    console.error('smoke failed: unexpected response shape')
    process.exit(1)
  }
  console.log('smoke ok: /api/files images ->', r.body.data.files.length, 'files')
}

main().catch((e) => {
  console.error('smoke failed:', e)
  process.exit(1)
})

import fs from 'node:fs/promises'
import path from 'node:path'
import { fileURLToPath } from 'node:url'
import { parse as parseYaml } from 'yaml'
import openapiToPostman from 'openapi-to-postmanv2'
import https from 'node:https'

const __filename = fileURLToPath(import.meta.url)
const __dirname = path.dirname(__filename)

const apiDocsDir = path.resolve(__dirname, '..')
const openapiYamlPath = path.join(apiDocsDir, 'openapi.yaml')
const openapiJsonPath = path.join(apiDocsDir, 'openapi.json')
const postmanPath = path.join(apiDocsDir, 'postman-collection.json')
const indexHtmlPath = path.join(apiDocsDir, 'index.html')
const redocJsPath = path.join(apiDocsDir, 'redoc.standalone.js')
const redocJsUrl = 'https://cdn.redoc.ly/redoc/latest/bundles/redoc.standalone.js'

function buildIndexHtml(specObject) {
  const specJson = JSON.stringify(specObject)
  return `<!doctype html>
<html lang="zh-CN">
  <head>
    <meta charset="utf-8" />
    <meta name="viewport" content="width=device-width, initial-scale=1" />
    <title>${escapeHtml(specObject?.info?.title ?? 'API Docs')}</title>
    <style>
      html, body { height: 100%; margin: 0; padding: 0; }
    </style>
  </head>
  <body>
    <div id="redoc-container"></div>
    <script>
      window.__OPENAPI_SPEC__ = ${specJson};
    </script>
    <script src="./redoc.standalone.js"></script>
    <script>
      const spec = window.__OPENAPI_SPEC__;
      Redoc.init(spec, { scrollYOffset: 0 }, document.getElementById('redoc-container'));
    </script>
  </body>
</html>
`
}

async function ensureRedocBundle() {
  try {
    const st = await fs.stat(redocJsPath)
    if (st.size > 1024 * 1024) return
  } catch {
    
  }

  const js = await new Promise((resolve, reject) => {
    https
      .get(redocJsUrl, (res) => {
        if (res.statusCode !== 200) {
          reject(new Error(`下载 ReDoc 失败：HTTP ${res.statusCode}`))
          res.resume()
          return
        }
        res.setEncoding('utf-8')
        let data = ''
        res.on('data', (chunk) => (data += chunk))
        res.on('end', () => resolve(data))
      })
      .on('error', reject)
  })

  await fs.writeFile(redocJsPath, js, 'utf-8')
}

function escapeHtml(v) {
  return String(v)
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;')
}

async function readOpenApiYaml() {
  const yamlText = await fs.readFile(openapiYamlPath, 'utf-8')
  return yamlText
}

async function main() {
  const yamlText = await readOpenApiYaml()
  const spec = parseYaml(yamlText)
  if (!spec || typeof spec !== 'object' || !spec.openapi) {
    throw new Error('openapi.yaml 解析失败：缺少 openapi 字段')
  }

  await ensureRedocBundle()

  await fs.writeFile(openapiJsonPath, JSON.stringify(spec, null, 2) + '\n', 'utf-8')

  const postman = await new Promise((resolve, reject) => {
    openapiToPostman.convert(
      { type: 'json', data: spec },
      {
        schemaFaker: true,
        requestParametersResolution: 'Schema',
        includeAuthInfoInExample: true
      },
      (err, result) => {
        if (err) return reject(err)
        if (!result?.result) return reject(new Error(result?.reason ?? 'OpenAPI 转 Postman 失败'))
        return resolve(result.output[0].data)
      }
    )
  })

  await fs.writeFile(postmanPath, JSON.stringify(postman, null, 2) + '\n', 'utf-8')

  const indexHtml = buildIndexHtml(spec)
  await fs.writeFile(indexHtmlPath, indexHtml, 'utf-8')
}

await main()

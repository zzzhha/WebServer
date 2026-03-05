import fs from 'node:fs'
import path from 'node:path'

const outDir = path.resolve(process.cwd(), '../html')
const assetsDir = path.join(outDir, 'assets')

if (!fs.existsSync(assetsDir)) {
  process.exit(0)
}

const entries = fs.readdirSync(assetsDir)
  .filter((f) => !f.endsWith('.map'))
  .map((f) => ({
    file: f,
    size: fs.statSync(path.join(assetsDir, f)).size,
  }))
  .sort((a, b) => b.size - a.size)

const maxAssetBytes = 512 * 1024
const tooLarge = entries.filter((e) => e.size > maxAssetBytes)

if (tooLarge.length) {
  console.log('Bundle assets too large (> 512KB):')
  tooLarge.slice(0, 10).forEach((e) => {
    console.log(`${e.file}\t${Math.round(e.size / 1024)}KB`)
  })
  process.exit(1)
}

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { storeToRefs } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import { listFiles, type FileItem } from '@/api/files'
import { useToastStore } from '@/stores/toast'
import { resumableUpload } from '@/utils/resumableUpload'

const { username, token } = storeToRefs(useAuthStore())

const toast = useToastStore()

const remoteFiles = ref<FileItem[] | null>(null)
const images = computed(() => {
  if (remoteFiles.value && remoteFiles.value.length) return remoteFiles.value
  return [
    { folder: 'images', name: 'hui.jpg', url: '/images/hui.jpg', downloadUrl: '/download/images/hui.jpg' },
    { folder: 'images', name: 'undeadunluck.jpg', url: '/images/undeadunluck.jpg', downloadUrl: '/download/images/undeadunluck.jpg' },
    { folder: 'images', name: 'naxida.jpg', url: '/images/naxida.jpg', downloadUrl: '/download/images/naxida.jpg' },
    { folder: 'images', name: 'galgame.jpg', url: '/images/galgame.jpg', downloadUrl: '/download/images/galgame.jpg' },
  ] as any
})

const loaded = ref<Record<string, boolean>>({})
const errored = ref<Record<string, boolean>>({})

const zoomOpen = ref(false)
const zoomSrc = ref('')

function loadImage(src: string) {
  if (loaded.value[src] || errored.value[src]) return
  const img = new Image()
  img.src = src
  img.onload = () => {
    loaded.value = { ...loaded.value, [src]: true }
  }
  img.onerror = () => {
    errored.value = { ...errored.value, [src]: true }
  }
}

function openZoom(src: string) {
  zoomSrc.value = src
  zoomOpen.value = true
}

function closeZoom() {
  zoomOpen.value = false
}

async function refresh() {
  const r = await listFiles('images')
  if (r.ok) {
    remoteFiles.value = r.files
  }
}

const uploadInput = ref<HTMLInputElement | null>(null)
const uploadOpen = ref(false)
const uploadName = ref('')
const uploadPercent = ref(0)
const uploadController = ref<AbortController | null>(null)

function pickUpload() {
  uploadInput.value?.click()
}

async function onPickFile(e: Event) {
  const el = e.target as HTMLInputElement
  const file = el.files?.[0]
  el.value = ''
  if (!file) return

  uploadOpen.value = true
  uploadName.value = file.name
  uploadPercent.value = 0
  uploadController.value?.abort()
  const ctrl = new AbortController()
  uploadController.value = ctrl

  toast.push('已开始上传：' + file.name, 1200)
  const done = await resumableUpload({
    file,
    folder: 'images',
    signal: ctrl.signal,
    onProgress(p) {
      uploadPercent.value = p.percent
    },
  })

  if (done.ok && done.body?.success) {
    toast.push('上传完成：' + file.name, 1500)
    uploadOpen.value = false
    await refresh()
  } else if (ctrl.signal.aborted) {
    toast.push('已暂停上传：' + file.name, 1500)
  } else {
    toast.push('上传失败：' + file.name, 2000)
  }
}

function pauseUpload() {
  uploadController.value?.abort()
}

onMounted(() => {
  refresh()
})
</script>

<template>
  <header>
    <div class="header-left">
      <a href="index.html" class="back-btn">← 返回主页</a>
      <div class="logo">图片展示</div>
    </div>
    <div class="nav-links">
      <span v-if="username && token" class="greeting">欢迎您，{{ username }}</span>
      <a v-else href="login.html">登录</a>
      <span v-if="!(username && token)" class="divider">|</span>
      <a v-if="!(username && token)" href="register.html">注册</a>
      <span v-if="!(username && token)" class="divider">|</span>
      <a href="video.html">视频</a>
      <span class="divider">|</span>
      <button type="button" class="upload-link" @click="pickUpload">上传</button>
    </div>
  </header>

  <input ref="uploadInput" type="file" accept="image/*" style="display: none" @change="onPickFile" />

  <div v-if="uploadOpen" class="upload-status">
    <div class="upload-title">正在上传：{{ uploadName }}</div>
    <div class="upload-bar">
      <div class="upload-bar-inner" :style="{ width: uploadPercent + '%' }"></div>
    </div>
    <div class="upload-actions">
      <button type="button" class="upload-action" @click="pauseUpload">暂停</button>
    </div>
  </div>

  <div class="image-grid">
    <div v-for="item in images" :key="item.name" class="image-card">
      <div
        class="image-placeholder"
        role="button"
        tabindex="0"
        @click="loadImage(item.url)"
        @keydown.enter="loadImage(item.url)"
      >
        <template v-if="loaded[item.url]">
          <img :src="item.url" alt="" class="image" />
        </template>
        <template v-else-if="errored[item.url]">
          <span class="error-text">图片加载失败</span>
        </template>
        <template v-else>
          点击加载图片
        </template>
      </div>
      <div class="image-actions">
        <a class="action-btn" :href="item.downloadUrl">下载</a>
        <button class="action-btn" type="button" @click="openZoom(item.url)">点击放大</button>
      </div>
    </div>
  </div>

  <div v-if="zoomOpen" class="modal" @click.self="closeZoom">
    <span class="close-btn" @click="closeZoom">&times;</span>
    <img class="modal-content" :src="zoomSrc" alt="放大的图片" />
  </div>
</template>

<style scoped>
header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 15px 20px;
  background-color: rgba(255, 255, 255, 0.8);
  border-radius: 10px;
  margin-bottom: 30px;
  box-shadow: 0 2px 10px rgba(0, 0, 0, 0.1);
}

.header-left {
  display: flex;
  align-items: center;
  gap: 15px;
}

.back-btn {
  padding: 6px 12px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 0.9rem;
}

.back-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.logo {
  font-size: 1.5rem;
  color: #333;
  font-weight: bold;
}

.nav-links {
  display: flex;
  gap: 20px;
  align-items: center;
}

.nav-links a,
.nav-links span {
  color: #555;
  text-decoration: none;
  transition: color 0.3s ease;
  font-size: 1rem;
}

.upload-link {
  background: transparent;
  border: none;
  padding: 0;
  cursor: pointer;
  color: #555;
  font-size: 1rem;
  transition: color 0.3s ease;
}

.upload-link:hover {
  color: #d2b48c;
}

.upload-status {
  position: fixed;
  right: 20px;
  bottom: 20px;
  padding: 10px 14px;
  background: rgba(0, 0, 0, 0.75);
  color: #fff;
  border-radius: 6px;
  font-size: 14px;
  z-index: 9999;
  width: min(360px, 86vw);
}

.upload-title {
  margin-bottom: 8px;
}

.upload-bar {
  height: 8px;
  background: rgba(255, 255, 255, 0.2);
  border-radius: 999px;
  overflow: hidden;
}

.upload-bar-inner {
  height: 100%;
  background: #d2b48c;
  width: 0;
  transition: width 0.2s ease;
}

.upload-actions {
  margin-top: 10px;
  display: flex;
  justify-content: flex-end;
}

.upload-action {
  padding: 6px 12px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  transition: all 0.3s ease;
}

.upload-action:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.nav-links a:hover {
  color: #d2b48c;
}

.divider {
  margin: 0 5px;
  color: #999;
}

.greeting {
  color: #333;
  font-weight: 500;
}

.image-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(300px, 1fr));
  gap: 20px;
  margin: 0 auto;
  max-width: 1200px;
}

.image-card {
  background-color: rgba(255, 255, 255, 0.8);
  border-radius: 15px;
  overflow: hidden;
  box-shadow: 0 4px 10px rgba(0, 0, 0, 0.1);
  display: flex;
  flex-direction: column;
}

.image-placeholder {
  width: 100%;
  aspect-ratio: 4/3;
  background-color: #eee;
  border-radius: 15px 15px 0 0;
  display: flex;
  justify-content: center;
  align-items: center;
  color: #999;
  font-size: 1rem;
  cursor: pointer;
}

.image {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.error-text {
  color: #e74c3c;
}

.image-actions {
  display: flex;
  justify-content: space-around;
  padding: 10px;
  border-top: 1px solid #eee;
}

.action-btn {
  padding: 8px 16px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 0.9rem;
}

.action-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.modal {
  position: fixed;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  background-color: rgba(0, 0, 0, 0.8);
  z-index: 1000;
  display: flex;
  justify-content: center;
  align-items: center;
}

.modal-content {
  max-width: 90%;
  max-height: 90%;
}

.close-btn {
  position: absolute;
  top: 20px;
  right: 30px;
  color: #fff;
  font-size: 2rem;
  cursor: pointer;
  transition: color 0.3s ease;
}

.close-btn:hover {
  color: #d2b48c;
}

@media (max-width: 768px) {
  header {
    flex-direction: column;
    gap: 10px;
  }

  .nav-links {
    flex-wrap: wrap;
    justify-content: center;
  }

  .image-grid {
    grid-template-columns: repeat(auto-fill, minmax(250px, 1fr));
  }
}

@media (max-width: 480px) {
  .image-grid {
    grid-template-columns: 1fr;
  }

  .action-btn {
    padding: 6px 12px;
    font-size: 0.8rem;
  }

  .header-left {
    flex-direction: column;
    gap: 8px;
  }
}
</style>

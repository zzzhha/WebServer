<script setup lang="ts">
import { computed, onMounted, ref } from 'vue'
import { storeToRefs } from 'pinia'
import { useAuthStore } from '@/stores/auth'
import { listFiles, type FileItem } from '@/api/files'
import { useToastStore } from '@/stores/toast'
import UserDropdownMenu from '@/components/UserDropdownMenu.vue'

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

const errored = ref<Record<string, boolean>>({})

const zoomOpen = ref(false)
const zoomSrc = ref('')

function onImageError(src: string) {
  errored.value = { ...errored.value, [src]: true }
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
  } else {
    toast.push('获取图片列表失败', 2000)
  }
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
    <div class="nav-controls">
      <a href="video.html" class="nav-btn">视频页面</a>
      <div class="auth-controls">
        <a v-if="!(username && token)" href="login.html" class="auth-btn">登录</a>
        <span v-if="!(username && token)" class="divider">|</span>
        <a v-if="!(username && token)" href="register.html" class="auth-btn">注册</a>
        <UserDropdownMenu v-else :username="username || ''" />
      </div>
    </div>
  </header>

  <div class="image-grid">
    <div v-for="item in images" :key="item.name" class="image-card">
      <div class="image-placeholder">
        <template v-if="errored[item.url]">
          <span class="error-text">图片加载失败</span>
        </template>
        <template v-else>
          <img :src="item.url" alt="" class="image" @error="onImageError(item.url)" />
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

.nav-controls {
  display: flex;
  gap: 15px;
  align-items: center;
}

.nav-btn {
  padding: 8px 16px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 1rem;
}

.nav-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.auth-controls {
  display: flex;
  gap: 15px;
  align-items: center;
}

.auth-btn {
  padding: 8px 16px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 1rem;
}

.auth-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.divider {
  margin: 0 5px;
  color: #999;
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

  .nav-controls {
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

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
const videos = computed(() => {
  if (remoteFiles.value && remoteFiles.value.length) {
    return remoteFiles.value.map((f, idx) => ({
      fileName: f.name,
      title: `${idx + 1}. ${f.name}`,
      downloadUrl: f.downloadUrl,
    }))
  }
  return [
    { fileName: 'diangunfukua.mp4', title: '1. 电棍浮夸倒放', downloadUrl: '/download/video/diangunfukua.mp4' },
    { fileName: 'mingweihouguoyu.mp4', title: '2. 名为侯国玉', downloadUrl: '/download/video/mingweihouguoyu.mp4' },
  ]
})

function downloadVideo(fileName: string) {
  const a = document.createElement('a')
  a.href = `/download/video/${fileName}`
  a.download = fileName
  a.style.display = 'none'
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
}

async function refresh() {
  const r = await listFiles('video')
  if (r.ok) remoteFiles.value = r.files
  else toast.push('获取视频列表失败', 2000)
}

onMounted(() => {
  refresh()
})
</script>

<template>
  <header>
    <div class="header-left">
      <a href="index.html" class="back-btn">← 返回主页</a>
      <div class="page-title">视频</div>
    </div>
    <div class="nav-controls">
      <a href="picture.html" class="nav-btn">图片页面</a>
      <div class="auth-controls">
        <a v-if="!(username && token)" href="login.html" class="auth-btn">登录</a>
        <span v-if="!(username && token)" class="divider">|</span>
        <a v-if="!(username && token)" href="register.html" class="auth-btn">注册</a>
        <UserDropdownMenu v-else :username="username || ''" />
      </div>
    </div>
  </header>

  <div class="video-list">
    <div v-for="v in videos" :key="v.fileName" class="video-item">
      <div class="video-name" role="button" tabindex="0" @click="downloadVideo(v.fileName)" @keydown.enter="downloadVideo(v.fileName)">
        {{ v.title }}
      </div>
      <p class="video-desc">点击名称或下载按钮，可下载该视频（不提供播放功能）</p>
      <button class="download-btn" type="button" @click="downloadVideo(v.fileName)">下载视频</button>
    </div>
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

.page-title {
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
  color: #999;
}

.video-list {
  max-width: 800px;
  margin: 0 auto;
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.video-item {
  background-color: rgba(255, 255, 255, 0.8);
  padding: 20px;
  border-radius: 10px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
  transition: transform 0.3s ease;
}

.video-item:hover {
  transform: translateY(-3px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}

.video-name {
  font-size: 1.2rem;
  color: #333;
  margin-bottom: 10px;
  cursor: pointer;
  display: inline-block;
  transition: color 0.3s ease;
}

.video-name:hover {
  color: #d2b48c;
}

.video-desc {
  color: #555;
  font-size: 0.9rem;
  margin-bottom: 15px;
}

.download-btn {
  padding: 6px 12px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 0.9rem;
}

.download-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

@media (max-width: 768px) {
  .nav-controls {
    flex-wrap: wrap;
    justify-content: center;
  }
}

@media (max-width: 480px) {
  header {
    flex-direction: column;
    gap: 10px;
  }

  .page-title {
    font-size: 1.3rem;
  }

  .video-item {
    padding: 15px;
  }

  .header-left {
    flex-direction: column;
    gap: 8px;
  }
}
</style>

<script setup lang="ts">
import { ref } from 'vue'
import { storeToRefs } from 'pinia'
import { Play, X } from 'lucide-vue-next'
import { useAuthStore } from '@/stores/auth'
import { usePagedFiles, type SortField, type SortOrder } from '@/composables/usePagedFiles'
import UserDropdownMenu from '@/components/UserDropdownMenu.vue'
import VideoPlayer from '@/components/VideoPlayer.vue'
import PaginationBar from '@/components/PaginationBar.vue'

const { username, token } = storeToRefs(useAuthStore())

const {
  files,
  total,
  page,
  totalPages,
  loading,
  error,
  sort,
  order,
  goto,
  setSort,
  setOrder,
  searchDebounced,
} = usePagedFiles('video', 12)

const activeSrc = ref<string | null>(null)
const posterError = ref<Record<string, boolean>>({})

function downloadVideo(fileName: string) {
  const a = document.createElement('a')
  a.href = `/download/video/${fileName}`
  a.download = fileName
  a.style.display = 'none'
  document.body.appendChild(a)
  a.click()
  document.body.removeChild(a)
}

function playVideo(downloadUrl: string) {
  activeSrc.value = downloadUrl
}

function stopPlayback() {
  activeSrc.value = null
}

function posterUrl(name: string) {
  return `/thumb/video/${name}`
}

function onPosterError(name: string) {
  posterError.value = { ...posterError.value, [name]: true }
}

const sortOptions: { label: string; value: SortField }[] = [
  { label: '时间', value: 'mtime' },
  { label: '名称', value: 'name' },
  { label: '大小', value: 'size' },
]

const orderOptions: { label: string; value: SortOrder }[] = [
  { label: '降序', value: 'desc' },
  { label: '升序', value: 'asc' },
]
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

  <div class="toolbar">
    <input
      class="search"
      type="text"
      placeholder="搜索视频名称…"
      @input="searchDebounced(($event.target as HTMLInputElement).value)"
    />
    <select class="sel" :value="sort" @change="setSort(($event.target as HTMLSelectElement).value as SortField)">
      <option v-for="o in sortOptions" :key="o.value" :value="o.value">{{ o.label }}</option>
    </select>
    <select class="sel" :value="order" @change="setOrder(($event.target as HTMLSelectElement).value as SortOrder)">
      <option v-for="o in orderOptions" :key="o.value" :value="o.value">{{ o.label }}</option>
    </select>
  </div>

  <div v-if="loading" class="state">加载中…</div>
  <div v-else-if="error" class="state">{{ error }}</div>
  <div v-else-if="files.length === 0" class="state">暂无视频</div>

  <div v-else class="video-list">
    <div v-for="v in files" :key="v.name" class="video-item">
      <div class="video-row">
        <div class="video-poster-wrap">
          <template v-if="posterError[v.name]">
            <span class="poster-fallback">无封面</span>
          </template>
          <template v-else>
            <img class="video-poster" :src="posterUrl(v.name)" :alt="v.name" loading="lazy" @error="onPosterError(v.name)" />
          </template>
        </div>
        <div class="video-info">
          <div class="video-name" role="button" tabindex="0" @click="playVideo(v.downloadUrl)" @keydown.enter="playVideo(v.downloadUrl)">
            {{ v.name }}
          </div>
          <p class="video-desc">点击“播放”在线观看，点击名称或“下载”按钮可下载该视频。</p>
          <div class="video-actions">
            <button
              class="play-btn"
              type="button"
              :class="{ active: activeSrc === v.downloadUrl }"
              @click="playVideo(v.downloadUrl)"
            >
              <Play :size="16" />
              {{ activeSrc === v.downloadUrl ? '播放中' : '播放' }}
            </button>
            <button class="download-btn" type="button" @click="downloadVideo(v.name)">下载视频</button>
          </div>
        </div>
      </div>

      <div v-if="activeSrc === v.downloadUrl" class="player-wrap">
        <div class="player-head">
          <span class="now-playing">正在播放：{{ v.name }}</span>
          <button class="close-btn" type="button" aria-label="收起播放器" @click="stopPlayback">
            <X :size="16" />
          </button>
        </div>
        <VideoPlayer :key="v.downloadUrl" :src="v.downloadUrl" :title="v.name" autoplay />
      </div>
    </div>
  </div>

  <PaginationBar :page="page" :total-pages="totalPages" :total="total" @change="goto" />
</template>

<style scoped>
header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 15px 20px;
  background-color: rgba(255, 255, 255, 0.8);
  border-radius: 10px;
  margin-bottom: 20px;
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

.toolbar {
  display: flex;
  gap: 12px;
  align-items: center;
  max-width: 900px;
  margin: 0 auto 20px;
  flex-wrap: wrap;
}

.search {
  flex: 1 1 220px;
  min-width: 180px;
  padding: 8px 12px;
  border: 1px solid #d9c4a0;
  border-radius: 8px;
  font-size: 0.9rem;
  background-color: rgba(255, 255, 255, 0.85);
}

.sel {
  padding: 8px 10px;
  border: 1px solid #d9c4a0;
  border-radius: 8px;
  font-size: 0.9rem;
  background-color: rgba(255, 255, 255, 0.85);
  color: #333;
  cursor: pointer;
}

.state {
  text-align: center;
  color: #888;
  padding: 60px 0;
  font-size: 1rem;
}

.video-list {
  max-width: 900px;
  margin: 0 auto;
  display: flex;
  flex-direction: column;
  gap: 18px;
}

.video-item {
  background-color: rgba(255, 255, 255, 0.8);
  padding: 16px;
  border-radius: 12px;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.1);
  transition: transform 0.3s ease;
}

.video-item:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}

.video-row {
  display: flex;
  gap: 16px;
  align-items: flex-start;
}

.video-poster-wrap {
  width: 168px;
  height: 94px;
  flex-shrink: 0;
  border-radius: 8px;
  overflow: hidden;
  background-color: #eee;
  display: flex;
  align-items: center;
  justify-content: center;
}

.video-poster {
  width: 100%;
  height: 100%;
  object-fit: cover;
}

.poster-fallback {
  color: #999;
  font-size: 0.85rem;
}

.video-info {
  flex: 1;
  min-width: 0;
}

.video-name {
  font-size: 1.15rem;
  color: #333;
  cursor: pointer;
  display: inline-block;
  transition: color 0.3s ease;
  word-break: break-all;
}

.video-name:hover {
  color: #d2b48c;
}

.video-desc {
  color: #555;
  font-size: 0.88rem;
  margin: 8px 0 12px;
}

.video-actions {
  display: flex;
  gap: 10px;
  align-items: center;
}

.play-btn,
.download-btn {
  display: inline-flex;
  align-items: center;
  gap: 6px;
  padding: 6px 14px;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  text-decoration: none;
  transition: all 0.3s ease;
  font-size: 0.9rem;
  color: #333;
}

.play-btn {
  background-color: #d2b48c;
}

.play-btn:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.play-btn.active {
  background-color: #c09871;
}

.download-btn {
  background-color: #eaddc4;
}

.download-btn:hover {
  background-color: #d9c4a0;
  transform: scale(1.05);
}

.player-wrap {
  margin-top: 14px;
}

.player-head {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 8px;
  margin-top: 8px;
}

.now-playing {
  font-size: 0.9rem;
  color: #333;
  font-weight: 600;
}

.close-btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 28px;
  height: 28px;
  border: none;
  background-color: transparent;
  color: #666;
  cursor: pointer;
  border-radius: 6px;
  transition: background-color 0.2s ease;
}

.close-btn:hover {
  background-color: #f3e5d2;
  color: #333;
}

@media (max-width: 768px) {
  .nav-controls {
    flex-wrap: wrap;
    justify-content: center;
  }

  .video-row {
    flex-direction: column;
  }

  .video-poster-wrap {
    width: 100%;
    height: auto;
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
    padding: 12px;
  }

  .header-left {
    flex-direction: column;
    gap: 8px;
  }
}
</style>

<script setup lang="ts">
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import {
  Play,
  Pause,
  Volume2,
  VolumeX,
  Maximize,
  Minimize,
} from 'lucide-vue-next'

const props = defineProps<{
  src: string
  title?: string
  autoplay?: boolean
}>()

const STORAGE_PREFIX = 'vp:'
const RATE_PRESETS = [0.5, 0.75, 1, 1.25, 1.5, 2] as const
// 距片尾小于该秒数时，不再续播（避免“秒结尾”卡在结尾），改为从头播放
const END_MARGIN_SECONDS = 5

const videoRef = ref<HTMLVideoElement | null>(null)
const isPlaying = ref(false)
const currentTime = ref(0)
const duration = ref(0)
const volume = ref(1)
const isMuted = ref(false)
const playbackRate = ref(1)
const isFullscreen = ref(false)
const isSeeking = ref(false)

let storage: Storage | null = null
if (typeof window !== 'undefined' && window.localStorage) storage = window.localStorage

function readNumber(key: string, fallback: number): number {
  const raw = storage?.getItem(key)
  if (!raw) return fallback
  const n = Number(raw)
  return Number.isFinite(n) ? n : fallback
}

function readBool(key: string, fallback: boolean): boolean {
  const raw = storage?.getItem(key)
  if (raw == null) return fallback
  return raw === '1'
}

function resumeKey(src: string) {
  return `${STORAGE_PREFIX}resume:${src}`
}

function loadResume(src: string): number {
  return readNumber(resumeKey(src), 0)
}

function persistResume() {
  if (!props.src || currentTime.value <= 0) return
  storage?.setItem(resumeKey(props.src), String(Math.floor(currentTime.value)))
}

function clearResume(src: string) {
  storage?.removeItem(resumeKey(src))
}

function formatTime(sec: number): string {
  if (!Number.isFinite(sec) || sec < 0) sec = 0
  const total = Math.floor(sec)
  const h = Math.floor(total / 3600)
  const m = Math.floor((total % 3600) / 60)
  const s = total % 60
  const mm = String(m).padStart(2, '0')
  const ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${mm}:${ss}` : `${mm}:${ss}`
}

function applyResumeAndPlay() {
  const video = videoRef.value
  if (!video || !duration.value) return
  const saved = loadResume(props.src)
  if (saved > 0) {
    // 接近片尾则从头播放并清除续播点
    if (duration.value - saved <= END_MARGIN_SECONDS) {
      clearResume(props.src)
      video.currentTime = 0
    } else {
      video.currentTime = saved
    }
  }
  if (props.autoplay) {
    void video.play().catch(() => {
      // 自动播放被浏览器策略拦截时静默忽略（用户可手动点击播放）
    })
  }
}

function onLoadedMetadata() {
  const video = videoRef.value
  if (video) {
    duration.value = video.duration || 0
    applyResumeAndPlay()
  }
}

function onTimeUpdate() {
  const video = videoRef.value
  if (video) currentTime.value = video.currentTime
  persistResume()
}

function onPlay() {
  isPlaying.value = true
}

function onPause() {
  isPlaying.value = false
  persistResume()
}

function onEnded() {
  isPlaying.value = false
  persistResume()
}

function syncAudioState() {
  const video = videoRef.value
  if (!video) return
  volume.value = video.volume
  isMuted.value = video.muted
  storage?.setItem(`${STORAGE_PREFIX}volume`, String(video.volume))
  storage?.setItem(`${STORAGE_PREFIX}muted`, video.muted ? '1' : '0')
}

function onVolumeChange() {
  syncAudioState()
}

function togglePlay() {
  const video = videoRef.value
  if (!video) return
  if (video.paused) {
    void video.play()
  } else {
    video.pause()
  }
}

function handleSeekInput(event: Event) {
  const video = videoRef.value
  const val = Number((event.target as HTMLInputElement).value)
  if (!video || !Number.isFinite(val)) return
  currentTime.value = val
  video.currentTime = val
}

function setVolumeFromInput(event: Event) {
  const video = videoRef.value
  const val = Number((event.target as HTMLInputElement).value)
  if (!video) return
  video.volume = val
  if (val > 0 && video.muted) video.muted = false
  syncAudioState()
}

function toggleMute() {
  const video = videoRef.value
  if (!video) return
  video.muted = !video.muted
  syncAudioState()
}

function setRate(rate: number) {
  const video = videoRef.value
  playbackRate.value = rate
  if (video) video.playbackRate = rate
  storage?.setItem(`${STORAGE_PREFIX}rate`, String(rate))
}

function toggleFullscreen() {
  const video = videoRef.value
  if (!video) return
  if (document.fullscreenElement) {
    void document.exitFullscreen()
  } else {
    void video.requestFullscreen?.()
  }
}

function handleFullscreenChange() {
  isFullscreen.value = document.fullscreenElement === videoRef.value
}

const progressPercent = computed(() =>
  duration.value > 0 ? (currentTime.value / duration.value) * 100 : 0,
)

const bufferedPercent = computed(() => {
  const video = videoRef.value
  if (!video || !duration.value || video.buffered.length === 0) return 0
  const end = video.buffered.end(video.buffered.length - 1)
  return Math.min((end / duration.value) * 100, 100)
})

watch(
  () => props.src,
  () => {
    currentTime.value = 0
    duration.value = 0
    onMountedApplyDefaults()
  },
  { immediate: true },
)

function onMountedApplyDefaults() {
  const video = videoRef.value
  if (!video) return
  video.volume = readNumber(`${STORAGE_PREFIX}volume`, 1)
  video.muted = readBool(`${STORAGE_PREFIX}muted`, false)
  playbackRate.value = readNumber(`${STORAGE_PREFIX}rate`, 1)
  video.playbackRate = playbackRate.value
}

onMounted(() => {
  onMountedApplyDefaults()
  document.addEventListener('fullscreenchange', handleFullscreenChange)
})

onBeforeUnmount(() => {
  persistResume()
  document.removeEventListener('fullscreenchange', handleFullscreenChange)
})

function onSeekStart() {
  isSeeking.value = true
}

function onSeekEnd() {
  isSeeking.value = false
  persistResume()
}

defineExpose({
  play: () => void videoRef.value?.play(),
  pause: () => videoRef.value?.pause(),
})
</script>

<template>
  <div class="player" :class="{ fullscreen: isFullscreen }">
    <video
      ref="videoRef"
      :src="src"
      preload="metadata"
      playsinline
      @loadedmetadata="onLoadedMetadata"
      @timeupdate="onTimeUpdate"
      @play="onPlay"
      @pause="onPause"
      @ended="onEnded"
      @volumechange="onVolumeChange"
    />

    <div class="controls">
      <div class="timeline" v-show="duration > 0">
        <div class="buffered" :style="{ width: bufferedPercent + '%' }"></div>
        <div class="progress" :style="{ width: progressPercent + '%' }"></div>
        <input
          class="seek"
          type="range"
          min="0"
          :max="duration || 0"
          step="0.1"
          :value="currentTime"
          :disabled="duration <= 0"
          aria-label="播放进度"
          @input="handleSeekInput"
          @pointerdown="onSeekStart"
          @pointerup="onSeekEnd"
          @change="onSeekEnd"
        />
      </div>

      <div class="bar">
        <button type="button" class="ctl" :aria-label="isPlaying ? '暂停' : '播放'" @click="togglePlay">
          <Pause v-if="isPlaying" :size="20" />
          <Play v-else :size="20" />
        </button>

        <div class="volume">
          <button
            type="button"
            class="ctl"
            :aria-label="isMuted ? '取消静音' : '静音'"
            @click="toggleMute"
          >
            <Volume2 v-if="!isMuted" :size="20" />
            <VolumeX v-else :size="20" />
          </button>
          <input
            class="vol"
            type="range"
            min="0"
            max="1"
            step="0.05"
            :value="isMuted ? 0 : volume"
            aria-label="音量"
            @input="setVolumeFromInput"
          />
        </div>

        <span class="time">{{ formatTime(currentTime) }} / {{ formatTime(duration) }}</span>

        <div class="rate">
          <label class="rate-label" for="rate-select">倍速</label>
          <select id="rate-select" class="rate-select" :value="playbackRate" @change="setRate(Number(($event.target as HTMLSelectElement).value))">
            <option v-for="r in RATE_PRESETS" :key="r" :value="r">{{ r }}x</option>
          </select>
        </div>

        <button
          type="button"
          class="ctl"
          :aria-label="isFullscreen ? '退出全屏' : '全屏'"
          @click="toggleFullscreen"
        >
          <Minimize v-if="isFullscreen" :size="20" />
          <Maximize v-else :size="20" />
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.player {
  position: relative;
  width: 100%;
  background-color: #000;
  border-radius: 10px;
  overflow: hidden;
  box-shadow: 0 4px 14px rgba(0, 0, 0, 0.18);
}

.player video {
  display: block;
  width: 100%;
  max-height: 60vh;
  background-color: #000;
  outline: none;
}

.player.fullscreen {
  position: fixed;
  inset: 0;
  z-index: 9999;
  border-radius: 0;
}

.player.fullscreen video {
  max-height: none;
  height: 100%;
}

.controls {
  position: absolute;
  left: 0;
  right: 0;
  bottom: 0;
  padding: 8px 12px 6px;
  background: linear-gradient(to top, rgba(0, 0, 0, 0.78), rgba(0, 0, 0, 0));
  color: #fff;
}

.timeline {
  position: relative;
  height: 16px;
  display: flex;
  align-items: center;
  margin-bottom: 4px;
  cursor: pointer;
}

.buffered {
  position: absolute;
  left: 0;
  top: 6px;
  height: 4px;
  background-color: rgba(255, 255, 255, 0.35);
  border-radius: 2px;
  pointer-events: none;
}

.progress {
  position: absolute;
  left: 0;
  top: 6px;
  height: 4px;
  background-color: #d2b48c;
  border-radius: 2px;
  pointer-events: none;
}

.seek {
  position: absolute;
  left: 0;
  right: 0;
  top: 0;
  width: 100%;
  height: 16px;
  margin: 0;
  opacity: 0;
  cursor: pointer;
}

.bar {
  display: flex;
  align-items: center;
  gap: 10px;
}

.ctl {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  width: 32px;
  height: 32px;
  border: none;
  background: transparent;
  color: #fff;
  cursor: pointer;
  border-radius: 6px;
  transition: background-color 0.2s ease;
  flex-shrink: 0;
}

.ctl:hover {
  background-color: rgba(255, 255, 255, 0.2);
}

.volume {
  display: flex;
  align-items: center;
  gap: 6px;
}

.vol {
  width: 80px;
  accent-color: #d2b48c;
}

.time {
  font-size: 0.8rem;
  color: rgba(255, 255, 255, 0.92);
  white-space: nowrap;
}

.rate {
  display: flex;
  align-items: center;
  gap: 6px;
  margin-left: auto;
}

.rate-label {
  font-size: 0.8rem;
  color: rgba(255, 255, 255, 0.9);
}

.rate-select {
  background-color: rgba(255, 255, 255, 0.15);
  color: #fff;
  border: 1px solid rgba(255, 255, 255, 0.3);
  border-radius: 6px;
  padding: 3px 6px;
  font-size: 0.82rem;
  cursor: pointer;
}

.rate-select option {
  color: #333;
}

@media (max-width: 560px) {
  .vol {
    width: 56px;
  }
  .rate-label {
    display: none;
  }
}
</style>

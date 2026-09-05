import { mount, type VueWrapper } from '@vue/test-utils'
import { nextTick } from 'vue'
import { beforeEach, describe, expect, it, vi } from 'vitest'
import VideoPlayer from '@/components/VideoPlayer.vue'

const RESUME_KEY = 'vp:resume:/v.mp4'

function mockVideo(wrapper: VueWrapper) {
  const video = wrapper.find('video').element as HTMLVideoElement & {
    play: ReturnType<typeof vi.fn>
    pause: ReturnType<typeof vi.fn>
  }
  let paused = true
  Object.defineProperty(video, 'paused', {
    configurable: true,
    get: () => paused,
  })
  Object.defineProperty(video, 'duration', { configurable: true, writable: true, value: NaN })
  Object.defineProperty(video, 'currentTime', { configurable: true, writable: true, value: 0 })
  Object.defineProperty(video, 'volume', { configurable: true, writable: true, value: 1 })
  Object.defineProperty(video, 'muted', { configurable: true, writable: true, value: false })
  Object.defineProperty(video, 'playbackRate', { configurable: true, writable: true, value: 1 })
  Object.defineProperty(video, 'buffered', {
    configurable: true,
    writable: true,
    value: { length: 0, end: () => 0 },
  })
  video.play = vi.fn(() => {
    paused = false
    return Promise.resolve()
  })
  video.pause = vi.fn(() => {
    paused = true
  })
  return video
}

function fireLoadedMetadata(video: HTMLVideoElement, duration: number) {
  Object.defineProperty(video, 'duration', { configurable: true, writable: true, value: duration })
  video.dispatchEvent(new Event('loadedmetadata'))
}

describe('VideoPlayer', () => {
  beforeEach(() => {
    localStorage.clear()
  })

  it('renders custom controls', () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    expect(wrapper.find('video').exists()).toBe(true)
    expect(wrapper.find('[aria-label="播放"]').exists()).toBe(true)
    expect(wrapper.find('[aria-label="音量"]').exists()).toBe(true)
    expect(wrapper.find('.rate-select').exists()).toBe(true)
  })

  it('changing playback rate persists to localStorage and applies to video', async () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    const video = mockVideo(wrapper)
    const select = wrapper.find('.rate-select')
    await select.setValue('1.5')
    expect(video.playbackRate).toBe(1.5)
    expect(localStorage.getItem('vp:rate')).toBe('1.5')
  })

  it('mute toggle updates video.muted and persists', async () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    const video = mockVideo(wrapper)
    await wrapper.find('[aria-label="静音"]').trigger('click')
    expect(video.muted).toBe(true)
    expect(localStorage.getItem('vp:muted')).toBe('1')
  })

  it('volume input updates video.volume and persists', async () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    const video = mockVideo(wrapper)
    const vol = wrapper.find('[aria-label="音量"]')
    await vol.setValue('0.5')
    expect(video.volume).toBe(0.5)
    expect(localStorage.getItem('vp:volume')).toBe('0.5')
  })

  it('applies saved resume position on play when not near the end', async () => {
    localStorage.setItem(RESUME_KEY, '30')
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4', autoplay: true } })
    const video = mockVideo(wrapper)
    fireLoadedMetadata(video, 100)
    expect(video.currentTime).toBe(30)
    expect(video.play).toHaveBeenCalled()
  })

  it('starts from beginning and clears resume when saved position is near the end', async () => {
    localStorage.setItem(RESUME_KEY, '98')
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4', autoplay: true } })
    const video = mockVideo(wrapper)
    fireLoadedMetadata(video, 100)
    expect(video.currentTime).toBe(0)
    expect(localStorage.getItem(RESUME_KEY)).toBeNull()
  })

  it('seek input sets video.currentTime', async () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    const video = mockVideo(wrapper)
    Object.defineProperty(video, 'duration', { configurable: true, writable: true, value: 100 })
    video.dispatchEvent(new Event('loadedmetadata'))
    await nextTick()
    await wrapper.find('.seek').setValue('42')
    expect(video.currentTime).toBe(42)
  })

  it('formats time text as mm:ss', async () => {
    const wrapper = mount(VideoPlayer, { props: { src: '/v.mp4' } })
    const video = mockVideo(wrapper)
    Object.defineProperty(video, 'duration', { configurable: true, writable: true, value: 100 })
    Object.defineProperty(video, 'currentTime', { configurable: true, writable: true, value: 65 })
    video.dispatchEvent(new Event('loadedmetadata'))
    video.dispatchEvent(new Event('timeupdate'))
    await nextTick()
    expect(wrapper.find('.time').text()).toBe('01:05 / 01:40')
  })
})

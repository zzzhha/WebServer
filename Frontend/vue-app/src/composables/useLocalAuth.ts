import { onMounted, onUnmounted, ref } from 'vue'

export function useLocalAuth() {
  const username = ref<string | null>(null)
  const token = ref<string | null>(null)

  const sync = () => {
    username.value = localStorage.getItem('username')
    token.value = localStorage.getItem('jwt_token')
  }

  const clear = () => {
    localStorage.removeItem('username')
    localStorage.removeItem('jwt_token')
    sync()
  }

  onMounted(() => {
    sync()
    window.addEventListener('storage', sync)
  })

  onUnmounted(() => {
    window.removeEventListener('storage', sync)
  })

  return {
    username,
    token,
    isLoggedIn: () => Boolean(username.value && token.value),
    sync,
    clear,
  }
}


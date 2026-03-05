import { defineStore } from 'pinia'
import { computed, ref } from 'vue'

type ToastItem = {
  id: string
  text: string
}

export const useToastStore = defineStore('toast', () => {
  const items = ref<ToastItem[]>([])

  const visibleItems = computed(() => items.value)

  function push(text: string, ms = 2000) {
    const id = `${Date.now()}-${Math.random().toString(16).slice(2)}`
    items.value = [...items.value, { id, text }]
    window.setTimeout(() => {
      items.value = items.value.filter((t) => t.id !== id)
    }, ms)
  }

  return {
    visibleItems,
    push,
  }
})


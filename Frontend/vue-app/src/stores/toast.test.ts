import { createPinia, setActivePinia } from 'pinia'
import { beforeEach, describe, expect, it } from 'vitest'
import { useToastStore } from '@/stores/toast'

describe('toast store', () => {
  beforeEach(() => {
    setActivePinia(createPinia())
  })

  it('push adds an item', () => {
    const toast = useToastStore()
    toast.push('hello', 10)
    expect(toast.visibleItems.length).toBe(1)
    expect(toast.visibleItems[0].text).toBe('hello')
  })
})


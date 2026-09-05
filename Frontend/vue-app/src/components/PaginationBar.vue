<script setup lang="ts">
import { computed } from 'vue'

const props = defineProps<{
  page: number
  totalPages: number
  total: number
}>()

const emit = defineEmits<{ (e: 'change', page: number): void }>()

const pages = computed<number[]>(() => {
  const tp = props.totalPages
  if (tp <= 0) return []
  const cur = props.page
  // 显示以当前页为首的若干页，足够用户跳转
  const start = Math.max(1, Math.min(cur - 2, tp - 4))
  const end = Math.min(tp, start + 4)
  const arr: number[] = []
  for (let i = start; i <= end; i++) arr.push(i)
  return arr
})

function go(p: number) {
  if (p < 1 || p > props.totalPages || p === props.page) return
  emit('change', p)
}
</script>

<template>
  <div class="pagination" v-if="totalPages > 0">
    <span class="info">共 {{ total }} 条</span>
    <button class="pg-btn" :disabled="page <= 1" @click="go(page - 1)">上一页</button>
    <button v-for="p in pages" :key="p" class="pg-btn" :class="{ active: p === page }" @click="go(p)">
      {{ p }}
    </button>
    <button class="pg-btn" :disabled="page >= totalPages" @click="go(page + 1)">下一页</button>
  </div>
</template>

<style scoped>
.pagination {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
  margin-top: 20px;
  justify-content: center;
}

.info {
  color: #666;
  font-size: 0.85rem;
  margin-right: 6px;
}

.pg-btn {
  min-width: 32px;
  padding: 6px 10px;
  border: none;
  background-color: #efe6d4;
  color: #333;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.85rem;
  transition: all 0.2s ease;
}

.pg-btn:hover:not(:disabled) {
  background-color: #d2b48c;
}

.pg-btn.active {
  background-color: #d2b48c;
  font-weight: 700;
}

.pg-btn:disabled {
  opacity: 0.45;
  cursor: not-allowed;
}
</style>

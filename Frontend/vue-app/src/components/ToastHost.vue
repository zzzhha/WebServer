<script setup lang="ts">
import { useToastStore } from '@/stores/toast'

const toast = useToastStore()
</script>

<template>
  <div class="toast-layer" aria-live="polite" aria-relevant="additions removals">
    <div v-for="t in toast.visibleItems" :key="t.id" class="toast">
      {{ t.text }}
    </div>
  </div>
</template>

<style scoped>
.toast-layer {
  position: fixed;
  left: 50%;
  top: 24px;
  transform: translateX(-50%);
  z-index: 9999;
  display: flex;
  flex-direction: column;
  gap: 10px;
  pointer-events: none;
}

.toast {
  padding: 10px 14px;
  border-radius: 10px;
  background: rgba(0, 0, 0, 0.78);
  color: #fff;
  font-size: 14px;
  max-width: 86vw;
  white-space: pre-line;
}
</style>


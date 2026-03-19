<script setup lang="ts">
import { useAuthStore } from '@/stores/auth'

defineProps<{
  username: string
}>()

const auth = useAuthStore()

function logout() {
  auth.clear()
}
</script>

<template>
  <div class="user-menu">
    <button type="button" class="user-trigger">欢迎您，{{ username }}</button>
    <div class="dropdown">
      <button type="button" class="dropdown-item" @click="logout">退出登录</button>
    </div>
  </div>
</template>

<style scoped>
.user-menu {
  position: relative;
  display: inline-flex;
  align-items: center;
}

.user-trigger {
  padding: 8px 16px;
  background-color: #d2b48c;
  color: #333;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-size: 1rem;
  transition: all 0.3s ease;
}

.user-trigger:hover {
  background-color: #c09871;
  transform: scale(1.05);
}

.dropdown {
  position: absolute;
  top: calc(100% + 8px);
  right: 0;
  min-width: 120px;
  background-color: rgba(255, 255, 255, 0.98);
  border-radius: 8px;
  box-shadow: 0 8px 18px rgba(0, 0, 0, 0.12);
  padding: 6px;
  opacity: 0;
  visibility: hidden;
  transform: translateY(-4px);
  transition: all 0.2s ease;
  z-index: 20;
}

.user-menu:hover .dropdown,
.user-menu:focus-within .dropdown {
  opacity: 1;
  visibility: visible;
  transform: translateY(0);
}

.dropdown-item {
  width: 100%;
  border: none;
  background: transparent;
  color: #333;
  border-radius: 6px;
  padding: 8px 10px;
  text-align: left;
  cursor: pointer;
  font-size: 0.95rem;
}

.dropdown-item:hover {
  background-color: #f3e5d2;
}
</style>

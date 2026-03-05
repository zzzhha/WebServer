import { expect, test } from '@playwright/test'

test('welcome page renders', async ({ page }) => {
  await page.goto('/welcome.html')
  await expect(page.getByText('欢迎来到我的小窝')).toBeVisible()
})

test('home page renders', async ({ page }) => {
  await page.goto('/index.html')
  await expect(page.getByText('内容导航')).toBeVisible()
})


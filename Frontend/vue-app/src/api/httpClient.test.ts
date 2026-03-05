import { describe, expect, it } from 'vitest'
import { toFormUrlEncoded } from '@/api/httpClient'

describe('httpClient helpers', () => {
  it('toFormUrlEncoded encodes key-values', () => {
    const p = toFormUrlEncoded({ username: 'a b', password: 'p@ss' })
    expect(p.toString()).toContain('username=a+b')
    expect(p.toString()).toContain('password=p%40ss')
  })
})


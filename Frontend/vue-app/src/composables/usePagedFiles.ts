import { onMounted, ref, watch, type Ref } from 'vue'
import { listFiles, type FileItem } from '@/api/files'

export type SortField = 'mtime' | 'name' | 'size'
export type SortOrder = 'desc' | 'asc'
type FileFolder = 'images' | 'video' | 'uploads'

/**
 * 分页 + 排序 + 名称搜索 的文件列表组合式函数。
 * 内部维护 files/total/page/totalPages/loading，并提供换页/排序/搜索入口。
 */
export function usePagedFiles(folder: Ref<FileFolder> | FileFolder, initialPageSize = 50) {
  const files = ref<FileItem[]>([])
  const total = ref(0)
  const page = ref(1)
  const pageSize = ref(initialPageSize > 0 ? initialPageSize : 50)
  const totalPages = ref(0)
  const loading = ref(false)
  const error = ref('')

  const sort: Ref<SortField> = ref('mtime')
  const order: Ref<SortOrder> = ref('desc')
  const q = ref('')

  async function load() {
    loading.value = true
    error.value = ''
    const folderValue = typeof folder === 'string' ? folder : folder.value
    const r = await listFiles(folderValue, {
      page: page.value,
      pageSize: pageSize.value,
      sort: sort.value === 'mtime' ? undefined : sort.value,
      order: order.value,
      q: q.value || undefined,
    })
    if (r.ok) {
      files.value = r.data.files
      total.value = r.data.total
      page.value = r.data.page
      pageSize.value = r.data.pageSize
      totalPages.value = r.data.totalPages
    } else {
      error.value = '获取列表失败'
      files.value = []
      total.value = 0
      totalPages.value = 0
    }
    loading.value = false
  }

  function goto(p: number) {
    if (p < 1) p = 1
    if (totalPages.value > 0 && p > totalPages.value) p = totalPages.value
    if (p === page.value && files.value.length) return
    page.value = p
    void load()
  }

  function setPageSize(size: number) {
    pageSize.value = size
    page.value = 1
    void load()
  }

  function setSort(field: SortField) {
    if (field === sort.value) return
    sort.value = field
    page.value = 1
    void load()
  }

  function setOrder(o: SortOrder) {
    if (o === order.value) return
    order.value = o
    page.value = 1
    void load()
  }

  function setSearch(value: string) {
    if (value === q.value) return
    q.value = value
    page.value = 1
    void load()
  }

  let timer: ReturnType<typeof setTimeout> | undefined
  function searchDebounced(value: string) {
    if (timer) clearTimeout(timer)
    timer = setTimeout(() => setSearch(value), 300)
  }

  watch(
    typeof folder === 'string' ? () => folder : folder,
    () => {
      page.value = 1
      void load()
    },
  )

  onMounted(load)

  return {
    files,
    total,
    page,
    pageSize,
    totalPages,
    loading,
    error,
    sort,
    order,
    q,
    load,
    goto,
    setPageSize,
    setSort,
    setOrder,
    setSearch,
    searchDebounced,
  }
}

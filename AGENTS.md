# Repository Guidelines

## Project Structure & Module Organization

```
WebServer/
├── CMakeLists.txt              # Root build config (C++17, CMake)
├── cppBackend/                 # C++ backend
│   ├── reactor/                # Event-driven core (Epoll, EventLoop, Channel, ThreadPool)
│   ├── http/                   # HTTP facade, parsers, router, security
│   ├── services/               # File API, auth, upload, download
│   ├── views/                  # Page handlers
│   ├── download/               # Chunked download manager
│   ├── timer/                  # Heap timer & time wheel
│   ├── MemoryPool/             # Thread-local + central cache pool
│   ├── mysql/                  # DB pool & DAO layer
│   ├── auth/jwt/               # JWT utilities
│   └── logger/                 # Async lock-free logger
├── Frontend/vue-app/           # Vue 3 + TypeScript SPA (Vite, Pinia, Tailwind)
├── test/                       # C++ Google Test binaries
├── api-docs/                   # OpenAPI spec & Redoc HTML
├── docs/                       # Architecture & design docs
└── reactor_docs/               # Reactor deep-dive documentation
```

## ThreadPool 关键说明

详见 [Reactor模块参考 SKILL](file:///home/zsy/WebServer/.trae/skills/Reactor模块参考/SKILL.md) 或 [concurrency-review.md#问题5](file:///home/zsy/WebServer/docs/concurrency-review/sequential-transfer-concurrency-review.md#%E4%BA%94%E9%97%AE%E9%A2%98-5threadpool-%E5%85%A8%E5%B1%80%E9%94%81%E7%AB%9E%E4%BA%89%E5%AF%BC%E8%87%B4-addtask-%E6%88%90%E4%B8%BA%E5%90%9E%E5%90%90%E7%93%B6%E9%A2%88)

架构：per-worker deque + inject_queue + work-stealing，替代原全局单队列。
- `addTask(Task) → bool`：新入口，支持背压反馈
- `addtask(function<void()>)`：兼容保留，内部转调 addTask
- 外部线程提交走 inject_queue；worker 内部提交直入本地 deque（零全局锁）
- 执行优先级：本地 pop → batch drain inject → steal → cv.wait
- `Task` 结构体已预留 priority/trace_id/affinity/cancel 字段，后续通过扩展 `TaskPriority` 枚举和 `affinity` hash 实现多级调度与任务亲和

---

## Build, Test, and Development Commands

### Backend (C++)

```bash
cmake -B build && cmake --build build -j$(nproc)   # Configure & build
ctest --test-dir build --output-on-failure          # Run all tests
./build/test/http_facade_tests                       # Run a specific test
```

### Frontend (Vue 3)

```bash
cd Frontend/vue-app
npm install         # Install dependencies
npm run dev         # Start Vite dev server
npm run build       # Production build
npm run test:run    # Run Vitest (no watch)
npm run lint        # ESLint check
```

---

## Coding Style & Naming Conventions

- **C++**: C++17. Follow existing style — `camelCase` for functions/variables, `PascalCase` for classes, 4-space indentation. No `using namespace std;` in headers.
- **Vue/TypeScript**: PascalCase for components, camelCase for variables. Composables prefixed with `use`. ESLint rules apply.
- **File naming**: `<module>.cpp`/`.h` for C++, `<module>.ts`/`.vue` for frontend. Test files: `<module>_tests.cpp` (unit) or `<scenario>_integration_tests.cpp`.

---

## Testing Guidelines

- **Frameworks**: Google Test (C++), Vitest (Vue), Playwright (E2E).
- **Naming**: Test cases in `snake_case`. Test files follow `<module>_tests.cpp` or `<scenario>_integration_tests.cpp`.
- **Coverage**: Unit tests cover individual modules; integration tests cover multi-module scenarios (worker offload, error handling, reactor).

---

## Commit & Pull Request Guidelines

Commit messages use a tag-based format from the project history:

```
[type][priority] Short description
```

- **Type**: `feat`, `fix`, `refactor`, `docs`, `test`, `chore`
- **Priority**: `P0` (critical), `P1` (high), `P2` (medium), `P3` (low)
- **Description**: Concise Chinese or English summary

Examples:

```
[fix][P0] 修复 HttpFacade 并发串扰并重构测试构建结构
[feat][P1] Add middle and large memory in memory_pool
[refactor][P0] Refactor the usage specifications of the router
```

PRs should include a description, link to related issues, and note any breaking API changes. Prefer small, focused commits.

---

## Agent-Specific Instructions

- `AGENTS.md` scope follows the standard Codex CLI spec: the file applies to its entire subtree.
- Shell commands: use `bash -lc` and set `workdir` explicitly.
- Prefer `rg` over `grep` for fast text search.

Repository Exploration Budget
Keep repository exploration shallow unless the user explicitly asks for a deep audit, debugging session, implementation, or code review.

Default exploration budget:

Run at most 5 shell commands before answering.
Prefer git status --short, git log --oneline -5, README.md, package.json, and directly relevant files.
Do not scan the whole repository with broad find, broad rg, recursive cat, or multi-package file walks unless necessary.
Do not inspect more than 3 recent commits unless the user asks about history or release context.
Do not inspect generated, vendored, build, cache, or dependency directories.
Stop criteria:

Stop exploring and answer once there is enough evidence for a useful response.
If more context would be helpful but not essential, state the assumption instead of continuing to call tools.
If a request is ambiguous, ask a concise clarification instead of expanding the search.
For proactive, background, suggestion, title-generation, or recommendation tasks:

Use at most 3 shell commands.
Prefer current git status, the last 5 commits, and obvious untracked files.
Generate suggestions from visible high-signal context only.
Do not perform test coverage audits unless the user explicitly asks for test recommendations.
If the task cannot be completed with shallow context, return fewer suggestions or no suggestions.
Preferred targeted commands:

git status --short
git log --oneline -5
ls
rg --files <specific-dir>
sed -n '1,120p' <specific-file>
Avoid broad exploratory commands by default:

find packages -type f ...
rg <term> packages/ without a narrow directory
git log --all -30
git show --stat on many commits
whole-file cat on large files
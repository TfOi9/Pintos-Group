## Fork 实现指南（对齐当前代码状态）

本文基于当前仓库状态编写：

- exec 和 wait 已经通过测试
- child_sync 已经落地，父子同步与退出回收逻辑已建立
- syscall 层还没有 SYS_FORK 分支
- process 层还没有 process_fork 和 start_fork 路径

目标是把 fork 接到现有框架上，而不是重写一整套进程控制逻辑。

---

## 现有基础可直接复用

当前代码里已经有一批可以直接复用的能力：

- child_sync_create, child_sync_get, child_sync_put
- child_sync_set_load, child_sync_set_exit
- child_sync_find, process_release_children
- process_execute 里的“父等待子加载完成”模式
- process_wait 里的“waited 一次性语义”
- process_exit 里的“统一 exit 打印和 child_sync 通知”

这些能力意味着 fork 只需要新增“子进程克隆路径”，不需要再发明新的同步协议。

---

## 需要新增的关键入口

### syscall 层入口

在 src/userprog/syscall.c 的 switch 中新增 SYS_FORK 分支。

该分支不需要读取用户参数，只需要调用 process_fork，并把当前的中断帧传下去。

推荐形态：

- f->eax = process_fork(f)

这样可以把“父返回子 pid、子返回 0”的寄存器语义放到 process_fork 路径里统一处理。

### process 层公开接口

在 src/userprog/process.h 新增声明：

- pid_t process_fork(struct intr_frame* parent_if)

并在 src/userprog/process.c 实现。

---

## 建议新增的数据结构

当前 process_execute 用 process_args 并把 child_sync 塞在 argv 末尾。fork 建议不要复用这一技巧，单独建结构更清晰。

建议在 src/userprog/process.c 新增：

- struct fork_args

字段建议：

- struct intr_frame parent_if
- struct child_sync* cs
- struct process* parent_pcb
- char child_name[16]

各字段意义：

- parent_if 保存父进程在 int 0x30 时的用户态寄存器快照，子进程将从同一点继续执行
- cs 复用当前 child_sync 协议，负责父等待子克隆完成
- parent_pcb 用于克隆父地址空间和父进程资源
- child_name 用于 thread_create 命名，便于调试和输出对齐

---

## 需要新增的工具函数（核心）

下面这些函数是 fork 成功率和可维护性的关键。

### 克隆地址空间相关

建议新增：

- static bool clone_pagedir(struct process* child_pcb, struct process* parent_pcb)
- static bool clone_one_user_page(uint32_t* child_pd, uint32_t* parent_pd, void* upage)
- static bool is_user_page_writable(uint32_t* pd, const void* upage)

实现原则：

- 只克隆用户地址空间，不碰内核映射
- 每个用户页都要新分配一张物理页并 memcpy 内容
- 页权限要尽量保持与父页一致，尤其是只读页

为什么需要 is_user_page_writable：

- 现有 pagedir_get_page 只能得到映射到的内核地址，不能直接告诉你写权限
- fork 若把只读页错误地映射成可写，可能导致隐藏测试失败

可选实现位置：

- 快速方案：在 process.c 内部通过页表走访拿 PTE_W
- 更整洁方案：在 pagedir.c 新增对外 helper，process.c 只调用接口

### 子进程启动相关

建议新增：

- static void start_fork(void* aux) NO_RETURN
- static bool init_child_pcb_for_fork(struct thread* t, struct child_sync* cs, const char* name)

实现原则：

- start_fork 负责完整的“子进程内核态初始化 + 克隆 + 唤醒父 + 跳 intr_exit”
- 任意失败都要设置 child_sync load 失败，并以 -1 退出子进程

### 回滚与清理相关

建议新增：

- static void free_fork_args(struct fork_args* fa)
- static void rollback_child_after_failed_clone(struct thread* t)

实现原则：

- 任意阶段失败都要保证不会泄漏页帧、child_sync 引用、fork_args
- 父进程只能看到“fork 返回 -1”，不能被挂死

---

## 可能要新增的额外实用函数

这部分与当前项目后续测试覆盖有关。

### 页表访问 helper

若你不想在 process.c 写过多页表细节，建议在 src/userprog/pagedir.c 增补：

- bool pagedir_is_writable(uint32_t* pd, const void* upage)
- bool pagedir_for_each_user_page(uint32_t* pd, callback, aux)

这样 clone_pagedir 会更简洁。

### 文件描述符克隆 helper（如果你已经实现文件类 syscall）

若你已经支持 open/read/write/seek/tell/close，fork 还需要保证描述符语义。

建议新增：

- static bool clone_fd_table(struct process* child_pcb, struct process* parent_pcb)

实现原则：

- 父子继承同一打开文件语义（偏移共享）
- 每个 fd 号在父子进程内独立管理，但底层 file 对象引用关系正确

如果当前项目还未实现 fd 表，这一块可以暂不接入。

---

## fork 详细执行流程（贴合当前 child_sync 体系）

### 父进程路径

父进程进入 SYS_FORK 后，process_fork 执行以下逻辑：

- 为新子进程创建 child_sync，父持有一份引用
- 构造 fork_args，拷贝 parent_if 到 fork_args.parent_if
- 给子线程预留 child_sync 的第二份引用
- 调用 thread_create 启动 start_fork
- 若 thread_create 失败，释放两份引用并返回 -1
- thread_create 成功后，将 child_sync 挂入当前进程 children 列表
- 父进程阻塞等待 cs->load_sema
- 子进程完成克隆后写 load_success 并唤醒父
- 若 load_success 为假，父从 children 删除该 cs，释放父引用，返回 -1
- 若 load_success 为真，返回子 pid

这个流程和你现有 process_execute 的同步模型一致，维护成本最低。

### 子进程路径

start_fork 执行以下逻辑：

- 从 fork_args 取出 parent_if, parent_pcb, cs
- 分配并初始化子 PCB（exit_status=-1, children 空表, self_sync=cs）
- 创建子 pagedir
- 克隆父进程全部用户页到子 pagedir
- 若项目已实现 fd 表，克隆 fd 表
- 准备子进程 intr_frame：
  - 以 parent_if 为模板完整拷贝
  - 强制 child_if.eax = 0
- 调用 child_sync_set_load(cs, true) 唤醒父
- 释放 fork_args
- 用 child_if 跳转到 intr_exit，进入用户态

### 任意失败路径

start_fork 任意阶段失败都必须执行：

- child_sync_set_load(cs, false)
- set_process_exit_status(-1)
- process_exit()

这样父进程一定会被唤醒，且 wait 能拿到正确退出状态，不会出现死等。

---

## 关键并发与一致性细节

- child_sync 的 load 状态和 exit 状态都必须先写字段再 sema_up
- process_wait 依赖 waited 标志保证一次性消费，fork 不能绕开这条约束
- process_exit 里通知 self_sync 的逻辑要保持不变，fork 子进程最终也走同一条退出通道
- 任何列表操作都要保证节点生命周期有效，避免先 free 后 list_remove

---

## 与当前代码衔接时的注意点

- process_execute 当前用 argv 末尾塞 child_sync 的方式对 exec 可工作，但 fork 建议单独 fork_args，避免类型混杂
- SYS_EXEC 分支里把 f->eax 写进父进程 exit_status 的行为不建议沿用到 fork；fork 不应修改父进程 exit_status
- read_user_u8 中变量 page 未使用，不影响 fork 语义，但可以顺手清理

---

## 最小可落地实现路径

推荐按这条顺序实施，调试成本最低：

- 先加 process_fork 接口与 SYS_FORK 分支，让编译通过
- 再加 fork_args 与 start_fork 空实现，先跑到父返回 -1
- 再实现 child pcb 初始化与 child_sync load 唤醒
- 再实现地址空间克隆
- 最后补上失败回滚和边界清理

每完成一层就跑 fork-simple, fork-nested, fork-bad，再逐步推进到 fork-tree 和文件相关 fork 测试。

---

## 你大概率会用到的现有函数速查

- thread_create
- thread_current
- thread_exit
- palloc_get_page, palloc_free_page
- pagedir_create, pagedir_set_page, pagedir_get_page, pagedir_destroy
- list_begin, list_next, list_remove, list_push_front, list_entry
- lock_init, lock_acquire, lock_release
- sema_init, sema_down, sema_up
- set_process_exit_status
- process_exit

以上函数已经覆盖了 fork 所需的大多数控制流、同步和资源管理能力。
/* /drivers/android/binder.c
 * binder.c
 *
 * Android IPC Subsystem
 *
 * Copyright (C) 2007-2008 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <asm/cacheflush.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/freezer.h>
#include <linux/fs.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/nsproxy.h>
#include <linux/poll.h>
#include <linux/debugfs.h>
#include <linux/rbtree.h>
#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/pid_namespace.h>
#include <linux/security.h>

#ifdef CONFIG_ANDROID_BINDER_IPC_32BIT
#define BINDER_IPC_32BIT 1
#endif

#include <uapi/linux/android/binder.h>
#include "binder_trace.h"

static DEFINE_MUTEX(binder_main_lock);
static DEFINE_MUTEX(binder_deferred_lock);
static DEFINE_MUTEX(binder_mmap_lock);

static HLIST_HEAD(binder_procs);
static HLIST_HEAD(binder_deferred_list);
static HLIST_HEAD(binder_dead_nodes);

static struct dentry *binder_debugfs_dir_entry_root;
static struct dentry *binder_debugfs_dir_entry_proc;
static struct binder_node *binder_context_mgr_node;//service manager的binder_node
static kuid_t binder_context_mgr_uid = INVALID_UID;
static int binder_last_id;
static struct workqueue_struct *binder_deferred_workqueue;

#define BINDER_DEBUG_ENTRY(name) \
static int binder_##name##_open(struct inode *inode, struct file *file) \
{ \
	return single_open(file, binder_##name##_show, inode->i_private); \
} \
\
static const struct file_operations binder_##name##_fops = { \
	.owner = THIS_MODULE, \
	.open = binder_##name##_open, \
	.read = seq_read, \
	.llseek = seq_lseek, \
	.release = single_release, \
}

static int binder_proc_show(struct seq_file *m, void *unused);
BINDER_DEBUG_ENTRY(proc);

/* This is only defined in include/asm-arm/sizes.h */
#ifndef SZ_1K
#define SZ_1K                               0x400
#endif

#ifndef SZ_4M
#define SZ_4M                               0x400000
#endif

#define FORBIDDEN_MMAP_FLAGS                (VM_WRITE)

#define BINDER_SMALL_BUF_SIZE (PAGE_SIZE * 64)

enum {
	BINDER_DEBUG_USER_ERROR             = 1U << 0,
	BINDER_DEBUG_FAILED_TRANSACTION     = 1U << 1,
	BINDER_DEBUG_DEAD_TRANSACTION       = 1U << 2,
	BINDER_DEBUG_OPEN_CLOSE             = 1U << 3,
	BINDER_DEBUG_DEAD_BINDER            = 1U << 4,
	BINDER_DEBUG_DEATH_NOTIFICATION     = 1U << 5,
	BINDER_DEBUG_READ_WRITE             = 1U << 6,
	BINDER_DEBUG_USER_REFS              = 1U << 7,
	BINDER_DEBUG_THREADS                = 1U << 8,
	BINDER_DEBUG_TRANSACTION            = 1U << 9,
	BINDER_DEBUG_TRANSACTION_COMPLETE   = 1U << 10,
	BINDER_DEBUG_FREE_BUFFER            = 1U << 11,
	BINDER_DEBUG_INTERNAL_REFS          = 1U << 12,
	BINDER_DEBUG_BUFFER_ALLOC           = 1U << 13,
	BINDER_DEBUG_PRIORITY_CAP           = 1U << 14,
	BINDER_DEBUG_BUFFER_ALLOC_ASYNC     = 1U << 15,
};
static uint32_t binder_debug_mask = BINDER_DEBUG_USER_ERROR |
	BINDER_DEBUG_FAILED_TRANSACTION | BINDER_DEBUG_DEAD_TRANSACTION;
module_param_named(debug_mask, binder_debug_mask, uint, S_IWUSR | S_IRUGO);

static bool binder_debug_no_lock;
module_param_named(proc_no_lock, binder_debug_no_lock, bool, S_IWUSR | S_IRUGO);

static DECLARE_WAIT_QUEUE_HEAD(binder_user_error_wait);
static int binder_stop_on_user_error;

static int binder_set_stop_on_user_error(const char *val,
					 struct kernel_param *kp)
{
	int ret;

	ret = param_set_int(val, kp);
	if (binder_stop_on_user_error < 2)
		wake_up(&binder_user_error_wait);
	return ret;
}
module_param_call(stop_on_user_error, binder_set_stop_on_user_error,
	param_get_int, &binder_stop_on_user_error, S_IWUSR | S_IRUGO);

#define binder_debug(mask, x...) \
	do { \
		if (binder_debug_mask & mask) \
			pr_info(x); \
	} while (0)

#define binder_user_error(x...) \
	do { \
		if (binder_debug_mask & BINDER_DEBUG_USER_ERROR) \
			pr_info(x); \
		if (binder_stop_on_user_error) \
			binder_stop_on_user_error = 2; \
	} while (0)

enum binder_stat_types {
	BINDER_STAT_PROC,
	BINDER_STAT_THREAD,
	BINDER_STAT_NODE,
	BINDER_STAT_REF,
	BINDER_STAT_DEATH,
	BINDER_STAT_TRANSACTION,
	BINDER_STAT_TRANSACTION_COMPLETE,
	BINDER_STAT_COUNT
};

struct binder_stats {
	int br[_IOC_NR(BR_FAILED_REPLY) + 1];
	int bc[_IOC_NR(BC_DEAD_BINDER_DONE) + 1];
	int obj_created[BINDER_STAT_COUNT];
	int obj_deleted[BINDER_STAT_COUNT];
};

static struct binder_stats binder_stats;

static inline void binder_stats_deleted(enum binder_stat_types type)
{
	binder_stats.obj_deleted[type]++;
}

static inline void binder_stats_created(enum binder_stat_types type)
{
	binder_stats.obj_created[type]++;
}

struct binder_transaction_log_entry {
	int debug_id;
	int call_type;
	int from_proc;
	int from_thread;
	int target_handle;
	int to_proc;
	int to_thread;
	int to_node;
	int data_size;
	int offsets_size;
};
struct binder_transaction_log {
	int next;
	int full;
	struct binder_transaction_log_entry entry[32];
};
static struct binder_transaction_log binder_transaction_log;
static struct binder_transaction_log binder_transaction_log_failed;

static struct binder_transaction_log_entry *binder_transaction_log_add(
	struct binder_transaction_log *log)
{
	struct binder_transaction_log_entry *e;

	e = &log->entry[log->next];
	memset(e, 0, sizeof(*e));
	log->next++;
	if (log->next == ARRAY_SIZE(log->entry)) {
		log->next = 0;
		log->full = 1;
	}
	return e;
}

//进程、线程todo、等待队列的节点
struct binder_work {
	struct list_head entry;//双向链表结构
	enum {
		BINDER_WORK_TRANSACTION = 1,
		BINDER_WORK_TRANSACTION_COMPLETE,
		BINDER_WORK_NODE,
		BINDER_WORK_DEAD_BINDER,
		BINDER_WORK_DEAD_BINDER_AND_CLEAR,
		BINDER_WORK_CLEAR_DEATH_NOTIFICATION,
	} type;
};

//服务实体节点
struct binder_node {
	int debug_id;
	struct binder_work work;
	union {
		struct rb_node rb_node;//红黑树组织节点
		struct hlist_node dead_node;
	};
	struct binder_proc *proc;//所属进程
	struct hlist_head refs;//该binder_node代理binder_ref的链表
	int internal_strong_refs;
	int local_weak_refs;
	int local_strong_refs;
	binder_uintptr_t ptr;//服务实体指针
	binder_uintptr_t cookie;//服务实体指针
	unsigned has_strong_ref:1;
	unsigned pending_strong_ref:1;
	unsigned has_weak_ref:1;
	unsigned pending_weak_ref:1;
	unsigned has_async_transaction:1;
	unsigned accept_fds:1;
	unsigned min_priority:8;
	struct list_head async_todo;
};

struct binder_ref_death {
	struct binder_work work;
	binder_uintptr_t cookie;
};

//服务代理节点
struct binder_ref {
	/* Lookups needed: */
	/*   node + proc => ref (transaction) */
	/*   desc + proc => ref (transaction, inc/dec ref) */
	/*   node => refs + procs (proc exit) */
	int debug_id;
	struct rb_node rb_node_desc;//refs_by_desc红黑树组织节点
	struct rb_node rb_node_node;//refs_by_node红黑树组织节点
	struct hlist_node node_entry;//binder_ref链表组织节点
	struct binder_proc *proc;//该ref所属进程
	struct binder_node *node;//代理服务的实体node
	uint32_t desc;//句柄
	int strong;
	int weak;
	struct binder_ref_death *death;
};

//通信数据缓冲区
struct binder_buffer {
	struct list_head entry; /* free and allocated entries by address */ //binder_buffer链表组织节点
	struct rb_node rb_node; /* free entry by size or allocated entry */ //binder_buffer红黑树组织节点
				/* by address */
	unsigned free:1;//是否分配标志
	unsigned allow_user_free:1;
	unsigned async_transaction:1;
	unsigned debug_id:29;

	struct binder_transaction *transaction;//所关联的binder传输事务

	struct binder_node *target_node;//该buffer数据处理服务的node
	size_t data_size;//通信数据data部分大小
	size_t offsets_size;//通信数据offsets部分大小
	uint8_t data[0];//通信数据首地址
};

enum binder_deferred_state {
	BINDER_DEFERRED_PUT_FILES    = 0x01,
	BINDER_DEFERRED_FLUSH        = 0x02,
	BINDER_DEFERRED_RELEASE      = 0x04,
};

//进程
struct binder_proc {
	struct hlist_node proc_node;//binder_procs链表组织节点
	struct rb_root threads;//该进程处理线程红黑树
	struct rb_root nodes;//该进程服务实体红黑树
	struct rb_root refs_by_desc;//该进程服务代理红黑树（通过desc查找）
	struct rb_root refs_by_node;//该进程服务代理红黑树（通过node地址查找）
	int pid;//进程pid
	struct vm_area_struct *vma;//用户空间描述结构体
	struct mm_struct *vma_vm_mm;
	struct task_struct *tsk;
	struct files_struct *files;
	struct hlist_node deferred_work_node;
	int deferred_work;
	void *buffer;//该进程映射的内核空间首地址
	ptrdiff_t user_buffer_offset;//内核空间与用户空间的偏移量

	struct list_head buffers;//binder_buffer链表
	struct rb_root free_buffers;//未分配的binder_buffer红黑树组织节点
	struct rb_root allocated_buffers;//已分配的binder_buffer红黑树组织节点
	size_t free_async_space;

	struct page **pages;//物理页结构指针数组
	size_t buffer_size;//binder映射的空间大小
	uint32_t buffer_free;
	struct list_head todo;//进程todo队列
	wait_queue_head_t wait;//进程等待队列
	struct binder_stats stats;
	struct list_head delivered_death;
	int max_threads;//最大线程数
	int requested_threads;
	int requested_threads_started;
	int ready_threads;
	long default_priority;
	struct dentry *debugfs_entry;
};

enum {
	BINDER_LOOPER_STATE_REGISTERED  = 0x01,
	BINDER_LOOPER_STATE_ENTERED     = 0x02,
	BINDER_LOOPER_STATE_EXITED      = 0x04,
	BINDER_LOOPER_STATE_INVALID     = 0x08,
	BINDER_LOOPER_STATE_WAITING     = 0x10,
	BINDER_LOOPER_STATE_NEED_RETURN = 0x20
};

//线程
struct binder_thread {
	struct binder_proc *proc;//该线程所属进程
	struct rb_node rb_node;//binder_thread红黑树组织节点
	int pid;//线程pid
	int looper;//循环标志位
	struct binder_transaction *transaction_stack;//线程的binder通信事务栈，里面放的都是需要等待回复的事务
	struct list_head todo;//线程todo队列
	uint32_t return_error; /* Write failed, return error code in read buf */
	uint32_t return_error2; /* Write failed, return error code in read */
		/* buffer. Used when sending a reply to a dead process that */
		/* we are also waiting on */
	wait_queue_head_t wait;//线程等待队列
	struct binder_stats stats;
};

//binder传输事务的数据结构
struct binder_transaction {
	int debug_id;
	struct binder_work work;//用来组织binder_transaction队列
	struct binder_thread *from;//通信数据发送方线程
	struct binder_transaction *from_parent;//binder_thread_write调用，指向该通信事务发送方线程的需等待回复的下一个事务
	struct binder_proc *to_proc;//通讯数据的接收方进程
	struct binder_thread *to_thread;//通讯数据的接收方处理线程，发送事务为null，回复事务为发送数据的线程
	struct binder_transaction *to_parent;//binder_thread_read调用，指向该通信事务接收方线程的需等待回复的下一个事务
	unsigned need_reply:1;
	/* unsigned is_dead:1; */	/* not used at the moment */

	struct binder_buffer *buffer;//通信目标进程的binder_buffer，装载通信数据
	unsigned int	code;//处理通信数据的服务指令
	unsigned int	flags;
	long	priority;
	long	saved_priority;
	kuid_t	sender_euid;
};

static void
binder_defer_work(struct binder_proc *proc, enum binder_deferred_state defer);

static int task_get_unused_fd_flags(struct binder_proc *proc, int flags)
{
	struct files_struct *files = proc->files;
	unsigned long rlim_cur;
	unsigned long irqs;

	if (files == NULL)
		return -ESRCH;

	if (!lock_task_sighand(proc->tsk, &irqs))
		return -EMFILE;

	rlim_cur = task_rlimit(proc->tsk, RLIMIT_NOFILE);
	unlock_task_sighand(proc->tsk, &irqs);

	return __alloc_fd(files, 0, rlim_cur, flags);
}

/*
 * copied from fd_install
 */
static void task_fd_install(
	struct binder_proc *proc, unsigned int fd, struct file *file)
{
	if (proc->files)
		__fd_install(proc->files, fd, file);
}

/*
 * copied from sys_close
 */
static long task_close_fd(struct binder_proc *proc, unsigned int fd)
{
	int retval;

	if (proc->files == NULL)
		return -ESRCH;

	retval = __close_fd(proc->files, fd);
	/* can't restart close syscall because file table entry was cleared */
	if (unlikely(retval == -ERESTARTSYS ||
		     retval == -ERESTARTNOINTR ||
		     retval == -ERESTARTNOHAND ||
		     retval == -ERESTART_RESTARTBLOCK))
		retval = -EINTR;

	return retval;
}

static inline void binder_lock(const char *tag)
{
	trace_binder_lock(tag);
	mutex_lock(&binder_main_lock);
	trace_binder_locked(tag);
}

static inline void binder_unlock(const char *tag)
{
	trace_binder_unlock(tag);
	mutex_unlock(&binder_main_lock);
}

static void binder_set_nice(long nice)
{
	long min_nice;

	if (can_nice(current, nice)) {
		set_user_nice(current, nice);
		return;
	}
	min_nice = rlimit_to_nice(current->signal->rlim[RLIMIT_NICE].rlim_cur);
	binder_debug(BINDER_DEBUG_PRIORITY_CAP,
		     "%d: nice value %ld not allowed use %ld instead\n",
		      current->pid, nice, min_nice);
	set_user_nice(current, min_nice);
	if (min_nice <= MAX_NICE)
		return;
	binder_user_error("%d RLIMIT_NICE not set\n", current->pid);
}

//计算binder_buffer的大小
static size_t binder_buffer_size(struct binder_proc *proc,
				 struct binder_buffer *buffer)
{
	if (list_is_last(&buffer->entry, &proc->buffers))//如果当前节点在链表末尾
		return proc->buffer + proc->buffer_size - (void *)buffer->data;//返回当前buffer->data到proc->buffer_size的空间大小
	return (size_t)list_entry(buffer->entry.next,
			  struct binder_buffer, entry) - (size_t)buffer->data;//返回当前buffer->data到下个binder_buffer首地址的空间大小
}

//插入未分配的buffer红黑树
static void binder_insert_free_buffer(struct binder_proc *proc,
				      struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->free_buffers.rb_node;//free_buffers红黑树根节点
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;//binder_buffer遍历临时变量
	size_t buffer_size;
	size_t new_buffer_size;//要插入的binder_buffer的大小

	BUG_ON(!new_buffer->free);

	new_buffer_size = binder_buffer_size(proc, new_buffer);//计算需插入的binder_buffer的大小

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: add free buffer, size %zd, at %p\n",
		      proc->pid, new_buffer_size, new_buffer);

    //红黑树遍历
	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);//根据成员rb_node拿到对应的binder_buffer
		BUG_ON(!buffer->free);

		buffer_size = binder_buffer_size(proc, buffer);//计算当前节点的binder_buffer的大小
		
		//按照buffer大小找到合适的插入位置
		if (new_buffer_size < buffer_size)
			p = &parent->rb_left;
		else
			p = &parent->rb_right;
	}
	//插入到红黑树里
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->free_buffers);
}

//插入已分配的buffer红黑树
static void binder_insert_allocated_buffer(struct binder_proc *proc,
					   struct binder_buffer *new_buffer)
{
	struct rb_node **p = &proc->allocated_buffers.rb_node;//allocated_buffers红黑树根节点
	struct rb_node *parent = NULL;
	struct binder_buffer *buffer;//binder_buffer遍历临时变量

	BUG_ON(new_buffer->free);

    //红黑树遍历，找到合适的插入位置
	while (*p) {
		parent = *p;
		buffer = rb_entry(parent, struct binder_buffer, rb_node);//根据成员rb_node拿到对应的binder_buffer
		BUG_ON(buffer->free);

		if (new_buffer < buffer)
			p = &parent->rb_left;
		else if (new_buffer > buffer)
			p = &parent->rb_right;
		else
			BUG();
	}
	//插入到红黑树里
	rb_link_node(&new_buffer->rb_node, parent, p);
	rb_insert_color(&new_buffer->rb_node, &proc->allocated_buffers);
}

//根据user_ptr（binder_buffer.data）的值找出对应的binder_buffer首地址
static struct binder_buffer *binder_buffer_lookup(struct binder_proc *proc,
						  uintptr_t user_ptr)
{
	struct rb_node *n = proc->allocated_buffers.rb_node;//已分配的binder_buffer红黑树的根节点
	struct binder_buffer *buffer;
	struct binder_buffer *kern_ptr;

    //通过user_ptr - proc->user_buffer_offset算出binder_buffer的data字段在内核的地址
    //offsetof(struct binder_buffer, data)再根据data相对于binder_buffer的偏移量算出binder_buffer的首地址
	kern_ptr = (struct binder_buffer *)(user_ptr - proc->user_buffer_offset
		- offsetof(struct binder_buffer, data));

    //直接返回kern_ptr不行吗？当然，必须要证明kern_ptr存在于已分配的binder_buffer红黑树才行
    //所以从根节点遍历红黑树
	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);
		BUG_ON(buffer->free);

		if (kern_ptr < buffer)
			n = n->rb_left;
		else if (kern_ptr > buffer)
			n = n->rb_right;
		else
			return buffer;//找到返回binder_buffer
	}
	return NULL;//没有找到返回null
}

//申请物理内存，并建立映射关系
static int binder_update_page_range(struct binder_proc *proc, int allocate,
				    void *start, void *end,
				    struct vm_area_struct *vma)
{
	void *page_addr;
	unsigned long user_page_addr;
	struct page **page;
	struct mm_struct *mm;

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: %s pages %p-%p\n", proc->pid,
		     allocate ? "allocate" : "free", start, end);

	if (end <= start)
		return 0;

	trace_binder_update_page_range(proc, allocate, start, end);

	if (vma)
		mm = NULL;
	else
		mm = get_task_mm(proc->tsk);

	if (mm) {
		down_write(&mm->mmap_sem);
		vma = proc->vma;//从proc拿到用户地址空间
		if (vma && mm != proc->vma_vm_mm) {
			pr_err("%d: vma mm and task mm mismatch\n",
				proc->pid);
			vma = NULL;
		}
	}

	if (allocate == 0)
		goto free_range;//释放物理内存

	if (vma == NULL) {
		pr_err("%d: binder_alloc_buf failed to map pages in userspace, no vma\n",
			proc->pid);
		goto err_no_vma;
	}

	//按页大小遍历
	for (page_addr = start; page_addr < end; page_addr += PAGE_SIZE) {
		int ret;

		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];//从指针数组pages取出指针赋给page

		BUG_ON(*page);
		*page = alloc_page(GFP_KERNEL | __GFP_HIGHMEM | __GFP_ZERO);//申请分配物理页，返回page结构体
		if (*page == NULL) {
			pr_err("%d: binder_alloc_buf failed for page at %p\n",
				proc->pid, page_addr);
			goto err_alloc_page_failed;
		}
		ret = map_kernel_range_noflush((unsigned long)page_addr,
					PAGE_SIZE, PAGE_KERNEL, page);//物理页与内核空间建立映射
		flush_cache_vmap((unsigned long)page_addr,
				(unsigned long)page_addr + PAGE_SIZE);
		if (ret != 1) {
			pr_err("%d: binder_alloc_buf failed to map page at %p in kernel\n",
			       proc->pid, page_addr);
			goto err_map_kernel_failed;
		}
		user_page_addr =
			(uintptr_t)page_addr + proc->user_buffer_offset;//算出用户空间的地址
		ret = vm_insert_page(vma, user_page_addr, page[0]);//物理页与用户空间建立映射
		if (ret) {
			pr_err("%d: binder_alloc_buf failed to map page at %lx in userspace\n",
			       proc->pid, user_page_addr);
			goto err_vm_insert_page_failed;
		}
		/* vm_insert_page does not seem to increment the refcount */
	}
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return 0;

free_range://释放物理内存
	for (page_addr = end - PAGE_SIZE; page_addr >= start;
	     page_addr -= PAGE_SIZE) {//按页大小遍历
		page = &proc->pages[(page_addr - proc->buffer) / PAGE_SIZE];//从页指针数组取出页结构体指针
		if (vma)
			zap_page_range(vma, (uintptr_t)page_addr +
				proc->user_buffer_offset, PAGE_SIZE, NULL);//解除用户空间与物理页的映射
err_vm_insert_page_failed:
		unmap_kernel_range((unsigned long)page_addr, PAGE_SIZE);//解除内核空间与物理页的映射
err_map_kernel_failed:
		__free_page(*page);//释放page结构体内存
		*page = NULL;//指针置空
err_alloc_page_failed:
		;
	}
err_no_vma:
	if (mm) {
		up_write(&mm->mmap_sem);
		mmput(mm);
	}
	return -ENOMEM;
}

//在proc进程申请binder buffer内存
static struct binder_buffer *binder_alloc_buf(struct binder_proc *proc,
					      size_t data_size,
					      size_t offsets_size, int is_async)
{
	struct rb_node *n = proc->free_buffers.rb_node;//free_buffers根节点
	struct binder_buffer *buffer;
	size_t buffer_size;
	struct rb_node *best_fit = NULL;
	void *has_page_addr;
	void *end_page_addr;
	size_t size;

	if (proc->vma == NULL) {
		pr_err("%d: binder_alloc_buf, no vma\n",
		       proc->pid);
		return NULL;
	}

	size = ALIGN(data_size, sizeof(void *)) +
		ALIGN(offsets_size, sizeof(void *));//字节对齐，算出当前通信数据需占用空间的大小

	if (size < data_size || size < offsets_size) {
		binder_user_error("%d: got transaction with invalid size %zd-%zd\n",
				proc->pid, data_size, offsets_size);
		return NULL;
	}

	if (is_async &&
	    proc->free_async_space < size + sizeof(struct binder_buffer)) {
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: binder_alloc_buf size %zd failed, no async space left\n",
			      proc->pid, size);
		return NULL;
	}

    //proc->free_buffers从红黑树树根开始遍历
	while (n) {
		buffer = rb_entry(n, struct binder_buffer, rb_node);//根据成员rb_node获取binder_buffer首地址
		BUG_ON(!buffer->free);
		buffer_size = binder_buffer_size(proc, buffer);//计算该binder_buffer的大小

		if (size < buffer_size) {
			best_fit = n;
			n = n->rb_left;
		} else if (size > buffer_size)
			n = n->rb_right;
		else {
			best_fit = n;//恰好相等直接退出遍历
			break;
		}
	}
	//best_fit表示从红黑树找出大小最符合（≥size且最小）的binder_buffer节点
	if (best_fit == NULL) {//空间不足
		pr_err("%d: binder_alloc_buf size %zd failed, no address space\n",
			proc->pid, size);
		return NULL;
	}
	if (n == NULL) {//遍历完毕，如果n非空证明前面遍历正好找到与通信数据刚好大小相等的binder_buffer，前面已经赋值过给buffer，所以以下代码不用执行
		buffer = rb_entry(best_fit, struct binder_buffer, rb_node);//根据成员rb_node获取相应binder_buffer
		buffer_size = binder_buffer_size(proc, buffer);//计算该binder_buffer大小
	}

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_alloc_buf size %zd got buffer %p size %zd\n",
		      proc->pid, size, buffer, buffer_size);

	has_page_addr =
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK);
	if (n == NULL) {//计算需分配物理内存的结束位置
		if (size + sizeof(struct binder_buffer) + 4 >= buffer_size)
			buffer_size = size; /* no room for other buffers 没有空间再放下一个binder_buffer的时候*/
		else
			buffer_size = size + sizeof(struct binder_buffer);//有剩余空间放下一个binder_buffer
	}
	end_page_addr =
		(void *)PAGE_ALIGN((uintptr_t)buffer->data + buffer_size);//结束位置在下一个binder_buffer->data
	if (end_page_addr > has_page_addr)
		end_page_addr = has_page_addr;
	if (binder_update_page_range(proc, 1,
	    (void *)PAGE_ALIGN((uintptr_t)buffer->data), end_page_addr, NULL))//申请物理内存，并建立映射关系
		return NULL;

	rb_erase(best_fit, &proc->free_buffers);//分配出去的binder_buffer从free_buffers红黑树删除
	buffer->free = 0;//已分配出去使用，置为0
	binder_insert_allocated_buffer(proc, buffer);//插入到已分配的红黑树
	if (buffer_size != size) {//两者不相等证明有空间分配下一个binder_buffer
		struct binder_buffer *new_buffer = (void *)buffer->data + size;//下一个binder_buffer结构体的首地址就在分配完的大小size的后面

		list_add(&new_buffer->entry, &buffer->entry);//new_buffer插入到队列
		new_buffer->free = 1;//未分配
		binder_insert_free_buffer(proc, new_buffer);//插入到未分配的红黑树
	}
	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_alloc_buf size %zd got %p\n",
		      proc->pid, size, buffer);
	//相关成员赋值
	buffer->data_size = data_size;//data部分大小
	buffer->offsets_size = offsets_size;//offsets部分大小
	buffer->async_transaction = is_async;
	if (is_async) {
		proc->free_async_space -= size + sizeof(struct binder_buffer);
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_alloc_buf size %zd async free %zd\n",
			      proc->pid, size, proc->free_async_space);
	}

	return buffer;
}

static void *buffer_start_page(struct binder_buffer *buffer)
{
	return (void *)((uintptr_t)buffer & PAGE_MASK);
}

static void *buffer_end_page(struct binder_buffer *buffer)
{
	return (void *)(((uintptr_t)(buffer + 1) - 1) & PAGE_MASK);
}

//释放未分配的binder_buffer内存 need to do
static void binder_delete_free_buffer(struct binder_proc *proc,
				      struct binder_buffer *buffer)
{
	struct binder_buffer *prev, *next = NULL;
	int free_page_end = 1;
	int free_page_start = 1;

	BUG_ON(proc->buffers.next == &buffer->entry);
	prev = list_entry(buffer->entry.prev, struct binder_buffer, entry);//拿到前一个binder_buffer
	BUG_ON(!prev->free);
	if (buffer_end_page(prev) == buffer_start_page(buffer)) {
		free_page_start = 0;
		if (buffer_end_page(prev) == buffer_end_page(buffer))
			free_page_end = 0;
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: merge free, buffer %p share page with %p\n",
			      proc->pid, buffer, prev);
	}

	if (!list_is_last(&buffer->entry, &proc->buffers)) {//当前的binder_buffer不是proc->buffers链表的最后一个
		next = list_entry(buffer->entry.next,
				  struct binder_buffer, entry);//拿到后一个binder_buffer
		if (buffer_start_page(next) == buffer_end_page(buffer)) {
			free_page_end = 0;
			if (buffer_start_page(next) ==
			    buffer_start_page(buffer))
				free_page_start = 0;
			binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
				     "%d: merge free, buffer %p share page with %p\n",
				      proc->pid, buffer, prev);
		}
	}
	list_del(&buffer->entry);//从proc->buffers链表删除当前binder_buffer
	if (free_page_start || free_page_end) {//都非0表示有物理页需要回收
		binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
			     "%d: merge free, buffer %p do not share page%s%s with %p or %p\n",
			     proc->pid, buffer, free_page_start ? "" : " end",
			     free_page_end ? "" : " start", prev, next);
		binder_update_page_range(proc, 0, free_page_start ?
			buffer_start_page(buffer) : buffer_end_page(buffer),
			(free_page_end ? buffer_end_page(buffer) :
			buffer_start_page(buffer)) + PAGE_SIZE, NULL);//释放物理页
	}
}

//释放binder_buffer
static void binder_free_buf(struct binder_proc *proc,
			    struct binder_buffer *buffer)
{
	size_t size, buffer_size;

	buffer_size = binder_buffer_size(proc, buffer);//计算buffer的大小

	size = ALIGN(buffer->data_size, sizeof(void *)) +
		ALIGN(buffer->offsets_size, sizeof(void *));

	binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
		     "%d: binder_free_buf %p size %zd buffer_size %zd\n",
		      proc->pid, buffer, size, buffer_size);

	BUG_ON(buffer->free);
	BUG_ON(size > buffer_size);
	BUG_ON(buffer->transaction != NULL);
	BUG_ON((void *)buffer < proc->buffer);
	BUG_ON((void *)buffer > proc->buffer + proc->buffer_size);

	if (buffer->async_transaction) {
		proc->free_async_space += size + sizeof(struct binder_buffer);

		binder_debug(BINDER_DEBUG_BUFFER_ALLOC_ASYNC,
			     "%d: binder_free_buf size %zd async free %zd\n",
			      proc->pid, size, proc->free_async_space);
	}

	binder_update_page_range(proc, 0,
		(void *)PAGE_ALIGN((uintptr_t)buffer->data),
		(void *)(((uintptr_t)buffer->data + buffer_size) & PAGE_MASK),
		NULL);//释放物理内存
	rb_erase(&buffer->rb_node, &proc->allocated_buffers);//从已分配的红黑树上删除该binder_buffer节点
	buffer->free = 1;//free成员置1
	if (!list_is_last(&buffer->entry, &proc->buffers)) {//如果该binder_buffer不是proc->buffers链表上的最后一个
		struct binder_buffer *next = list_entry(buffer->entry.next,
						struct binder_buffer, entry);//把链表上的下一个的binder_buffer找到

		if (next->free) {//下一个binder_buffer未分配
			rb_erase(&next->rb_node, &proc->free_buffers);//将下一个binder_buffer从未分配的红黑树上删除
			binder_delete_free_buffer(proc, next);//将下一个binder_buffer从proc->buffers链表中删除，并释放内存
		}
	}
	if (proc->buffers.next != &buffer->entry) {//如果该binder_buffer不是proc->buffers链表上的第一个
		struct binder_buffer *prev = list_entry(buffer->entry.prev,
						struct binder_buffer, entry);//把链表上的前一个的binder_buffer找到

		if (prev->free) {//前一个binder_buffer未分配
			binder_delete_free_buffer(proc, buffer);//将当前binder_buffer从proc->buffers链表中删除，并释放内存
			rb_erase(&prev->rb_node, &proc->free_buffers);//将前一个binder_buffer从未分配的红黑树上删除
			buffer = prev;//将前一个binder_buffer赋值给buffer
		}
	}
	binder_insert_free_buffer(proc, buffer);//插入到未分配的红黑树
}

//根据服务指针ptr从proc进程的nodes红黑树说去对应的binder_node
static struct binder_node *binder_get_node(struct binder_proc *proc,
					   binder_uintptr_t ptr)
{
	struct rb_node *n = proc->nodes.rb_node;//获取nodes红黑树根节点
	struct binder_node *node;

    //红黑树遍历
	while (n) {
		node = rb_entry(n, struct binder_node, rb_node);//通过成员rb_node获取binder_node的首地址

		if (ptr < node->ptr)
			n = n->rb_left;
		else if (ptr > node->ptr)
			n = n->rb_right;
		else
			return node;//找到，返回binder_node
	}
	return NULL;//没找到，返回null
}

//创建binder_node
static struct binder_node *binder_new_node(struct binder_proc *proc,
					   binder_uintptr_t ptr,
					   binder_uintptr_t cookie)
{
	struct rb_node **p = &proc->nodes.rb_node;
	struct rb_node *parent = NULL;
	struct binder_node *node;

	//遍历node红黑树，找到插入的位置
	while (*p) {
		parent = *p;
		node = rb_entry(parent, struct binder_node, rb_node);//通过成员rb_node获取binder_node的首地址

		if (ptr < node->ptr)
			p = &(*p)->rb_left;
		else if (ptr > node->ptr)
			p = &(*p)->rb_right;
		else
			return NULL;
	}

	node = kzalloc(sizeof(*node), GFP_KERNEL);//申请binder_node的内存空间
	if (node == NULL)
		return NULL;
	binder_stats_created(BINDER_STAT_NODE);
	//插入到node红黑树指定位置
	rb_link_node(&node->rb_node, parent, p);
	rb_insert_color(&node->rb_node, &proc->nodes);
	//成员赋值
	node->debug_id = ++binder_last_id;
	node->proc = proc;//所属进程
	node->ptr = ptr;//binder实体指针
	node->cookie = cookie;//binder实体指针
	node->work.type = BINDER_WORK_NODE;
	INIT_LIST_HEAD(&node->work.entry);
	INIT_LIST_HEAD(&node->async_todo);
	binder_debug(BINDER_DEBUG_INTERNAL_REFS,
		     "%d:%d node %d u%016llx c%016llx created\n",
		     proc->pid, current->pid, node->debug_id,
		     (u64)node->ptr, (u64)node->cookie);
	return node;
}

//binder_node引用计数+1
static int binder_inc_node(struct binder_node *node, int strong, int internal,
			   struct list_head *target_list)
{
	if (strong) {
		if (internal) {
			if (target_list == NULL &&
			    node->internal_strong_refs == 0 &&
			    !(node == binder_context_mgr_node &&
			    node->has_strong_ref)) {
				pr_err("invalid inc strong node for %d\n",
					node->debug_id);
				return -EINVAL;
			}
			node->internal_strong_refs++;
		} else
			node->local_strong_refs++;
		if (!node->has_strong_ref && target_list) {
			list_del_init(&node->work.entry);
			list_add_tail(&node->work.entry, target_list);
		}
	} else {
		if (!internal)
			node->local_weak_refs++;
		if (!node->has_weak_ref && list_empty(&node->work.entry)) {
			if (target_list == NULL) {
				pr_err("invalid inc weak node for %d\n",
					node->debug_id);
				return -EINVAL;
			}
			list_add_tail(&node->work.entry, target_list);
		}
	}
	return 0;
}

//binder_node引用计数-1
static int binder_dec_node(struct binder_node *node, int strong, int internal)
{
	if (strong) {
		if (internal)
			node->internal_strong_refs--;
		else
			node->local_strong_refs--;
		if (node->local_strong_refs || node->internal_strong_refs)
			return 0;
	} else {
		if (!internal)
			node->local_weak_refs--;
		if (node->local_weak_refs || !hlist_empty(&node->refs))
			return 0;
	}
	if (node->proc && (node->has_strong_ref || node->has_weak_ref)) {//引用计数不为0
		if (list_empty(&node->work.entry)) {
			list_add_tail(&node->work.entry, &node->proc->todo);
			wake_up_interruptible(&node->proc->wait);
		}
	} else {//没有引用的时候
		if (hlist_empty(&node->refs) && !node->local_strong_refs &&
		    !node->local_weak_refs) {
			list_del_init(&node->work.entry);
			if (node->proc) {//node所属进程有效
				rb_erase(&node->rb_node, &node->proc->nodes);//将node从nodes红黑树删除
				binder_debug(BINDER_DEBUG_INTERNAL_REFS,
					     "refless node %d deleted\n",
					     node->debug_id);
			} else {
				hlist_del(&node->dead_node);
				binder_debug(BINDER_DEBUG_INTERNAL_REFS,
					     "dead node %d deleted\n",
					     node->debug_id);
			}
			kfree(node);//释放binder_node
			binder_stats_deleted(BINDER_STAT_NODE);
		}
	}

	return 0;
}

//通过desc(handle)值从proc进程的refs_by_desc红黑树获取相应的binder_ref
static struct binder_ref *binder_get_ref(struct binder_proc *proc,
					 uint32_t desc)
{
	struct rb_node *n = proc->refs_by_desc.rb_node;//获取refs_by_desc红黑树的根节点
	struct binder_ref *ref;

    //红黑树遍历
	while (n) {
		ref = rb_entry(n, struct binder_ref, rb_node_desc);//通过成员rb_node_desc获取的binder_ref的首地址

		if (desc < ref->desc)
			n = n->rb_left;
		else if (desc > ref->desc)
			n = n->rb_right;
		else
			return ref;//找到返回binder_ref
	}
	return NULL;//找不到返回null
}

//根据node在进程proc的refs_by_node红黑树查找对应的binder_ref，找不到则创建一个，并插入红黑树
static struct binder_ref *binder_get_ref_for_node(struct binder_proc *proc,
						  struct binder_node *node)
{
	struct rb_node *n;
	struct rb_node **p = &proc->refs_by_node.rb_node;//获取refs_by_node红黑树根节点
	struct rb_node *parent = NULL;
	struct binder_ref *ref, *new_ref;

    //红黑树遍历
	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_node);//通过成员rb_node_node获取binder_ref的首地址

		if (node < ref->node)//binder_node体地址值比较
			p = &(*p)->rb_left;
		else if (node > ref->node)
			p = &(*p)->rb_right;
		else
			return ref;//找到，直接返回ref
	}
	new_ref = kzalloc(sizeof(*ref), GFP_KERNEL);//没找到，创建一个binder_ref
	if (new_ref == NULL)
		return NULL;
	binder_stats_created(BINDER_STAT_REF);
	new_ref->debug_id = ++binder_last_id;
	new_ref->proc = proc;//该binder_ref所属进程
	new_ref->node = node;//代理的binder_node
	//将新建的binder_ref插入到refs_by_node红黑树的合适位置（parent和p）
	rb_link_node(&new_ref->rb_node_node, parent, p);
	rb_insert_color(&new_ref->rb_node_node, &proc->refs_by_node);

	new_ref->desc = (node == binder_context_mgr_node) ? 0 : 1;//desc字段赋值，如果是service_manager,desc统一是0
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {//计算desc的最终值 need to do
		ref = rb_entry(n, struct binder_ref, rb_node_desc);
		if (ref->desc > new_ref->desc)
			break;
		new_ref->desc = ref->desc + 1;
	}

	p = &proc->refs_by_desc.rb_node;//获取refs_by_desc红黑树根节点
	//红黑树遍历
	while (*p) {
		parent = *p;
		ref = rb_entry(parent, struct binder_ref, rb_node_desc);//通过成员rb_node_desc获取binder_ref的首地址

		if (new_ref->desc < ref->desc)
			p = &(*p)->rb_left;
		else if (new_ref->desc > ref->desc)
			p = &(*p)->rb_right;
		else
			BUG();
	}
	//将新建的binder_ref插入到refs_by_desc红黑树的合适位置（parent和p）
	rb_link_node(&new_ref->rb_node_desc, parent, p);
	rb_insert_color(&new_ref->rb_node_desc, &proc->refs_by_desc);
	if (node) {
		hlist_add_head(&new_ref->node_entry, &node->refs);//将该binder_ref插入到node->refs链表

		binder_debug(BINDER_DEBUG_INTERNAL_REFS,
			     "%d new ref %d desc %d for node %d\n",
			      proc->pid, new_ref->debug_id, new_ref->desc,
			      node->debug_id);
	} else {
		binder_debug(BINDER_DEBUG_INTERNAL_REFS,
			     "%d new ref %d desc %d for dead node\n",
			      proc->pid, new_ref->debug_id, new_ref->desc);
	}
	return new_ref;//返回新建的binder_ref
}

//删除binder_ref
static void binder_delete_ref(struct binder_ref *ref)
{
	binder_debug(BINDER_DEBUG_INTERNAL_REFS,
		     "%d delete ref %d desc %d for node %d\n",
		      ref->proc->pid, ref->debug_id, ref->desc,
		      ref->node->debug_id);

	rb_erase(&ref->rb_node_desc, &ref->proc->refs_by_desc);//从refs_by_desc红黑树删除
	rb_erase(&ref->rb_node_node, &ref->proc->refs_by_node);//从refs_by_node红黑树删除
	if (ref->strong)
		binder_dec_node(ref->node, 1, 1);//binder_ref代理的binder_node引用计数-1
	hlist_del(&ref->node_entry);
	binder_dec_node(ref->node, 0, 1);//binder_ref代理的binder_node引用计数-1
	if (ref->death) {
		binder_debug(BINDER_DEBUG_DEAD_BINDER,
			     "%d delete ref %d desc %d has death notification\n",
			      ref->proc->pid, ref->debug_id, ref->desc);
		list_del(&ref->death->work.entry);
		kfree(ref->death);
		binder_stats_deleted(BINDER_STAT_DEATH);
	}
	kfree(ref);//释放binder_ref内存
	binder_stats_deleted(BINDER_STAT_REF);
}

//binder_ref引用计数+1
static int binder_inc_ref(struct binder_ref *ref, int strong,
			  struct list_head *target_list)
{
	int ret;

	if (strong) {
		if (ref->strong == 0) {
			ret = binder_inc_node(ref->node, 1, 1, target_list);
			if (ret)
				return ret;
		}
		ref->strong++;
	} else {
		if (ref->weak == 0) {
			ret = binder_inc_node(ref->node, 0, 1, target_list);
			if (ret)
				return ret;
		}
		ref->weak++;
	}
	return 0;
}

//binder_ref引用计数-1
static int binder_dec_ref(struct binder_ref *ref, int strong)
{
	if (strong) {
		if (ref->strong == 0) {
			binder_user_error("%d invalid dec strong, ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->strong--;
		if (ref->strong == 0) {
			int ret;

			ret = binder_dec_node(ref->node, strong, 1);
			if (ret)
				return ret;
		}
	} else {
		if (ref->weak == 0) {
			binder_user_error("%d invalid dec weak, ref %d desc %d s %d w %d\n",
					  ref->proc->pid, ref->debug_id,
					  ref->desc, ref->strong, ref->weak);
			return -EINVAL;
		}
		ref->weak--;
	}
	if (ref->strong == 0 && ref->weak == 0)
		binder_delete_ref(ref);
	return 0;
}

//释放binder_transaction内存
static void binder_pop_transaction(struct binder_thread *target_thread,
				   struct binder_transaction *t)
{
	if (target_thread) {
		BUG_ON(target_thread->transaction_stack != t);
		BUG_ON(target_thread->transaction_stack->from != target_thread);
		target_thread->transaction_stack =
			target_thread->transaction_stack->from_parent;//binder_transaction出栈
		t->from = NULL;//置空
	}
	t->need_reply = 0;
	if (t->buffer)
		t->buffer->transaction = NULL;//置空
	kfree(t);//释放内存
	binder_stats_deleted(BINDER_STAT_TRANSACTION);
}

static void binder_send_failed_reply(struct binder_transaction *t,
				     uint32_t error_code)
{
	struct binder_thread *target_thread;
	struct binder_transaction *next;

	BUG_ON(t->flags & TF_ONE_WAY);
	while (1) {
		target_thread = t->from;
		if (target_thread) {
			if (target_thread->return_error != BR_OK &&
			   target_thread->return_error2 == BR_OK) {
				target_thread->return_error2 =
					target_thread->return_error;
				target_thread->return_error = BR_OK;
			}
			if (target_thread->return_error == BR_OK) {
				binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
					     "send failed reply for transaction %d to %d:%d\n",
					      t->debug_id,
					      target_thread->proc->pid,
					      target_thread->pid);

				binder_pop_transaction(target_thread, t);
				target_thread->return_error = error_code;
				wake_up_interruptible(&target_thread->wait);
			} else {
				pr_err("reply failed, target thread, %d:%d, has error code %d already\n",
					target_thread->proc->pid,
					target_thread->pid,
					target_thread->return_error);
			}
			return;
		}
		next = t->from_parent;

		binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
			     "send failed reply for transaction %d, target dead\n",
			     t->debug_id);

		binder_pop_transaction(target_thread, t);
		if (next == NULL) {
			binder_debug(BINDER_DEBUG_DEAD_BINDER,
				     "reply failed, no target thread at root\n");
			return;
		}
		t = next;
		binder_debug(BINDER_DEBUG_DEAD_BINDER,
			     "reply failed, no target thread -- retry %d\n",
			      t->debug_id);
	}
}

//处理binder_node和binder_ref的引用计数，没有引用则释放内存
static void binder_transaction_buffer_release(struct binder_proc *proc,
					      struct binder_buffer *buffer,
					      binder_size_t *failed_at)
{
	binder_size_t *offp, *off_end;
	int debug_id = buffer->debug_id;

	binder_debug(BINDER_DEBUG_TRANSACTION,
		     "%d buffer release %d, size %zd-%zd, failed at %p\n",
		     proc->pid, buffer->debug_id,
		     buffer->data_size, buffer->offsets_size, failed_at);

	if (buffer->target_node)
		binder_dec_node(buffer->target_node, 1, 0);//buffer->target_node引用计数-1

	offp = (binder_size_t *)(buffer->data +
				 ALIGN(buffer->data_size, sizeof(void *)));//offset指针数组的开始地址
	if (failed_at)
		off_end = failed_at;
	else
		off_end = (void *)offp + buffer->offsets_size;//offset指针数组的结束地址
	for (; offp < off_end; offp++) {//通过offset指针数组遍历flat_binder_object
		struct flat_binder_object *fp;

		if (*offp > buffer->data_size - sizeof(*fp) ||
		    buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(u32))) {
			pr_err("transaction release %d bad offset %lld, size %zd\n",
			       debug_id, (u64)*offp, buffer->data_size);
			continue;
		}
		fp = (struct flat_binder_object *)(buffer->data + *offp);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {
			struct binder_node *node = binder_get_node(proc, fp->binder);//根据binder找到binder_node

			if (node == NULL) {
				pr_err("transaction release %d bad node %016llx\n",
				       debug_id, (u64)fp->binder);
				break;
			}
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        node %d u%016llx\n",
				     node->debug_id, (u64)node->ptr);
			binder_dec_node(node, fp->type == BINDER_TYPE_BINDER, 0);//binder_node的引用计数-1
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {
                struct binder_ref *ref = binder_get_ref(proc, fp->handle);//根据handle找到binder_ref

			if (ref == NULL) {
				pr_err("transaction release %d bad handle %d\n",
				 debug_id, fp->handle);
				break;
			}
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        ref %d desc %d (node %d)\n",
				     ref->debug_id, ref->desc, ref->node->debug_id);
			binder_dec_ref(ref, fp->type == BINDER_TYPE_HANDLE);//binder_ref的引用计数-1
		} break;

		case BINDER_TYPE_FD:
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        fd %d\n", fp->handle);
			if (failed_at)
				task_close_fd(proc, fp->handle);
			break;

		default:
			pr_err("transaction release %d bad object type %x\n",
				debug_id, fp->type);
			break;
		}
	}
}

//binder数据传输
static void binder_transaction(struct binder_proc *proc,
			       struct binder_thread *thread,
			       struct binder_transaction_data *tr, int reply)
{
	struct binder_transaction *t;
	struct binder_work *tcomplete;
	binder_size_t *offp, *off_end;
	binder_size_t off_min;
	struct binder_proc *target_proc;
	struct binder_thread *target_thread = NULL;
	struct binder_node *target_node = NULL;
	struct list_head *target_list;
	wait_queue_head_t *target_wait;
	struct binder_transaction *in_reply_to = NULL;
	struct binder_transaction_log_entry *e;
	uint32_t return_error;

	e = binder_transaction_log_add(&binder_transaction_log);
	e->call_type = reply ? 2 : !!(tr->flags & TF_ONE_WAY);
	e->from_proc = proc->pid;
	e->from_thread = thread->pid;
	e->target_handle = tr->target.handle;
	e->data_size = tr->data_size;
	e->offsets_size = tr->offsets_size;

	if (reply) {//当前处于回复事务
		in_reply_to = thread->transaction_stack;//回复：从当前线程的transaction栈中取出数据发送方的binder_transaction(在binder_thread_read方法中赋的值)
		if (in_reply_to == NULL) {
			binder_user_error("%d:%d got reply transaction with no transaction stack\n",
					  proc->pid, thread->pid);
			return_error = BR_FAILED_REPLY;
			goto err_empty_call_stack;
		}
		binder_set_nice(in_reply_to->saved_priority);
		if (in_reply_to->to_thread != thread) {
			binder_user_error("%d:%d got reply transaction with bad transaction stack, transaction %d has target %d:%d\n",
				proc->pid, thread->pid, in_reply_to->debug_id,
				in_reply_to->to_proc ?
				in_reply_to->to_proc->pid : 0,
				in_reply_to->to_thread ?
				in_reply_to->to_thread->pid : 0);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			goto err_bad_call_stack;
		}
		thread->transaction_stack = in_reply_to->to_parent;//in_reply_to出栈
		target_thread = in_reply_to->from;//回复：拿到数据的发送方线程
		if (target_thread == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (target_thread->transaction_stack != in_reply_to) {
			binder_user_error("%d:%d got reply transaction with bad target transaction stack %d, expected %d\n",
				proc->pid, thread->pid,
				target_thread->transaction_stack ?
				target_thread->transaction_stack->debug_id : 0,
				in_reply_to->debug_id);
			return_error = BR_FAILED_REPLY;
			in_reply_to = NULL;
			target_thread = NULL;
			goto err_dead_binder;
		}
		target_proc = target_thread->proc;//回复：拿到数据发送方进程
	} else {//发送事务
		if (tr->target.handle) {//发送：handle非0
			struct binder_ref *ref;

			ref = binder_get_ref(proc, tr->target.handle);//发送：根据handle从当前进程的binder_ref红黑树获取对应的binder_ref
			if (ref == NULL) {
				binder_user_error("%d:%d got transaction to invalid handle\n",
					proc->pid, thread->pid);
				return_error = BR_FAILED_REPLY;
				goto err_invalid_target_handle;
			}
			target_node = ref->node;//发送：获取目标进程处理通信数据的node
		} else {//发送：handle为0表示要通讯的进程是service_manager
			target_node = binder_context_mgr_node;//发送：binder_context_mgr_node是service_manager的node，在service_manager启动并成为守护进程的时候创建赋值
			if (target_node == NULL) {
				return_error = BR_DEAD_REPLY;
				goto err_no_context_mgr_node;
			}
		}
		e->to_node = target_node->debug_id;
		target_proc = target_node->proc;//发送：获取目标进程
		if (target_proc == NULL) {
			return_error = BR_DEAD_REPLY;
			goto err_dead_binder;
		}
		if (security_binder_transaction(proc->tsk,
						target_proc->tsk) < 0) {
			return_error = BR_FAILED_REPLY;
			goto err_invalid_target_handle;
		}
		if (!(tr->flags & TF_ONE_WAY) && thread->transaction_stack) {
			struct binder_transaction *tmp;

			tmp = thread->transaction_stack;
			if (tmp->to_thread != thread) {
				binder_user_error("%d:%d got new transaction with bad transaction stack, transaction %d has target %d:%d\n",
					proc->pid, thread->pid, tmp->debug_id,
					tmp->to_proc ? tmp->to_proc->pid : 0,
					tmp->to_thread ?
					tmp->to_thread->pid : 0);
				return_error = BR_FAILED_REPLY;
				goto err_bad_call_stack;
			}
			while (tmp) {
				if (tmp->from && tmp->from->proc == target_proc)
					target_thread = tmp->from;
				tmp = tmp->from_parent;
			}
		}
	}
	if (target_thread) {
		e->to_thread = target_thread->pid;
		target_list = &target_thread->todo;//回复：数据发送方todo队列
		target_wait = &target_thread->wait;//回复：数据发送方等待队列
	} else {
		target_list = &target_proc->todo;//发送：目标进程的todo队列
		target_wait = &target_proc->wait;//发送：目标进程的等待队列
	}
	e->to_proc = target_proc->pid;

	/* TODO: reuse incoming transaction for reply */
	t = kzalloc(sizeof(*t), GFP_KERNEL);//申请binder_transaction内存
	if (t == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_t_failed;
	}
	binder_stats_created(BINDER_STAT_TRANSACTION);

	tcomplete = kzalloc(sizeof(*tcomplete), GFP_KERNEL);//申请通信完成binder_work内存
	if (tcomplete == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_alloc_tcomplete_failed;
	}
	binder_stats_created(BINDER_STAT_TRANSACTION_COMPLETE);

	t->debug_id = ++binder_last_id;
	e->debug_id = t->debug_id;

	if (reply)
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d BC_REPLY %d -> %d:%d, data %016llx-%016llx size %lld-%lld\n",
			     proc->pid, thread->pid, t->debug_id,
			     target_proc->pid, target_thread->pid,
			     (u64)tr->data.ptr.buffer,
			     (u64)tr->data.ptr.offsets,
			     (u64)tr->data_size, (u64)tr->offsets_size);
	else
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d BC_TRANSACTION %d -> %d - node %d, data %016llx-%016llx size %lld-%lld\n",
			     proc->pid, thread->pid, t->debug_id,
			     target_proc->pid, target_node->debug_id,
			     (u64)tr->data.ptr.buffer,
			     (u64)tr->data.ptr.offsets,
			     (u64)tr->data_size, (u64)tr->offsets_size);

	if (!reply && !(tr->flags & TF_ONE_WAY))//当前处于发送事务
		t->from = thread;//发送：标记当前的transaction来自哪个发送方线程，后面回复用到
	else
		t->from = NULL;//回复：不需要from，设置为null
	t->sender_euid = task_euid(proc->tsk);
	t->to_proc = target_proc;//目标进程
	t->to_thread = target_thread;//目标线程
	t->code = tr->code;
	t->flags = tr->flags;
	t->priority = task_nice(current);

	trace_binder_transaction(reply, t, target_node);

	t->buffer = binder_alloc_buf(target_proc, tr->data_size,
		tr->offsets_size, !reply && (t->flags & TF_ONE_WAY));//在目标进程中申请binder buffer空间
	if (t->buffer == NULL) {
		return_error = BR_FAILED_REPLY;
		goto err_binder_alloc_buf_failed;
	}
	t->buffer->allow_user_free = 0;
	t->buffer->debug_id = t->debug_id;
	t->buffer->transaction = t;
	t->buffer->target_node = target_node;//目标node
	trace_binder_transaction_alloc_buf(t->buffer);
	if (target_node)
		binder_inc_node(target_node, 1, 0, NULL);//目标node引用计数+1

	offp = (binder_size_t *)(t->buffer->data +
				 ALIGN(tr->data_size, sizeof(void *)));//计算offsets的首地址

	if (copy_from_user(t->buffer->data, (const void __user *)(uintptr_t)
			   tr->data.ptr.buffer, tr->data_size)) {//从用户空间拷贝data部分到内核空间，起始地址binder_buffer.data
		binder_user_error("%d:%d got transaction with invalid data ptr\n",
				proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	if (copy_from_user(offp, (const void __user *)(uintptr_t)
			   tr->data.ptr.offsets, tr->offsets_size)) {//从用户空间拷贝offsets部分到内核空间
		binder_user_error("%d:%d got transaction with invalid offsets ptr\n",
				proc->pid, thread->pid);
		return_error = BR_FAILED_REPLY;
		goto err_copy_data_failed;
	}
	if (!IS_ALIGNED(tr->offsets_size, sizeof(binder_size_t))) {
		binder_user_error("%d:%d got transaction with invalid offsets size, %lld\n",
				proc->pid, thread->pid, (u64)tr->offsets_size);
		return_error = BR_FAILED_REPLY;
		goto err_bad_offset;
	}
	off_end = (void *)offp + tr->offsets_size;//offsets部分的结束位置
	off_min = 0;
	for (; offp < off_end; offp++) {//遍历传输数据中的flat_binder_object
		struct flat_binder_object *fp;

		if (*offp > t->buffer->data_size - sizeof(*fp) ||
		    *offp < off_min ||
		    t->buffer->data_size < sizeof(*fp) ||
		    !IS_ALIGNED(*offp, sizeof(u32))) {
			binder_user_error("%d:%d got transaction with invalid offset, %lld (min %lld, max %lld)\n",
					  proc->pid, thread->pid, (u64)*offp,
					  (u64)off_min,
					  (u64)(t->buffer->data_size -
					  sizeof(*fp)));
			return_error = BR_FAILED_REPLY;
			goto err_bad_offset;
		}
		fp = (struct flat_binder_object *)(t->buffer->data + *offp);//获取flat_binder_object
		off_min = *offp + sizeof(struct flat_binder_object);
		switch (fp->type) {
		case BINDER_TYPE_BINDER:
		case BINDER_TYPE_WEAK_BINDER: {//binder实体类型
			struct binder_ref *ref;
			struct binder_node *node = binder_get_node(proc, fp->binder);//从binder_node红黑树获取该fp->binder对应的binder_node

			if (node == NULL) {//首次调用，node为空
				node = binder_new_node(proc, fp->binder, fp->cookie);//创建一个binder_node，并插入到nodes红黑树的合适位置
				if (node == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_new_node_failed;
				}
				node->min_priority = fp->flags & FLAT_BINDER_FLAG_PRIORITY_MASK;
				node->accept_fds = !!(fp->flags & FLAT_BINDER_FLAG_ACCEPTS_FDS);
			}
			if (fp->cookie != node->cookie) {
				binder_user_error("%d:%d sending u%016llx node %d, cookie mismatch %016llx != %016llx\n",
					proc->pid, thread->pid,
					(u64)fp->binder, node->debug_id,
					(u64)fp->cookie, (u64)node->cookie);
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_for_node_failed;
			}
			if (security_binder_transfer_binder(proc->tsk,
							    target_proc->tsk)) {
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_for_node_failed;
			}
			ref = binder_get_ref_for_node(target_proc, node);//根据node在目标进程的binder_ref红黑树查找对应的binder_ref，找不到则创建一个，并插入红黑树
			if (ref == NULL) {
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_for_node_failed;
			}
			//将通讯数据的binder对象的binder类型从binder实体改成binder代理，所以非binder所属进程持有的binder引用都是binder代理
			if (fp->type == BINDER_TYPE_BINDER)
				fp->type = BINDER_TYPE_HANDLE;
			else
				fp->type = BINDER_TYPE_WEAK_HANDLE;
			fp->handle = ref->desc;//binder代理句柄赋值
			binder_inc_ref(ref, fp->type == BINDER_TYPE_HANDLE,
				       &thread->todo);//binder_ref引用计数+1

			trace_binder_transaction_node_to_ref(t, node, ref);
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        node %d u%016llx -> ref %d desc %d\n",
				     node->debug_id, (u64)node->ptr,
				     ref->debug_id, ref->desc);
		} break;
		case BINDER_TYPE_HANDLE:
		case BINDER_TYPE_WEAK_HANDLE: {//binder代理类型
			struct binder_ref *ref = binder_get_ref(proc, fp->handle);//根据句柄从当前进程获取对应的binder_ref

			if (ref == NULL) {
				binder_user_error("%d:%d got transaction with invalid handle, %d\n",
						proc->pid,
						thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_failed;
			}
			if (security_binder_transfer_binder(proc->tsk,
							    target_proc->tsk)) {
				return_error = BR_FAILED_REPLY;
				goto err_binder_get_ref_failed;
			}
			if (ref->node->proc == target_proc) {//如果binder_ref代理的binder_node的所属进程与目标进程是同一进程，通信的binder对象的类型从binder代理改成binder实体
				if (fp->type == BINDER_TYPE_HANDLE)
					fp->type = BINDER_TYPE_BINDER;
				else
					fp->type = BINDER_TYPE_WEAK_BINDER;
				//服务指针赋值
				fp->binder = ref->node->ptr;
				fp->cookie = ref->node->cookie;
				binder_inc_node(ref->node, fp->type == BINDER_TYPE_BINDER, 0, NULL);//binder_node引用计数+1
				trace_binder_transaction_ref_to_node(t, ref);
				binder_debug(BINDER_DEBUG_TRANSACTION,
					     "        ref %d desc %d -> node %d u%016llx\n",
					     ref->debug_id, ref->desc, ref->node->debug_id,
					     (u64)ref->node->ptr);
			} else {//binder_ref代理的binder_node的所属进程与目标进程不；是同一进程
				struct binder_ref *new_ref;

				new_ref = binder_get_ref_for_node(target_proc, ref->node);//根据node在目标进程的binder_ref红黑树查找对应的binder_ref，找不到则创建一个，并插入红黑树
				if (new_ref == NULL) {
					return_error = BR_FAILED_REPLY;
					goto err_binder_get_ref_for_node_failed;
				}
				fp->handle = new_ref->desc;//句柄赋值
				binder_inc_ref(new_ref, fp->type == BINDER_TYPE_HANDLE, NULL);//binder_ref引用计数+1
				trace_binder_transaction_ref_to_ref(t, ref,
								    new_ref);
				binder_debug(BINDER_DEBUG_TRANSACTION,
					     "        ref %d desc %d -> ref %d desc %d (node %d)\n",
					     ref->debug_id, ref->desc, new_ref->debug_id,
					     new_ref->desc, ref->node->debug_id);
			}
		} break;

		case BINDER_TYPE_FD: {//文件描述符fd类型
			int target_fd;
			struct file *file;

			if (reply) {
				if (!(in_reply_to->flags & TF_ACCEPT_FDS)) {
					binder_user_error("%d:%d got reply with fd, %d, but target does not allow fds\n",
						proc->pid, thread->pid, fp->handle);
					return_error = BR_FAILED_REPLY;
					goto err_fd_not_allowed;
				}
			} else if (!target_node->accept_fds) {
				binder_user_error("%d:%d got transaction with fd, %d, but target does not allow fds\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fd_not_allowed;
			}

			file = fget(fp->handle);//根据fd获取file结构体
			if (file == NULL) {
				binder_user_error("%d:%d got transaction with invalid fd, %d\n",
					proc->pid, thread->pid, fp->handle);
				return_error = BR_FAILED_REPLY;
				goto err_fget_failed;
			}
			if (security_binder_transfer_file(proc->tsk,
							  target_proc->tsk,
							  file) < 0) {
				fput(file);
				return_error = BR_FAILED_REPLY;
				goto err_get_unused_fd_failed;
			}
			target_fd = task_get_unused_fd_flags(target_proc, O_CLOEXEC);//为目标进程分配fd
			if (target_fd < 0) {
				fput(file);
				return_error = BR_FAILED_REPLY;
				goto err_get_unused_fd_failed;
			}
			task_fd_install(target_proc, target_fd, file);//目标fd与file绑定
			trace_binder_transaction_fd(t, fp->handle, target_fd);
			binder_debug(BINDER_DEBUG_TRANSACTION,
				     "        fd %d -> %d\n", fp->handle, target_fd);
			/* TODO: fput? */
			fp->handle = target_fd;
		} break;

		default:
			binder_user_error("%d:%d got transaction with invalid object type, %x\n",
				proc->pid, thread->pid, fp->type);
			return_error = BR_FAILED_REPLY;
			goto err_bad_object_type;
		}
	}
	if (reply) {//回复事务
		BUG_ON(t->buffer->async_transaction != 0);
		binder_pop_transaction(target_thread, in_reply_to);//回复：完成，释放发送方的binder_transaction内存
	} else if (!(t->flags & TF_ONE_WAY)) {//需要接收方回复的发送事务
		BUG_ON(t->buffer->async_transaction != 0);
		t->need_reply = 1;//该transaction需要回复
		//压栈，通过from_parent成员把binder_transaction串成一个栈结构
		t->from_parent = thread->transaction_stack;
		thread->transaction_stack = t;
	} else {
		BUG_ON(target_node == NULL);
		BUG_ON(t->buffer->async_transaction != 1);
		if (target_node->has_async_transaction) {
			target_list = &target_node->async_todo;
			target_wait = NULL;
		} else
			target_node->has_async_transaction = 1;
	}
	t->work.type = BINDER_WORK_TRANSACTION;//事务的成员binder_work的类型设置
	list_add_tail(&t->work.entry, target_list);//将当前transaction添加到目标进程的todo队列（以binder_work.entry组织）
	tcomplete->type = BINDER_WORK_TRANSACTION_COMPLETE;//类型：通信完成
	list_add_tail(&tcomplete->entry, &thread->todo);//把通信完成binder_work插入到当前发送线程的todo队列
	if (target_wait)//目标进（线）程等待队列不为空
	    //唤醒等待队列。发送事务的时候，不能确定接收方哪个线程处理，所以唤醒目标进程的等待队列；
	    //回复事务的时候，能从发送事务确定发送方的发送线程，所以唤醒目标线程的等待队列。
		wake_up_interruptible(target_wait);
	return;

err_get_unused_fd_failed:
err_fget_failed:
err_fd_not_allowed:
err_binder_get_ref_for_node_failed:
err_binder_get_ref_failed:
err_binder_new_node_failed:
err_bad_object_type:
err_bad_offset:
err_copy_data_failed:
	trace_binder_transaction_failed_buffer_release(t->buffer);
	binder_transaction_buffer_release(target_proc, t->buffer, offp);
	t->buffer->transaction = NULL;
	binder_free_buf(target_proc, t->buffer);
err_binder_alloc_buf_failed:
	kfree(tcomplete);
	binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
err_alloc_tcomplete_failed:
	kfree(t);
	binder_stats_deleted(BINDER_STAT_TRANSACTION);
err_alloc_t_failed:
err_bad_call_stack:
err_empty_call_stack:
err_dead_binder:
err_invalid_target_handle:
err_no_context_mgr_node:
	binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
		     "%d:%d transaction failed %d, size %lld-%lld\n",
		     proc->pid, thread->pid, return_error,
		     (u64)tr->data_size, (u64)tr->offsets_size);

	{
		struct binder_transaction_log_entry *fe;

		fe = binder_transaction_log_add(&binder_transaction_log_failed);
		*fe = *e;
	}

	BUG_ON(thread->return_error != BR_OK);
	if (in_reply_to) {
		thread->return_error = BR_TRANSACTION_COMPLETE;
		binder_send_failed_reply(in_reply_to, return_error);
	} else
		thread->return_error = return_error;
}

//binder写
static int binder_thread_write(struct binder_proc *proc,
			struct binder_thread *thread,
			binder_uintptr_t binder_buffer, size_t size,
			binder_size_t *consumed)
{
	uint32_t cmd;
	void __user *buffer = (void __user *)(uintptr_t)binder_buffer;//buffer首地址
	void __user *ptr = buffer + *consumed;//开始位置
	void __user *end = buffer + size;//结束位置

	while (ptr < end && thread->return_error == BR_OK) {
		if (get_user(cmd, (uint32_t __user *)ptr))//从用户空间读出cmd的值
			return -EFAULT;
		ptr += sizeof(uint32_t);//指针后移
		trace_binder_command(cmd);
		if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.bc)) {
			binder_stats.bc[_IOC_NR(cmd)]++;
			proc->stats.bc[_IOC_NR(cmd)]++;
			thread->stats.bc[_IOC_NR(cmd)]++;
		}
		switch (cmd) {
		case BC_INCREFS:
		case BC_ACQUIRE:
		case BC_RELEASE:
		case BC_DECREFS: {
			uint32_t target;
			struct binder_ref *ref;
			const char *debug_string;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (target == 0 && binder_context_mgr_node &&
			    (cmd == BC_INCREFS || cmd == BC_ACQUIRE)) {
				ref = binder_get_ref_for_node(proc,
					       binder_context_mgr_node);
				if (ref->desc != target) {
					binder_user_error("%d:%d tried to acquire reference to desc 0, got %d instead\n",
						proc->pid, thread->pid,
						ref->desc);
				}
			} else
				ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("%d:%d refcount change on invalid ref %d\n",
					proc->pid, thread->pid, target);
				break;
			}
			switch (cmd) {
			case BC_INCREFS:
				debug_string = "IncRefs";
				binder_inc_ref(ref, 0, NULL);
				break;
			case BC_ACQUIRE:
				debug_string = "Acquire";
				binder_inc_ref(ref, 1, NULL);
				break;
			case BC_RELEASE:
				debug_string = "Release";
				binder_dec_ref(ref, 1);
				break;
			case BC_DECREFS:
			default:
				debug_string = "DecRefs";
				binder_dec_ref(ref, 0);
				break;
			}
			binder_debug(BINDER_DEBUG_USER_REFS,
				     "%d:%d %s ref %d desc %d s %d w %d for node %d\n",
				     proc->pid, thread->pid, debug_string, ref->debug_id,
				     ref->desc, ref->strong, ref->weak, ref->node->debug_id);
			break;
		}
		case BC_INCREFS_DONE:
		case BC_ACQUIRE_DONE: {
			binder_uintptr_t node_ptr;
			binder_uintptr_t cookie;
			struct binder_node *node;

			if (get_user(node_ptr, (binder_uintptr_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(binder_uintptr_t);
			if (get_user(cookie, (binder_uintptr_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(binder_uintptr_t);
			node = binder_get_node(proc, node_ptr);
			if (node == NULL) {
				binder_user_error("%d:%d %s u%016llx no match\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" :
					"BC_ACQUIRE_DONE",
					(u64)node_ptr);
				break;
			}
			if (cookie != node->cookie) {
				binder_user_error("%d:%d %s u%016llx node %d cookie mismatch %016llx != %016llx\n",
					proc->pid, thread->pid,
					cmd == BC_INCREFS_DONE ?
					"BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
					(u64)node_ptr, node->debug_id,
					(u64)cookie, (u64)node->cookie);
				break;
			}
			if (cmd == BC_ACQUIRE_DONE) {
				if (node->pending_strong_ref == 0) {
					binder_user_error("%d:%d BC_ACQUIRE_DONE node %d has no pending acquire request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_strong_ref = 0;
			} else {
				if (node->pending_weak_ref == 0) {
					binder_user_error("%d:%d BC_INCREFS_DONE node %d has no pending increfs request\n",
						proc->pid, thread->pid,
						node->debug_id);
					break;
				}
				node->pending_weak_ref = 0;
			}
			binder_dec_node(node, cmd == BC_ACQUIRE_DONE, 0);
			binder_debug(BINDER_DEBUG_USER_REFS,
				     "%d:%d %s node %d ls %d lw %d\n",
				     proc->pid, thread->pid,
				     cmd == BC_INCREFS_DONE ? "BC_INCREFS_DONE" : "BC_ACQUIRE_DONE",
				     node->debug_id, node->local_strong_refs, node->local_weak_refs);
			break;
		}
		case BC_ATTEMPT_ACQUIRE:
			pr_err("BC_ATTEMPT_ACQUIRE not supported\n");
			return -EINVAL;
		case BC_ACQUIRE_RESULT:
			pr_err("BC_ACQUIRE_RESULT not supported\n");
			return -EINVAL;

		case BC_FREE_BUFFER: {//释放binder_buffer
			binder_uintptr_t data_ptr;
			struct binder_buffer *buffer;

			if (get_user(data_ptr, (binder_uintptr_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(binder_uintptr_t);

			buffer = binder_buffer_lookup(proc, data_ptr);//找到要释放的buffer，传入的是binder_buffer.data的地址
			if (buffer == NULL) {
				binder_user_error("%d:%d BC_FREE_BUFFER u%016llx no match\n",
					proc->pid, thread->pid, (u64)data_ptr);
				break;
			}
			if (!buffer->allow_user_free) {
				binder_user_error("%d:%d BC_FREE_BUFFER u%016llx matched unreturned buffer\n",
					proc->pid, thread->pid, (u64)data_ptr);
				break;
			}
			binder_debug(BINDER_DEBUG_FREE_BUFFER,
				     "%d:%d BC_FREE_BUFFER u%016llx found buffer %d for %s transaction\n",
				     proc->pid, thread->pid, (u64)data_ptr,
				     buffer->debug_id,
				     buffer->transaction ? "active" : "finished");

			if (buffer->transaction) {//置空
				buffer->transaction->buffer = NULL;
				buffer->transaction = NULL;
			}
			if (buffer->async_transaction && buffer->target_node) {
				BUG_ON(!buffer->target_node->has_async_transaction);
				if (list_empty(&buffer->target_node->async_todo))
					buffer->target_node->has_async_transaction = 0;
				else
					list_move_tail(buffer->target_node->async_todo.next, &thread->todo);
			}
			trace_binder_transaction_buffer_release(buffer);
			binder_transaction_buffer_release(proc, buffer, NULL);//处理binder_buffer相关的binder_node和binder_ref的释放
			binder_free_buf(proc, buffer);//处理binder_buffer的释放
			break;
		}

		case BC_TRANSACTION:
		case BC_REPLY: {
			struct binder_transaction_data tr;

			if (copy_from_user(&tr, ptr, sizeof(tr)))//将传输的数据binder_transaction_data从用户空间地址ptr拷贝到内核地址&tr
				return -EFAULT;
			ptr += sizeof(tr);//指针后移
			binder_transaction(proc, thread, &tr, cmd == BC_REPLY);//调用binder_transaction处理数据
			break;
		}

		case BC_REGISTER_LOOPER://注册循环？
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_REGISTER_LOOPER\n",
				     proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_ENTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_REGISTER_LOOPER called after BC_ENTER_LOOPER\n",
					proc->pid, thread->pid);
			} else if (proc->requested_threads == 0) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_REGISTER_LOOPER called without request\n",
					proc->pid, thread->pid);
			} else {
				proc->requested_threads--;
				proc->requested_threads_started++;
			}
			thread->looper |= BINDER_LOOPER_STATE_REGISTERED;//设置thread->looper状态位
			break;
		case BC_ENTER_LOOPER://线程进入循环
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_ENTER_LOOPER\n",
				     proc->pid, thread->pid);
			if (thread->looper & BINDER_LOOPER_STATE_REGISTERED) {
				thread->looper |= BINDER_LOOPER_STATE_INVALID;
				binder_user_error("%d:%d ERROR: BC_ENTER_LOOPER called after BC_REGISTER_LOOPER\n",
					proc->pid, thread->pid);
			}
			thread->looper |= BINDER_LOOPER_STATE_ENTERED;//将thread->looper 状态位设置成BINDER_LOOPER_STATE_ENTERED
			break;
		case BC_EXIT_LOOPER://线程退出循环
			binder_debug(BINDER_DEBUG_THREADS,
				     "%d:%d BC_EXIT_LOOPER\n",
				     proc->pid, thread->pid);
			thread->looper |= BINDER_LOOPER_STATE_EXITED;//将thread->looper 状态位设置成BINDER_LOOPER_STATE_EXITED
			break;

		case BC_REQUEST_DEATH_NOTIFICATION:
		case BC_CLEAR_DEATH_NOTIFICATION: {
			uint32_t target;
			binder_uintptr_t cookie;
			struct binder_ref *ref;
			struct binder_ref_death *death;

			if (get_user(target, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (get_user(cookie, (binder_uintptr_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(binder_uintptr_t);
			ref = binder_get_ref(proc, target);
			if (ref == NULL) {
				binder_user_error("%d:%d %s invalid ref %d\n",
					proc->pid, thread->pid,
					cmd == BC_REQUEST_DEATH_NOTIFICATION ?
					"BC_REQUEST_DEATH_NOTIFICATION" :
					"BC_CLEAR_DEATH_NOTIFICATION",
					target);
				break;
			}

			binder_debug(BINDER_DEBUG_DEATH_NOTIFICATION,
				     "%d:%d %s %016llx ref %d desc %d s %d w %d for node %d\n",
				     proc->pid, thread->pid,
				     cmd == BC_REQUEST_DEATH_NOTIFICATION ?
				     "BC_REQUEST_DEATH_NOTIFICATION" :
				     "BC_CLEAR_DEATH_NOTIFICATION",
				     (u64)cookie, ref->debug_id, ref->desc,
				     ref->strong, ref->weak, ref->node->debug_id);

			if (cmd == BC_REQUEST_DEATH_NOTIFICATION) {
				if (ref->death) {
					binder_user_error("%d:%d BC_REQUEST_DEATH_NOTIFICATION death notification already set\n",
						proc->pid, thread->pid);
					break;
				}
				death = kzalloc(sizeof(*death), GFP_KERNEL);
				if (death == NULL) {
					thread->return_error = BR_ERROR;
					binder_debug(BINDER_DEBUG_FAILED_TRANSACTION,
						     "%d:%d BC_REQUEST_DEATH_NOTIFICATION failed\n",
						     proc->pid, thread->pid);
					break;
				}
				binder_stats_created(BINDER_STAT_DEATH);
				INIT_LIST_HEAD(&death->work.entry);
				death->cookie = cookie;
				ref->death = death;
				if (ref->node->proc == NULL) {
					ref->death->work.type = BINDER_WORK_DEAD_BINDER;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&ref->death->work.entry, &thread->todo);
					} else {
						list_add_tail(&ref->death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				}
			} else {
				if (ref->death == NULL) {
					binder_user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification not active\n",
						proc->pid, thread->pid);
					break;
				}
				death = ref->death;
				if (death->cookie != cookie) {
					binder_user_error("%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification cookie mismatch %016llx != %016llx\n",
						proc->pid, thread->pid,
						(u64)death->cookie,
						(u64)cookie);
					break;
				}
				ref->death = NULL;
				if (list_empty(&death->work.entry)) {
					death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
					if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
						list_add_tail(&death->work.entry, &thread->todo);
					} else {
						list_add_tail(&death->work.entry, &proc->todo);
						wake_up_interruptible(&proc->wait);
					}
				} else {
					BUG_ON(death->work.type != BINDER_WORK_DEAD_BINDER);
					death->work.type = BINDER_WORK_DEAD_BINDER_AND_CLEAR;
				}
			}
		} break;
		case BC_DEAD_BINDER_DONE: {
			struct binder_work *w;
			binder_uintptr_t cookie;
			struct binder_ref_death *death = NULL;

			if (get_user(cookie, (binder_uintptr_t __user *)ptr))
				return -EFAULT;

			ptr += sizeof(void *);
			list_for_each_entry(w, &proc->delivered_death, entry) {
				struct binder_ref_death *tmp_death = container_of(w, struct binder_ref_death, work);

				if (tmp_death->cookie == cookie) {
					death = tmp_death;
					break;
				}
			}
			binder_debug(BINDER_DEBUG_DEAD_BINDER,
				     "%d:%d BC_DEAD_BINDER_DONE %016llx found %p\n",
				     proc->pid, thread->pid, (u64)cookie,
				     death);
			if (death == NULL) {
				binder_user_error("%d:%d BC_DEAD_BINDER_DONE %016llx not found\n",
					proc->pid, thread->pid, (u64)cookie);
				break;
			}

			list_del_init(&death->work.entry);
			if (death->work.type == BINDER_WORK_DEAD_BINDER_AND_CLEAR) {
				death->work.type = BINDER_WORK_CLEAR_DEATH_NOTIFICATION;
				if (thread->looper & (BINDER_LOOPER_STATE_REGISTERED | BINDER_LOOPER_STATE_ENTERED)) {
					list_add_tail(&death->work.entry, &thread->todo);
				} else {
					list_add_tail(&death->work.entry, &proc->todo);
					wake_up_interruptible(&proc->wait);
				}
			}
		} break;

		default:
			pr_err("%d:%d unknown command %d\n",
			       proc->pid, thread->pid, cmd);
			return -EINVAL;
		}
		*consumed = ptr - buffer;//binder数据的消费量赋值
	}
	return 0;
}

static void binder_stat_br(struct binder_proc *proc,
			   struct binder_thread *thread, uint32_t cmd)
{
	trace_binder_return(cmd);
	if (_IOC_NR(cmd) < ARRAY_SIZE(binder_stats.br)) {
		binder_stats.br[_IOC_NR(cmd)]++;
		proc->stats.br[_IOC_NR(cmd)]++;
		thread->stats.br[_IOC_NR(cmd)]++;
	}
}

static int binder_has_proc_work(struct binder_proc *proc,
				struct binder_thread *thread)
{
	return !list_empty(&proc->todo) ||
		(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

static int binder_has_thread_work(struct binder_thread *thread)
{
	return !list_empty(&thread->todo) || thread->return_error != BR_OK ||
		(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN);
}

//binder读
static int binder_thread_read(struct binder_proc *proc,
			      struct binder_thread *thread,
			      binder_uintptr_t binder_buffer, size_t size,
			      binder_size_t *consumed, int non_block)
{
	void __user *buffer = (void __user *)(uintptr_t)binder_buffer;//buffer首地址
	void __user *ptr = buffer + *consumed;//开始位置
	void __user *end = buffer + size;//结束位置

	int ret = 0;
	int wait_for_proc_work;

	if (*consumed == 0) {//need to do
		if (put_user(BR_NOOP, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
	}

retry:
	wait_for_proc_work = thread->transaction_stack == NULL &&
				list_empty(&thread->todo);

	if (thread->return_error != BR_OK && ptr < end) {
		if (thread->return_error2 != BR_OK) {
			if (put_user(thread->return_error2, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			binder_stat_br(proc, thread, thread->return_error2);
			if (ptr == end)
				goto done;
			thread->return_error2 = BR_OK;
		}
		if (put_user(thread->return_error, (uint32_t __user *)ptr))
			return -EFAULT;
		ptr += sizeof(uint32_t);
		binder_stat_br(proc, thread, thread->return_error);
		thread->return_error = BR_OK;
		goto done;
	}


	thread->looper |= BINDER_LOOPER_STATE_WAITING;
	if (wait_for_proc_work)
		proc->ready_threads++;

	binder_unlock(__func__);

	trace_binder_wait_for_work(wait_for_proc_work,
				   !!thread->transaction_stack,
				   !list_empty(&thread->todo));
	if (wait_for_proc_work) {
		if (!(thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
					BINDER_LOOPER_STATE_ENTERED))) {
			binder_user_error("%d:%d ERROR: Thread waiting for process work before calling BC_REGISTER_LOOPER or BC_ENTER_LOOPER (state %x)\n",
				proc->pid, thread->pid, thread->looper);
			wait_event_interruptible(binder_user_error_wait,
						 binder_stop_on_user_error < 2);//没数据可读的时候阻塞在这里等待唤醒
		}
		binder_set_nice(proc->default_priority);
		if (non_block) {
			if (!binder_has_proc_work(proc, thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_freezable_exclusive(proc->wait, binder_has_proc_work(proc, thread));
	} else {
		if (non_block) {
			if (!binder_has_thread_work(thread))
				ret = -EAGAIN;
		} else
			ret = wait_event_freezable(thread->wait, binder_has_thread_work(thread));
	}

	binder_lock(__func__);

	if (wait_for_proc_work)
		proc->ready_threads--;
	thread->looper &= ~BINDER_LOOPER_STATE_WAITING;

	if (ret)
		return ret;

	while (1) {
		uint32_t cmd;
		struct binder_transaction_data tr;
		struct binder_work *w;
		struct binder_transaction *t = NULL;

		if (!list_empty(&thread->todo)) {//优先判断线程todo队列是否为空
			w = list_first_entry(&thread->todo, struct binder_work,
					     entry);//根据成员entry从线程的todo队列取出binder_work
		} else if (!list_empty(&proc->todo) && wait_for_proc_work) {//再判断进程todo队列是否为空
			w = list_first_entry(&proc->todo, struct binder_work,
					     entry);//根据成员entry从进程的todo队列取出binder_work
		} else {
			/* no data added */
			if (ptr - buffer == 4 &&
			    !(thread->looper & BINDER_LOOPER_STATE_NEED_RETURN))
				goto retry;
			break;
		}

		if (end - ptr < sizeof(tr) + 4)
			break;

		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {//数据通信case
			t = container_of(w, struct binder_transaction, work);//根据成员binder_work拿到binder_transaction首地址
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {//通信完成
			cmd = BR_TRANSACTION_COMPLETE;
			if (put_user(cmd, (uint32_t __user *)ptr))//将BR_TRANSACTION_COMPLETE拷贝到用户空间
				return -EFAULT;
			ptr += sizeof(uint32_t);//指针后移

			binder_stat_br(proc, thread, cmd);
			binder_debug(BINDER_DEBUG_TRANSACTION_COMPLETE,
				     "%d:%d BR_TRANSACTION_COMPLETE\n",
				     proc->pid, thread->pid);

			list_del(&w->entry);//将binder_work从所在队列删除
			kfree(w);//释放binder_work内存
			binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
		} break;
		case BINDER_WORK_NODE: {
			struct binder_node *node = container_of(w, struct binder_node, work);
			uint32_t cmd = BR_NOOP;
			const char *cmd_name;
			int strong = node->internal_strong_refs || node->local_strong_refs;
			int weak = !hlist_empty(&node->refs) || node->local_weak_refs || strong;

			if (weak && !node->has_weak_ref) {
				cmd = BR_INCREFS;
				cmd_name = "BR_INCREFS";
				node->has_weak_ref = 1;
				node->pending_weak_ref = 1;
				node->local_weak_refs++;
			} else if (strong && !node->has_strong_ref) {
				cmd = BR_ACQUIRE;
				cmd_name = "BR_ACQUIRE";
				node->has_strong_ref = 1;
				node->pending_strong_ref = 1;
				node->local_strong_refs++;
			} else if (!strong && node->has_strong_ref) {
				cmd = BR_RELEASE;
				cmd_name = "BR_RELEASE";
				node->has_strong_ref = 0;
			} else if (!weak && node->has_weak_ref) {
				cmd = BR_DECREFS;
				cmd_name = "BR_DECREFS";
				node->has_weak_ref = 0;
			}
			if (cmd != BR_NOOP) {
				if (put_user(cmd, (uint32_t __user *)ptr))
					return -EFAULT;
				ptr += sizeof(uint32_t);
				if (put_user(node->ptr,
					     (binder_uintptr_t __user *)ptr))
					return -EFAULT;
				ptr += sizeof(binder_uintptr_t);
				if (put_user(node->cookie,
					     (binder_uintptr_t __user *)ptr))
					return -EFAULT;
				ptr += sizeof(binder_uintptr_t);

				binder_stat_br(proc, thread, cmd);
				binder_debug(BINDER_DEBUG_USER_REFS,
					     "%d:%d %s %d u%016llx c%016llx\n",
					     proc->pid, thread->pid, cmd_name,
					     node->debug_id,
					     (u64)node->ptr, (u64)node->cookie);
			} else {
				list_del_init(&w->entry);
				if (!weak && !strong) {
					binder_debug(BINDER_DEBUG_INTERNAL_REFS,
						     "%d:%d node %d u%016llx c%016llx deleted\n",
						     proc->pid, thread->pid,
						     node->debug_id,
						     (u64)node->ptr,
						     (u64)node->cookie);
					rb_erase(&node->rb_node, &proc->nodes);
					kfree(node);
					binder_stats_deleted(BINDER_STAT_NODE);
				} else {
					binder_debug(BINDER_DEBUG_INTERNAL_REFS,
						     "%d:%d node %d u%016llx c%016llx state unchanged\n",
						     proc->pid, thread->pid,
						     node->debug_id,
						     (u64)node->ptr,
						     (u64)node->cookie);
				}
			}
		} break;
		case BINDER_WORK_DEAD_BINDER:
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death;
			uint32_t cmd;

			death = container_of(w, struct binder_ref_death, work);
			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION)
				cmd = BR_CLEAR_DEATH_NOTIFICATION_DONE;
			else
				cmd = BR_DEAD_BINDER;
			if (put_user(cmd, (uint32_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(uint32_t);
			if (put_user(death->cookie,
				     (binder_uintptr_t __user *)ptr))
				return -EFAULT;
			ptr += sizeof(binder_uintptr_t);
			binder_stat_br(proc, thread, cmd);
			binder_debug(BINDER_DEBUG_DEATH_NOTIFICATION,
				     "%d:%d %s %016llx\n",
				      proc->pid, thread->pid,
				      cmd == BR_DEAD_BINDER ?
				      "BR_DEAD_BINDER" :
				      "BR_CLEAR_DEATH_NOTIFICATION_DONE",
				      (u64)death->cookie);

			if (w->type == BINDER_WORK_CLEAR_DEATH_NOTIFICATION) {
				list_del(&w->entry);
				kfree(death);
				binder_stats_deleted(BINDER_STAT_DEATH);
			} else
				list_move(&w->entry, &proc->delivered_death);
			if (cmd == BR_DEAD_BINDER)
				goto done; /* DEAD_BINDER notifications can cause transactions */
		} break;
		}

		if (!t)
			continue;

		BUG_ON(t->buffer == NULL);
		if (t->buffer->target_node) {//目标binder_node非空
			struct binder_node *target_node = t->buffer->target_node;//目标binder_node

			//目标服务指针赋值
			tr.target.ptr = target_node->ptr;
			tr.cookie =  target_node->cookie;
			t->saved_priority = task_nice(current);
			if (t->priority < target_node->min_priority &&
			    !(t->flags & TF_ONE_WAY))
				binder_set_nice(t->priority);
			else if (!(t->flags & TF_ONE_WAY) ||
				 t->saved_priority > target_node->min_priority)
				binder_set_nice(target_node->min_priority);
			cmd = BR_TRANSACTION;//cmd后面会拷贝到用户空间使用
		} else {//目标binder_node为空，证明是回复事务
			tr.target.ptr = 0;
			tr.cookie = 0;
			cmd = BR_REPLY;//后面会拷贝到用户空间使用
		}
		tr.code = t->code;
		tr.flags = t->flags;
		tr.sender_euid = from_kuid(current_user_ns(), t->sender_euid);

		if (t->from) {
			struct task_struct *sender = t->from->proc->tsk;

			tr.sender_pid = task_tgid_nr_ns(sender,
							task_active_pid_ns(current));
		} else {
			tr.sender_pid = 0;
		}

		tr.data_size = t->buffer->data_size;//data_size赋值
		tr.offsets_size = t->buffer->offsets_size;//offsets_size赋值
		tr.data.ptr.buffer = (binder_uintptr_t)(
					(uintptr_t)t->buffer->data +
					proc->user_buffer_offset);//data部分的用户空间地址
		tr.data.ptr.offsets = tr.data.ptr.buffer +
					ALIGN(t->buffer->data_size,
					    sizeof(void *));//offsets部分的用户空间地址

		if (put_user(cmd, (uint32_t __user *)ptr))//把cmd的值从内核空间拷贝到用户空间ptr
			return -EFAULT;
		ptr += sizeof(uint32_t);//指针后移
		if (copy_to_user(ptr, &tr, sizeof(tr)))//把传输的数据binder_transaction_data从内核空间拷贝到用户空间地址ptr
			return -EFAULT;
		ptr += sizeof(tr);//指针后移

		trace_binder_transaction_received(t);
		binder_stat_br(proc, thread, cmd);
		binder_debug(BINDER_DEBUG_TRANSACTION,
			     "%d:%d %s %d %d:%d, cmd %d size %zd-%zd ptr %016llx-%016llx\n",
			     proc->pid, thread->pid,
			     (cmd == BR_TRANSACTION) ? "BR_TRANSACTION" :
			     "BR_REPLY",
			     t->debug_id, t->from ? t->from->proc->pid : 0,
			     t->from ? t->from->pid : 0, cmd,
			     t->buffer->data_size, t->buffer->offsets_size,
			     (u64)tr.data.ptr.buffer, (u64)tr.data.ptr.offsets);

		list_del(&t->work.entry);//将binder_transaction从其所在队列中删除
		t->buffer->allow_user_free = 1;
		if (cmd == BR_TRANSACTION && !(t->flags & TF_ONE_WAY)) {//发送事务，接收方需要回复
			t->to_parent = thread->transaction_stack;//压栈，通过to_parent成员把binder_transaction串成一个栈结构
			t->to_thread = thread;//发送事务给接收方处理线程赋值当前线程
			thread->transaction_stack = t;//当前binder_transaction放入thread->transaction_stack，后面回复数据时要从这里取出
		} else {//回复事务
			t->buffer->transaction = NULL;//置空
			kfree(t);//释放当前binder_transaction
			binder_stats_deleted(BINDER_STAT_TRANSACTION);
		}
		break;
	}

done:

	*consumed = ptr - buffer;//计算往用户空间buffer拷贝数据的进度
	if (proc->requested_threads + proc->ready_threads == 0 &&
	    proc->requested_threads_started < proc->max_threads &&
	    (thread->looper & (BINDER_LOOPER_STATE_REGISTERED |
	     BINDER_LOOPER_STATE_ENTERED)) /* the user-space code fails to */
	     /*spawn a new thread if we leave this out */) {//如果没有可请求线程且线程数未达到最大值，新建线程
		proc->requested_threads++;
		binder_debug(BINDER_DEBUG_THREADS,
			     "%d:%d BR_SPAWN_LOOPER\n",
			     proc->pid, thread->pid);
		if (put_user(BR_SPAWN_LOOPER, (uint32_t __user *)buffer))//通过向用户空间buffer拷贝BR_SPAWN_LOOPER指令新建binder处理线程
			return -EFAULT;
		binder_stat_br(proc, thread, BR_SPAWN_LOOPER);
	}
	return 0;
}

//释放binder_work队列
static void binder_release_work(struct list_head *list)
{
	struct binder_work *w;

	while (!list_empty(list)) {//循环，直到链表为空
		w = list_first_entry(list, struct binder_work, entry);//通过成员entry获取链表第一个binder_work
		list_del_init(&w->entry);//通过成员entry将binder_work从链表删除
		switch (w->type) {
		case BINDER_WORK_TRANSACTION: {
			struct binder_transaction *t;

			t = container_of(w, struct binder_transaction, work);
			if (t->buffer->target_node &&
			    !(t->flags & TF_ONE_WAY)) {//需要接收方回复的发送事务
				binder_send_failed_reply(t, BR_DEAD_REPLY);//走失败流程
			} else {
				binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
					"undelivered transaction %d\n",
					t->debug_id);
				t->buffer->transaction = NULL;//置空
				kfree(t);//释放事务binder_transaction内存
				binder_stats_deleted(BINDER_STAT_TRANSACTION);
			}
		} break;
		case BINDER_WORK_TRANSACTION_COMPLETE: {
			binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
				"undelivered TRANSACTION_COMPLETE\n");
			kfree(w);//释放binder_work内存
			binder_stats_deleted(BINDER_STAT_TRANSACTION_COMPLETE);
		} break;
		case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		case BINDER_WORK_CLEAR_DEATH_NOTIFICATION: {
			struct binder_ref_death *death;

			death = container_of(w, struct binder_ref_death, work);
			binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
				"undelivered death notification, %016llx\n",
				(u64)death->cookie);
			kfree(death);
			binder_stats_deleted(BINDER_STAT_DEATH);
		} break;
		default:
			pr_err("unexpected work type, %d, not freed\n",
			       w->type);
			break;
		}
	}

}

//获取处理线程
static struct binder_thread *binder_get_thread(struct binder_proc *proc)
{
	struct binder_thread *thread = NULL;
	struct rb_node *parent = NULL;
	struct rb_node **p = &proc->threads.rb_node;//获取threads红黑树根节点

	//遍历proc->threads红黑树查找
	while (*p) {
		parent = *p;
		thread = rb_entry(parent, struct binder_thread, rb_node);//通过成员rb_node获取binder_thread的首地址

		if (current->pid < thread->pid)
			p = &(*p)->rb_left;
		else if (current->pid > thread->pid)
			p = &(*p)->rb_right;
		else
			break;
	}
	if (*p == NULL) {//没找到，则创建一个binder_thread
		thread = kzalloc(sizeof(*thread), GFP_KERNEL);//申请内存给binder_thread
		if (thread == NULL)
			return NULL;
		binder_stats_created(BINDER_STAT_THREAD);
		thread->proc = proc;
		thread->pid = current->pid;//设置当前线程的pid，current为当前线程(linux特性，线程实际是共享地址空间的进程)
		init_waitqueue_head(&thread->wait);//初始化线程的等待队列
		INIT_LIST_HEAD(&thread->todo);//初始化todo队列
		//插入到红黑树
		rb_link_node(&thread->rb_node, parent, p);
		rb_insert_color(&thread->rb_node, &proc->threads);
		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		thread->return_error = BR_OK;
		thread->return_error2 = BR_OK;
	}
	return thread;
}

//释放binder线程
static int binder_free_thread(struct binder_proc *proc,
			      struct binder_thread *thread)
{
	struct binder_transaction *t;
	struct binder_transaction *send_reply = NULL;
	int active_transactions = 0;

	rb_erase(&thread->rb_node, &proc->threads);//通过成员rb_node将当前binder_thread从threads红黑树删除
	t = thread->transaction_stack;//取出还没完成的传输事务
	if (t && t->to_thread == thread)
		send_reply = t;
	while (t) {//从transaction_stack开始遍历该线程所有的事务，做置空操作
		active_transactions++;
		binder_debug(BINDER_DEBUG_DEAD_TRANSACTION,
			     "release %d:%d transaction %d %s, still active\n",
			      proc->pid, thread->pid,
			     t->debug_id,
			     (t->to_thread == thread) ? "in" : "out");

		if (t->to_thread == thread) {//该事务的目标线程是当前线程，证明当前线程是接收线程
			t->to_proc = NULL;
			t->to_thread = NULL;
			if (t->buffer) {
				t->buffer->transaction = NULL;
				t->buffer = NULL;
			}
			t = t->to_parent;
		} else if (t->from == thread) {//发送线程
			t->from = NULL;
			t = t->from_parent;
		} else
			BUG();
	}
	if (send_reply)
		binder_send_failed_reply(send_reply, BR_DEAD_REPLY);//走失败流程
	binder_release_work(&thread->todo);//释放线程的todo队列
	kfree(thread);//释放binder_thread内存
	binder_stats_deleted(BINDER_STAT_THREAD);
	return active_transactions;
}

static unsigned int binder_poll(struct file *filp,
				struct poll_table_struct *wait)
{
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread = NULL;
	int wait_for_proc_work;

	binder_lock(__func__);

	thread = binder_get_thread(proc);

	wait_for_proc_work = thread->transaction_stack == NULL &&
		list_empty(&thread->todo) && thread->return_error == BR_OK;

	binder_unlock(__func__);

	if (wait_for_proc_work) {
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
		poll_wait(filp, &proc->wait, wait);
		if (binder_has_proc_work(proc, thread))
			return POLLIN;
	} else {
		if (binder_has_thread_work(thread))
			return POLLIN;
		poll_wait(filp, &thread->wait, wait);
		if (binder_has_thread_work(thread))
			return POLLIN;
	}
	return 0;
}

//binder读写
static int binder_ioctl_write_read(struct file *filp,
				unsigned int cmd, unsigned long arg,
				struct binder_thread *thread)
{
	int ret = 0;
	struct binder_proc *proc = filp->private_data;//拿到当前的binder_proc
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;//用户空间的binder_write_read的用户空间地址
	struct binder_write_read bwr;

	if (size != sizeof(struct binder_write_read)) {
		ret = -EINVAL;
		goto out;
	}
	if (copy_from_user(&bwr, ubuf, sizeof(bwr))) {//把用户空间的binder_write_read内容拷贝到内核空间的binder_write_read
		ret = -EFAULT;
		goto out;
	}
	binder_debug(BINDER_DEBUG_READ_WRITE,
		     "%d:%d write %lld at %016llx, read %lld at %016llx\n",
		     proc->pid, thread->pid,
		     (u64)bwr.write_size, (u64)bwr.write_buffer,
		     (u64)bwr.read_size, (u64)bwr.read_buffer);

	if (bwr.write_size > 0) {//有写的内容
		ret = binder_thread_write(proc, thread,
					  bwr.write_buffer,
					  bwr.write_size,
					  &bwr.write_consumed);//调用binder写方法
		trace_binder_write_done(ret);
		if (ret < 0) {
			bwr.read_consumed = 0;
			if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
				ret = -EFAULT;
			goto out;
		}
	}
	if (bwr.read_size > 0) {//有读的内容
		ret = binder_thread_read(proc, thread, bwr.read_buffer,
					 bwr.read_size,
					 &bwr.read_consumed,
					 filp->f_flags & O_NONBLOCK);//调用binder读方法
		trace_binder_read_done(ret);
		if (!list_empty(&proc->todo))
			wake_up_interruptible(&proc->wait);
		if (ret < 0) {
			if (copy_to_user(ubuf, &bwr, sizeof(bwr)))
				ret = -EFAULT;
			goto out;
		}
	}
	binder_debug(BINDER_DEBUG_READ_WRITE,
		     "%d:%d wrote %lld of %lld, read return %lld of %lld\n",
		     proc->pid, thread->pid,
		     (u64)bwr.write_consumed, (u64)bwr.write_size,
		     (u64)bwr.read_consumed, (u64)bwr.read_size);
	if (copy_to_user(ubuf, &bwr, sizeof(bwr))) {//将内核空间的bwr拷贝到用户空间的bwr
		ret = -EFAULT;
		goto out;
	}
out:
	return ret;
}

//设置为binder守护进程
static int binder_ioctl_set_ctx_mgr(struct file *filp)
{
	int ret = 0;
	struct binder_proc *proc = filp->private_data;//从private_data拿到binder_proc
	kuid_t curr_euid = current_euid();//获取当前进程euid

	if (binder_context_mgr_node != NULL) {
		pr_err("BINDER_SET_CONTEXT_MGR already set\n");
		ret = -EBUSY;
		goto out;
	}
	ret = security_binder_set_context_mgr(proc->tsk);
	if (ret < 0)
		goto out;
	if (uid_valid(binder_context_mgr_uid)) {
		if (!uid_eq(binder_context_mgr_uid, curr_euid)) {
			pr_err("BINDER_SET_CONTEXT_MGR bad uid %d != %d\n",
			       from_kuid(&init_user_ns, curr_euid),
			       from_kuid(&init_user_ns,
					binder_context_mgr_uid));
			ret = -EPERM;
			goto out;
		}
	} else {
		binder_context_mgr_uid = curr_euid;//设置守护进程uid为当前进程的euid
	}
	binder_context_mgr_node = binder_new_node(proc, 0, 0);//新建一个binder_node
		ret = -ENOMEM;
		goto out;
	}
	//处理引用计数
	binder_context_mgr_node->local_weak_refs++;
	binder_context_mgr_node->local_strong_refs++;
	binder_context_mgr_node->has_strong_ref = 1;
	binder_context_mgr_node->has_weak_ref = 1;
out:
	return ret;
}

//驱动ioctl函数
static long binder_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	int ret;
	struct binder_proc *proc = filp->private_data;
	struct binder_thread *thread;
	unsigned int size = _IOC_SIZE(cmd);
	void __user *ubuf = (void __user *)arg;//用户空间地址

	/*pr_info("binder_ioctl: %d:%d %x %lx\n",
			proc->pid, current->pid, cmd, arg);*/

	trace_binder_ioctl(cmd, arg);

	ret = wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret)
		goto err_unlocked;

	binder_lock(__func__);
	thread = binder_get_thread(proc);//获取处理线程
	if (thread == NULL) {
		ret = -ENOMEM;
		goto err;
	}

	switch (cmd) {
	case BINDER_WRITE_READ://读写
		ret = binder_ioctl_write_read(filp, cmd, arg, thread);
		if (ret)
			goto err;
		break;
	case BINDER_SET_MAX_THREADS://设置最大线程数
		if (copy_from_user(&proc->max_threads, ubuf, sizeof(proc->max_threads))) {//从用户空间拷贝最大线程数到内核，赋值给proc->max_threads
			ret = -EINVAL;
			goto err;
		}
		break;
	case BINDER_SET_CONTEXT_MGR://设置为binder守护进程
		ret = binder_ioctl_set_ctx_mgr(filp);
		if (ret)
			goto err;
		break;
	case BINDER_THREAD_EXIT://释放binder线程
		binder_debug(BINDER_DEBUG_THREADS, "%d:%d exit\n",
			     proc->pid, thread->pid);
		binder_free_thread(proc, thread);
		thread = NULL;
		break;
	case BINDER_VERSION: {//binder驱动版本
		struct binder_version __user *ver = ubuf;

		if (size != sizeof(struct binder_version)) {
			ret = -EINVAL;
			goto err;
		}
		if (put_user(BINDER_CURRENT_PROTOCOL_VERSION,
			     &ver->protocol_version)) {//将版本号拷贝到用户空间
			ret = -EINVAL;
			goto err;
		}
		break;
	}
	default:
		ret = -EINVAL;
		goto err;
	}
	ret = 0;
err:
	if (thread)
		thread->looper &= ~BINDER_LOOPER_STATE_NEED_RETURN;
	binder_unlock(__func__);
	wait_event_interruptible(binder_user_error_wait, binder_stop_on_user_error < 2);
	if (ret && ret != -ERESTARTSYS)
		pr_info("%d:%d ioctl %x %lx returned %d\n", proc->pid, current->pid, cmd, arg, ret);
err_unlocked:
	trace_binder_ioctl_done(ret);
	return ret;
}

static void binder_vma_open(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%d open vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));
}

static void binder_vma_close(struct vm_area_struct *vma)
{
	struct binder_proc *proc = vma->vm_private_data;

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%d close vm area %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));
	proc->vma = NULL;
	proc->vma_vm_mm = NULL;
	binder_defer_work(proc, BINDER_DEFERRED_PUT_FILES);
}

static int binder_vm_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static const struct vm_operations_struct binder_vm_ops = {
	.open = binder_vma_open,
	.close = binder_vma_close,
	.fault = binder_vm_fault,
};

//驱动mmap函数
static int binder_mmap(struct file *filp, struct vm_area_struct *vma)//vma 用户虚拟空间
{
	int ret;
	struct vm_struct *area;
	struct binder_proc *proc = filp->private_data;//1、取出binder_proc指针
	const char *failure_string;
	struct binder_buffer *buffer;

	if (proc->tsk != current)
		return -EINVAL;

	if ((vma->vm_end - vma->vm_start) > SZ_4M)//申请空间大小不超过SZ_4M
		vma->vm_end = vma->vm_start + SZ_4M;

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "binder_mmap: %d %lx-%lx (%ld K) vma %lx pagep %lx\n",
		     proc->pid, vma->vm_start, vma->vm_end,
		     (vma->vm_end - vma->vm_start) / SZ_1K, vma->vm_flags,
		     (unsigned long)pgprot_val(vma->vm_page_prot));

	if (vma->vm_flags & FORBIDDEN_MMAP_FLAGS) {
		ret = -EPERM;
		failure_string = "bad vm_flags";
		goto err_bad_arg;
	}
	vma->vm_flags = (vma->vm_flags | VM_DONTCOPY) & ~VM_MAYWRITE;

	mutex_lock(&binder_mmap_lock);//加锁
	if (proc->buffer) {
		ret = -EBUSY;
		failure_string = "already mapped";
		goto err_already_mapped;
	}

	area = get_vm_area(vma->vm_end - vma->vm_start, VM_IOREMAP);//2、申请一块内核虚拟空间
	if (area == NULL) {
		ret = -ENOMEM;
		failure_string = "get_vm_area";
		goto err_get_vm_area_failed;
	}
	proc->buffer = area->addr;//3、内核空间首地址赋值给proc->buffer
	proc->user_buffer_offset = vma->vm_start - (uintptr_t)proc->buffer;//4、计算出内核空间与用户空间的偏移量
	mutex_unlock(&binder_mmap_lock);//释放锁

#ifdef CONFIG_CPU_CACHE_VIPT
	if (cache_is_vipt_aliasing()) {
		while (CACHE_COLOUR((vma->vm_start ^ (uint32_t)proc->buffer))) {
			pr_info("binder_mmap: %d %lx-%lx maps %p bad alignment\n", proc->pid, vma->vm_start, vma->vm_end, proc->buffer);
			vma->vm_start += PAGE_SIZE;
		}
	}
#endif
    //5、申请页指针数组内存，首地址赋值给proc->pages。因为数组的长度事先无法确定，所以proc->pages声明为双重指针。
	proc->pages = kzalloc(sizeof(proc->pages[0]) * ((vma->vm_end - vma->vm_start) / PAGE_SIZE), GFP_KERNEL);
	if (proc->pages == NULL) {
		ret = -ENOMEM;
		failure_string = "alloc page array";
		goto err_alloc_pages_failed;
	}
	proc->buffer_size = vma->vm_end - vma->vm_start;//5、映射的空间大小赋值给proc->buffer_size

	vma->vm_ops = &binder_vm_ops;
	vma->vm_private_data = proc;

	if (binder_update_page_range(proc, 1, proc->buffer, proc->buffer + PAGE_SIZE, vma)) {//6、分配物理内存，并建立映射关系，大小为一页。为什么不申请整个binder_mmap的大小128k？因为需要节省物理内存，后续等需要用的时候再申请。
		ret = -ENOMEM;
		failure_string = "alloc small buf";
		goto err_alloc_small_buf_failed;
	}
	buffer = proc->buffer;//7、内核空间首地址赋值给binder_buffer的指针buffer
	INIT_LIST_HEAD(&proc->buffers);//8、初始化proc->buffers链表
	list_add(&buffer->entry, &proc->buffers);//9、将binder_buffer插入到proc->buffers链表
	buffer->free = 1;//10、free成员赋值1
	binder_insert_free_buffer(proc, buffer);//11、将binder_buffer插入到未分配的buffer红黑树free_buffer
	proc->free_async_space = proc->buffer_size / 2;
	barrier();
	proc->files = get_files_struct(current);
	proc->vma = vma;//描述用户地址空间结构体赋值
	proc->vma_vm_mm = vma->vm_mm;

	/*pr_info("binder_mmap: %d %lx-%lx maps %p\n",
		 proc->pid, vma->vm_start, vma->vm_end, proc->buffer);*/
	return 0;

err_alloc_small_buf_failed:
	kfree(proc->pages);
	proc->pages = NULL;
err_alloc_pages_failed:
	mutex_lock(&binder_mmap_lock);
	vfree(proc->buffer);
	proc->buffer = NULL;
err_get_vm_area_failed:
err_already_mapped:
	mutex_unlock(&binder_mmap_lock);
err_bad_arg:
	pr_err("binder_mmap: %d %lx-%lx %s failed %d\n",
	       proc->pid, vma->vm_start, vma->vm_end, failure_string, ret);
	return ret;
}

//驱动open函数
static int binder_open(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc;//1、声明binder_proc指针

	binder_debug(BINDER_DEBUG_OPEN_CLOSE, "binder_open: %d:%d\n",
		     current->group_leader->pid, current->pid);

	proc = kzalloc(sizeof(*proc), GFP_KERNEL);//2、分配binder_proc内存
	if (proc == NULL)
		return -ENOMEM;
	get_task_struct(current);
	proc->tsk = current;
	INIT_LIST_HEAD(&proc->todo);//4、设置todo队列
	init_waitqueue_head(&proc->wait);//5、设置wait队列
	proc->default_priority = task_nice(current);

	binder_lock(__func__);//加锁

	binder_stats_created(BINDER_STAT_PROC);
	hlist_add_head(&proc->proc_node, &binder_procs);//7、将当前binder_proc节点加入到binder_procs链表中
	proc->pid = current->group_leader->pid;//8、设置pid
	INIT_LIST_HEAD(&proc->delivered_death);
	filp->private_data = proc;//9、把文件private_data指向binder_proc结构体

	binder_unlock(__func__);//释放锁

	if (binder_debugfs_dir_entry_proc) {
		char strbuf[11];

		snprintf(strbuf, sizeof(strbuf), "%u", proc->pid);
		proc->debugfs_entry = debugfs_create_file(strbuf, S_IRUGO,
			binder_debugfs_dir_entry_proc, proc, &binder_proc_fops);
	}

	return 0;
}

static int binder_flush(struct file *filp, fl_owner_t id)
{
	struct binder_proc *proc = filp->private_data;

	binder_defer_work(proc, BINDER_DEFERRED_FLUSH);

	return 0;
}

static void binder_deferred_flush(struct binder_proc *proc)
{
	struct rb_node *n;
	int wake_count = 0;

	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n)) {
		struct binder_thread *thread = rb_entry(n, struct binder_thread, rb_node);

		thread->looper |= BINDER_LOOPER_STATE_NEED_RETURN;
		if (thread->looper & BINDER_LOOPER_STATE_WAITING) {
			wake_up_interruptible(&thread->wait);
			wake_count++;
		}
	}
	wake_up_interruptible_all(&proc->wait);

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "binder_flush: %d woke %d threads\n", proc->pid,
		     wake_count);
}

static int binder_release(struct inode *nodp, struct file *filp)
{
	struct binder_proc *proc = filp->private_data;

	debugfs_remove(proc->debugfs_entry);
	binder_defer_work(proc, BINDER_DEFERRED_RELEASE);

	return 0;
}

static int binder_node_release(struct binder_node *node, int refs)
{
	struct binder_ref *ref;
	int death = 0;

	list_del_init(&node->work.entry);
	binder_release_work(&node->async_todo);

	if (hlist_empty(&node->refs)) {
		kfree(node);
		binder_stats_deleted(BINDER_STAT_NODE);

		return refs;
	}

	node->proc = NULL;
	node->local_strong_refs = 0;
	node->local_weak_refs = 0;
	hlist_add_head(&node->dead_node, &binder_dead_nodes);

	hlist_for_each_entry(ref, &node->refs, node_entry) {
		refs++;

		if (!ref->death)
			continue;

		death++;

		if (list_empty(&ref->death->work.entry)) {
			ref->death->work.type = BINDER_WORK_DEAD_BINDER;
			list_add_tail(&ref->death->work.entry,
				      &ref->proc->todo);
			wake_up_interruptible(&ref->proc->wait);
		} else
			BUG();
	}

	binder_debug(BINDER_DEBUG_DEAD_BINDER,
		     "node %d now dead, refs %d, death %d\n",
		     node->debug_id, refs, death);

	return refs;
}

static void binder_deferred_release(struct binder_proc *proc)
{
	struct binder_transaction *t;
	struct rb_node *n;
	int threads, nodes, incoming_refs, outgoing_refs, buffers,
		active_transactions, page_count;

	BUG_ON(proc->vma);
	BUG_ON(proc->files);

	hlist_del(&proc->proc_node);

	if (binder_context_mgr_node && binder_context_mgr_node->proc == proc) {
		binder_debug(BINDER_DEBUG_DEAD_BINDER,
			     "%s: %d context_mgr_node gone\n",
			     __func__, proc->pid);
		binder_context_mgr_node = NULL;
	}

	threads = 0;
	active_transactions = 0;
	while ((n = rb_first(&proc->threads))) {
		struct binder_thread *thread;

		thread = rb_entry(n, struct binder_thread, rb_node);
		threads++;
		active_transactions += binder_free_thread(proc, thread);
	}

	nodes = 0;
	incoming_refs = 0;
	while ((n = rb_first(&proc->nodes))) {
		struct binder_node *node;

		node = rb_entry(n, struct binder_node, rb_node);
		nodes++;
		rb_erase(&node->rb_node, &proc->nodes);
		incoming_refs = binder_node_release(node, incoming_refs);
	}

	outgoing_refs = 0;
	while ((n = rb_first(&proc->refs_by_desc))) {
		struct binder_ref *ref;

		ref = rb_entry(n, struct binder_ref, rb_node_desc);
		outgoing_refs++;
		binder_delete_ref(ref);
	}

	binder_release_work(&proc->todo);
	binder_release_work(&proc->delivered_death);

	buffers = 0;
	while ((n = rb_first(&proc->allocated_buffers))) {
		struct binder_buffer *buffer;

		buffer = rb_entry(n, struct binder_buffer, rb_node);

		t = buffer->transaction;
		if (t) {
			t->buffer = NULL;
			buffer->transaction = NULL;
			pr_err("release proc %d, transaction %d, not freed\n",
			       proc->pid, t->debug_id);
			/*BUG();*/
		}

		binder_free_buf(proc, buffer);
		buffers++;
	}

	binder_stats_deleted(BINDER_STAT_PROC);

	page_count = 0;
	if (proc->pages) {
		int i;

		for (i = 0; i < proc->buffer_size / PAGE_SIZE; i++) {
			void *page_addr;

			if (!proc->pages[i])
				continue;

			page_addr = proc->buffer + i * PAGE_SIZE;
			binder_debug(BINDER_DEBUG_BUFFER_ALLOC,
				     "%s: %d: page %d at %p not freed\n",
				     __func__, proc->pid, i, page_addr);
			unmap_kernel_range((unsigned long)page_addr, PAGE_SIZE);
			__free_page(proc->pages[i]);
			page_count++;
		}
		kfree(proc->pages);
		vfree(proc->buffer);
	}

	put_task_struct(proc->tsk);

	binder_debug(BINDER_DEBUG_OPEN_CLOSE,
		     "%s: %d threads %d, nodes %d (ref %d), refs %d, active transactions %d, buffers %d, pages %d\n",
		     __func__, proc->pid, threads, nodes, incoming_refs,
		     outgoing_refs, active_transactions, buffers, page_count);

	kfree(proc);
}

static void binder_deferred_func(struct work_struct *work)
{
	struct binder_proc *proc;
	struct files_struct *files;

	int defer;

	do {
		binder_lock(__func__);
		mutex_lock(&binder_deferred_lock);
		if (!hlist_empty(&binder_deferred_list)) {
			proc = hlist_entry(binder_deferred_list.first,
					struct binder_proc, deferred_work_node);
			hlist_del_init(&proc->deferred_work_node);
			defer = proc->deferred_work;
			proc->deferred_work = 0;
		} else {
			proc = NULL;
			defer = 0;
		}
		mutex_unlock(&binder_deferred_lock);

		files = NULL;
		if (defer & BINDER_DEFERRED_PUT_FILES) {
			files = proc->files;
			if (files)
				proc->files = NULL;
		}

		if (defer & BINDER_DEFERRED_FLUSH)
			binder_deferred_flush(proc);

		if (defer & BINDER_DEFERRED_RELEASE)
			binder_deferred_release(proc); /* frees proc */

		binder_unlock(__func__);
		if (files)
			put_files_struct(files);
	} while (proc);
}
static DECLARE_WORK(binder_deferred_work, binder_deferred_func);

static void
binder_defer_work(struct binder_proc *proc, enum binder_deferred_state defer)
{
	mutex_lock(&binder_deferred_lock);
	proc->deferred_work |= defer;
	if (hlist_unhashed(&proc->deferred_work_node)) {
		hlist_add_head(&proc->deferred_work_node,
				&binder_deferred_list);
		queue_work(binder_deferred_workqueue, &binder_deferred_work);
	}
	mutex_unlock(&binder_deferred_lock);
}

static void print_binder_transaction(struct seq_file *m, const char *prefix,
				     struct binder_transaction *t)
{
	seq_printf(m,
		   "%s %d: %p from %d:%d to %d:%d code %x flags %x pri %ld r%d",
		   prefix, t->debug_id, t,
		   t->from ? t->from->proc->pid : 0,
		   t->from ? t->from->pid : 0,
		   t->to_proc ? t->to_proc->pid : 0,
		   t->to_thread ? t->to_thread->pid : 0,
		   t->code, t->flags, t->priority, t->need_reply);
	if (t->buffer == NULL) {
		seq_puts(m, " buffer free\n");
		return;
	}
	if (t->buffer->target_node)
		seq_printf(m, " node %d",
			   t->buffer->target_node->debug_id);
	seq_printf(m, " size %zd:%zd data %p\n",
		   t->buffer->data_size, t->buffer->offsets_size,
		   t->buffer->data);
}

static void print_binder_buffer(struct seq_file *m, const char *prefix,
				struct binder_buffer *buffer)
{
	seq_printf(m, "%s %d: %p size %zd:%zd %s\n",
		   prefix, buffer->debug_id, buffer->data,
		   buffer->data_size, buffer->offsets_size,
		   buffer->transaction ? "active" : "delivered");
}

static void print_binder_work(struct seq_file *m, const char *prefix,
			      const char *transaction_prefix,
			      struct binder_work *w)
{
	struct binder_node *node;
	struct binder_transaction *t;

	switch (w->type) {
	case BINDER_WORK_TRANSACTION:
		t = container_of(w, struct binder_transaction, work);
		print_binder_transaction(m, transaction_prefix, t);
		break;
	case BINDER_WORK_TRANSACTION_COMPLETE:
		seq_printf(m, "%stransaction complete\n", prefix);
		break;
	case BINDER_WORK_NODE:
		node = container_of(w, struct binder_node, work);
		seq_printf(m, "%snode work %d: u%016llx c%016llx\n",
			   prefix, node->debug_id,
			   (u64)node->ptr, (u64)node->cookie);
		break;
	case BINDER_WORK_DEAD_BINDER:
		seq_printf(m, "%shas dead binder\n", prefix);
		break;
	case BINDER_WORK_DEAD_BINDER_AND_CLEAR:
		seq_printf(m, "%shas cleared dead binder\n", prefix);
		break;
	case BINDER_WORK_CLEAR_DEATH_NOTIFICATION:
		seq_printf(m, "%shas cleared death notification\n", prefix);
		break;
	default:
		seq_printf(m, "%sunknown work: type %d\n", prefix, w->type);
		break;
	}
}

static void print_binder_thread(struct seq_file *m,
				struct binder_thread *thread,
				int print_always)
{
	struct binder_transaction *t;
	struct binder_work *w;
	size_t start_pos = m->count;
	size_t header_pos;

	seq_printf(m, "  thread %d: l %02x\n", thread->pid, thread->looper);
	header_pos = m->count;
	t = thread->transaction_stack;
	while (t) {
		if (t->from == thread) {
			print_binder_transaction(m,
						 "    outgoing transaction", t);
			t = t->from_parent;
		} else if (t->to_thread == thread) {
			print_binder_transaction(m,
						 "    incoming transaction", t);
			t = t->to_parent;
		} else {
			print_binder_transaction(m, "    bad transaction", t);
			t = NULL;
		}
	}
	list_for_each_entry(w, &thread->todo, entry) {
		print_binder_work(m, "    ", "    pending transaction", w);
	}
	if (!print_always && m->count == header_pos)
		m->count = start_pos;
}

static void print_binder_node(struct seq_file *m, struct binder_node *node)
{
	struct binder_ref *ref;
	struct binder_work *w;
	int count;

	count = 0;
	hlist_for_each_entry(ref, &node->refs, node_entry)
		count++;

	seq_printf(m, "  node %d: u%016llx c%016llx hs %d hw %d ls %d lw %d is %d iw %d",
		   node->debug_id, (u64)node->ptr, (u64)node->cookie,
		   node->has_strong_ref, node->has_weak_ref,
		   node->local_strong_refs, node->local_weak_refs,
		   node->internal_strong_refs, count);
	if (count) {
		seq_puts(m, " proc");
		hlist_for_each_entry(ref, &node->refs, node_entry)
			seq_printf(m, " %d", ref->proc->pid);
	}
	seq_puts(m, "\n");
	list_for_each_entry(w, &node->async_todo, entry)
		print_binder_work(m, "    ",
				  "    pending async transaction", w);
}

static void print_binder_ref(struct seq_file *m, struct binder_ref *ref)
{
	seq_printf(m, "  ref %d: desc %d %snode %d s %d w %d d %p\n",
		   ref->debug_id, ref->desc, ref->node->proc ? "" : "dead ",
		   ref->node->debug_id, ref->strong, ref->weak, ref->death);
}

static void print_binder_proc(struct seq_file *m,
			      struct binder_proc *proc, int print_all)
{
	struct binder_work *w;
	struct rb_node *n;
	size_t start_pos = m->count;
	size_t header_pos;

	seq_printf(m, "proc %d\n", proc->pid);
	header_pos = m->count;

	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n))
		print_binder_thread(m, rb_entry(n, struct binder_thread,
						rb_node), print_all);
	for (n = rb_first(&proc->nodes); n != NULL; n = rb_next(n)) {
		struct binder_node *node = rb_entry(n, struct binder_node,
						    rb_node);
		if (print_all || node->has_async_transaction)
			print_binder_node(m, node);
	}
	if (print_all) {
		for (n = rb_first(&proc->refs_by_desc);
		     n != NULL;
		     n = rb_next(n))
			print_binder_ref(m, rb_entry(n, struct binder_ref,
						     rb_node_desc));
	}
	for (n = rb_first(&proc->allocated_buffers); n != NULL; n = rb_next(n))
		print_binder_buffer(m, "  buffer",
				    rb_entry(n, struct binder_buffer, rb_node));
	list_for_each_entry(w, &proc->todo, entry)
		print_binder_work(m, "  ", "  pending transaction", w);
	list_for_each_entry(w, &proc->delivered_death, entry) {
		seq_puts(m, "  has delivered dead binder\n");
		break;
	}
	if (!print_all && m->count == header_pos)
		m->count = start_pos;
}

static const char * const binder_return_strings[] = {
	"BR_ERROR",
	"BR_OK",
	"BR_TRANSACTION",
	"BR_REPLY",
	"BR_ACQUIRE_RESULT",
	"BR_DEAD_REPLY",
	"BR_TRANSACTION_COMPLETE",
	"BR_INCREFS",
	"BR_ACQUIRE",
	"BR_RELEASE",
	"BR_DECREFS",
	"BR_ATTEMPT_ACQUIRE",
	"BR_NOOP",
	"BR_SPAWN_LOOPER",
	"BR_FINISHED",
	"BR_DEAD_BINDER",
	"BR_CLEAR_DEATH_NOTIFICATION_DONE",
	"BR_FAILED_REPLY"
};

static const char * const binder_command_strings[] = {
	"BC_TRANSACTION",
	"BC_REPLY",
	"BC_ACQUIRE_RESULT",
	"BC_FREE_BUFFER",
	"BC_INCREFS",
	"BC_ACQUIRE",
	"BC_RELEASE",
	"BC_DECREFS",
	"BC_INCREFS_DONE",
	"BC_ACQUIRE_DONE",
	"BC_ATTEMPT_ACQUIRE",
	"BC_REGISTER_LOOPER",
	"BC_ENTER_LOOPER",
	"BC_EXIT_LOOPER",
	"BC_REQUEST_DEATH_NOTIFICATION",
	"BC_CLEAR_DEATH_NOTIFICATION",
	"BC_DEAD_BINDER_DONE"
};

static const char * const binder_objstat_strings[] = {
	"proc",
	"thread",
	"node",
	"ref",
	"death",
	"transaction",
	"transaction_complete"
};

static void print_binder_stats(struct seq_file *m, const char *prefix,
			       struct binder_stats *stats)
{
	int i;

	BUILD_BUG_ON(ARRAY_SIZE(stats->bc) !=
		     ARRAY_SIZE(binder_command_strings));
	for (i = 0; i < ARRAY_SIZE(stats->bc); i++) {
		if (stats->bc[i])
			seq_printf(m, "%s%s: %d\n", prefix,
				   binder_command_strings[i], stats->bc[i]);
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->br) !=
		     ARRAY_SIZE(binder_return_strings));
	for (i = 0; i < ARRAY_SIZE(stats->br); i++) {
		if (stats->br[i])
			seq_printf(m, "%s%s: %d\n", prefix,
				   binder_return_strings[i], stats->br[i]);
	}

	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) !=
		     ARRAY_SIZE(binder_objstat_strings));
	BUILD_BUG_ON(ARRAY_SIZE(stats->obj_created) !=
		     ARRAY_SIZE(stats->obj_deleted));
	for (i = 0; i < ARRAY_SIZE(stats->obj_created); i++) {
		if (stats->obj_created[i] || stats->obj_deleted[i])
			seq_printf(m, "%s%s: active %d total %d\n", prefix,
				binder_objstat_strings[i],
				stats->obj_created[i] - stats->obj_deleted[i],
				stats->obj_created[i]);
	}
}

static void print_binder_proc_stats(struct seq_file *m,
				    struct binder_proc *proc)
{
	struct binder_work *w;
	struct rb_node *n;
	int count, strong, weak;

	seq_printf(m, "proc %d\n", proc->pid);
	count = 0;
	for (n = rb_first(&proc->threads); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  threads: %d\n", count);
	seq_printf(m, "  requested threads: %d+%d/%d\n"
			"  ready threads %d\n"
			"  free async space %zd\n", proc->requested_threads,
			proc->requested_threads_started, proc->max_threads,
			proc->ready_threads, proc->free_async_space);
	count = 0;
	for (n = rb_first(&proc->nodes); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  nodes: %d\n", count);
	count = 0;
	strong = 0;
	weak = 0;
	for (n = rb_first(&proc->refs_by_desc); n != NULL; n = rb_next(n)) {
		struct binder_ref *ref = rb_entry(n, struct binder_ref,
						  rb_node_desc);
		count++;
		strong += ref->strong;
		weak += ref->weak;
	}
	seq_printf(m, "  refs: %d s %d w %d\n", count, strong, weak);

	count = 0;
	for (n = rb_first(&proc->allocated_buffers); n != NULL; n = rb_next(n))
		count++;
	seq_printf(m, "  buffers: %d\n", count);

	count = 0;
	list_for_each_entry(w, &proc->todo, entry) {
		switch (w->type) {
		case BINDER_WORK_TRANSACTION:
			count++;
			break;
		default:
			break;
		}
	}
	seq_printf(m, "  pending transactions: %d\n", count);

	print_binder_stats(m, "  ", &proc->stats);
}


static int binder_state_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	struct binder_node *node;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder state:\n");

	if (!hlist_empty(&binder_dead_nodes))
		seq_puts(m, "dead nodes:\n");
	hlist_for_each_entry(node, &binder_dead_nodes, dead_node)
		print_binder_node(m, node);

	hlist_for_each_entry(proc, &binder_procs, proc_node)
		print_binder_proc(m, proc, 1);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_stats_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder stats:\n");

	print_binder_stats(m, "", &binder_stats);

	hlist_for_each_entry(proc, &binder_procs, proc_node)
		print_binder_proc_stats(m, proc);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_transactions_show(struct seq_file *m, void *unused)
{
	struct binder_proc *proc;
	int do_lock = !binder_debug_no_lock;

	if (do_lock)
		binder_lock(__func__);

	seq_puts(m, "binder transactions:\n");
	hlist_for_each_entry(proc, &binder_procs, proc_node)
		print_binder_proc(m, proc, 0);
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static int binder_proc_show(struct seq_file *m, void *unused)
{
	struct binder_proc *itr;
	struct binder_proc *proc = m->private;
	int do_lock = !binder_debug_no_lock;
	bool valid_proc = false;

	if (do_lock)
		binder_lock(__func__);

	hlist_for_each_entry(itr, &binder_procs, proc_node) {
		if (itr == proc) {
			valid_proc = true;
			break;
		}
	}
	if (valid_proc) {
		seq_puts(m, "binder proc state:\n");
		print_binder_proc(m, proc, 1);
	}
	if (do_lock)
		binder_unlock(__func__);
	return 0;
}

static void print_binder_transaction_log_entry(struct seq_file *m,
					struct binder_transaction_log_entry *e)
{
	seq_printf(m,
		   "%d: %s from %d:%d to %d:%d node %d handle %d size %d:%d\n",
		   e->debug_id, (e->call_type == 2) ? "reply" :
		   ((e->call_type == 1) ? "async" : "call "), e->from_proc,
		   e->from_thread, e->to_proc, e->to_thread, e->to_node,
		   e->target_handle, e->data_size, e->offsets_size);
}

static int binder_transaction_log_show(struct seq_file *m, void *unused)
{
	struct binder_transaction_log *log = m->private;
	int i;

	if (log->full) {
		for (i = log->next; i < ARRAY_SIZE(log->entry); i++)
			print_binder_transaction_log_entry(m, &log->entry[i]);
	}
	for (i = 0; i < log->next; i++)
		print_binder_transaction_log_entry(m, &log->entry[i]);
	return 0;
}

//binder驱动操作函数
static const struct file_operations binder_fops = {
	.owner = THIS_MODULE,
	.poll = binder_poll,
	.unlocked_ioctl = binder_ioctl,
	.compat_ioctl = binder_ioctl,
	.mmap = binder_mmap,
	.open = binder_open,
	.flush = binder_flush,
	.release = binder_release,
};

static struct miscdevice binder_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "binder",
	.fops = &binder_fops
};

BINDER_DEBUG_ENTRY(state);
BINDER_DEBUG_ENTRY(stats);
BINDER_DEBUG_ENTRY(transactions);
BINDER_DEBUG_ENTRY(transaction_log);

static int __init binder_init(void)
{
	int ret;

	binder_deferred_workqueue = create_singlethread_workqueue("binder");
	if (!binder_deferred_workqueue)
		return -ENOMEM;

	binder_debugfs_dir_entry_root = debugfs_create_dir("binder", NULL);
	if (binder_debugfs_dir_entry_root)
		binder_debugfs_dir_entry_proc = debugfs_create_dir("proc",
						 binder_debugfs_dir_entry_root);
	ret = misc_register(&binder_miscdev);//misc设备注册
	if (binder_debugfs_dir_entry_root) {
		debugfs_create_file("state",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_state_fops);
		debugfs_create_file("stats",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_stats_fops);
		debugfs_create_file("transactions",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    NULL,
				    &binder_transactions_fops);
		debugfs_create_file("transaction_log",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    &binder_transaction_log,
				    &binder_transaction_log_fops);
		debugfs_create_file("failed_transaction_log",
				    S_IRUGO,
				    binder_debugfs_dir_entry_root,
				    &binder_transaction_log_failed,
				    &binder_transaction_log_fops);
	}
	return ret;
}

device_initcall(binder_init);//初始化

#define CREATE_TRACE_POINTS
#include "binder_trace.h"

MODULE_LICENSE("GPL v2");

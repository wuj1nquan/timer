#ifndef SPINLOCK_H
#define SPINLOCK_H


typedef struct spinlock {  // 自定义自旋锁变量
	int lock;
} spinlock_t;


void spinlock_init(spinlock_t *lock) { // 初始化自旋锁，将 lock 设为 0，表示解锁状态
	lock->lock = 0;
}

void spinlock_lock(spinlock_t *lock) {
	
	while (__sync_lock_test_and_set(&lock->lock, 1)) {}
	
	/**
	 * 1.尝试获取锁。使用 GCC 内建的 __sync_lock_test_and_set 原子操作设置 lock->lock 为 1
	 * 
	 * 2.如果 lock->lock 已经是 1，则表示锁已被其他线程持有，当前线程会在 while 循环中忙等待，直到获取锁成功
	 * 
	 */
}

int spinlock_trylock(spinlock_t *lock) {

	return __sync_lock_test_and_set(&lock->lock, 1) == 0;

	/**
	 * 1.尝试获取锁，但不会阻塞
	 * 
	 * 2. 如果获取锁成功，则返回 1（true）；否则返回 0（false）
	 */
}

void spinlock_unlock(spinlock_t *lock) {

	__sync_lock_release(&lock->lock);

	/**
	 *  释放锁。使用 GCC 内建的 __sync_lock_release 原子操作将 lock->lock 设置为 0 
	 */
}

void spinlock_destroy(spinlock_t *lock) {  // 暂时没有作用
	(void) lock;
}

#endif

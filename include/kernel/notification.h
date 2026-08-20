/**
 * @file notification.h
 * @brief P1-1 (A4): 独立 notification 对象 — seL4 风格单字聚合通知
 *
 * 与 event 标志组的区别(M4/P2-2/P2-3 的前置):
 *   - word 是"聚合徽章":signal 只做 `word |= badge`,badge 通常来自
 *     信号方 capability 的徽章(sys_cap_mint 铸入),标识"谁发的";
 *   - wait/poll 消费整个 word(原子取清),无按位匹配/AND/OR 语义;
 *   - word 非零时一次唤醒至多一个等待者(整字交给它),其余等待者
 *     留给后续 signal —— 与 seL4 notification 一致;
 *   - signal 可从 ISR/临界区调用(irq_spinlock 保护,同 event_set)。
 *
 * 内核生产者(timer/irq/BH,P2-2/P2-3)用 notification_signal() 直呼;
 * 用户任务经 syscall 家族:create 拿全权 cap,mint 出带徽章的 signal
 * cap(CAP_WRITE),wait/poll 用 CAP_READ cap。
 *
 * ISR-safety contract (同 event.h):
 *   - notification_signal()   ISR-safe
 *   - notification_poll()     ISR-safe(不阻塞;out 指针须内核可达)
 *   - notification_wait_syscall() 绝不可在 ISR 调用(会阻塞)
 */

#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include "kernel_types.h"

/*============================================================================
 * 对象池与生命周期
 *============================================================================*/

void notification_init(void);

/**
 * @brief 创建 notification 对象 (word 初始为 0)
 * @return notification_id_t >= 0, 或 KERN_INVALID_ID
 */
notification_id_t notification_create(void);

/**
 * @brief 删除对象;唤醒所有等待者(结果 KERN_ERR_NOEXIST)并撤销全部 cap
 */
kern_err_t notification_delete(notification_id_t id);

/*============================================================================
 * 信号与等待
 *============================================================================*/

/**
 * @brief 聚合信号:word |= badge;word 非零时唤醒队头等待者并移交整字
 * @param id    对象 id
 * @param badge 通常取 signal cap 的徽章(内核生产者直呼时自定)
 * @return KERN_OK / KERN_ERR_PARAM(对象不存在)
 */
kern_err_t notification_signal(notification_id_t id, uint32_t badge);

/** @brief 同上,按对象指针(timer/irq 内核侧持有 obj 时免 id 换算) */
kern_err_t notification_signal_obj(void *obj, uint32_t badge);

/**
 * @brief 非阻塞取走整个 word(消费型 poll;word 为 0 时返回 0)
 * @param out_word 输出取到的 word(可为 NULL,仅消费)
 * @return KERN_OK / KERN_ERR_PARAM
 */
kern_err_t notification_poll(notification_id_t id, uint32_t *out_word);

/**
 * @brief 阻塞等待(内核任务线程式,手动阻塞协议;SVC 上下文勿用)
 *
 * fast path: word 非零 → 取清整字,*out_word 返回。否则锁内手动
 * 出队+置 BLOCKED(消除解锁窗口竞态),唤醒后从 cont.u.ntfn.word
 * 取移交字。内核服务任务(timer/irq/BH)与 K 白盒测试用。
 *
 * @param timeout ticks, 0 = 不等待(空字即 TIMEOUT)
 * @return KERN_OK(取到字) / KERN_ERR_TIMEOUT / KERN_ERR_NOEXIST(删除)
 */
kern_err_t notification_wait(notification_id_t id, uint32_t timeout,
                             uint32_t *out_word);

/**
 * @brief 阻塞等待(两阶段 continuation 协议,SVC 上下文专用)
 *
 * fast path: word 非零 → 取清整字,经 copy_to_user 写 *user_word_out,
 * 返回 KERN_OK。否则阻塞,由 signal 方在唤醒前完成 copy。
 *
 * @param timeout     ticks, 0 = 永久等待
 * @param user_word_out 用户可写 4 字节输出指针(阻塞前校验)
 * @return KERN_OK(取到字) / KERN_ERR_TIMEOUT / KERN_ERR_NOEXIST(删除)
 */
kern_err_t notification_wait_syscall(notification_id_t id, uint32_t timeout,
                                     void *user_word_out);

/*============================================================================
 * cap 集成 (M2-Step3a 模式,同 event)
 *============================================================================*/

notification_id_t notification_id_from_obj(void *obj);
void *notification_obj_for_cap(notification_id_t id);

/** task_unlink_blocked 的 NOTIFICATION 分支:从等待队列安全摘除 */
void notification_cleanup_task(void *ntfn_obj, tcb_t *tcb);

#endif /* NOTIFICATION_H */

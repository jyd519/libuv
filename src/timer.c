/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "uv-common.h"
#include "heap-inl.h"

#include <assert.h>
#include <limits.h>

/*
 * 维护timer heap的有序：timeout小的在堆的顶部，如果timeout一样，则比较start_id
 * */
static int timer_less_than(const struct heap_node* ha,
                           const struct heap_node* hb) {
  const uv_timer_t* a;
  const uv_timer_t* b;

  a = container_of(ha, uv_timer_t, heap_node);
  b = container_of(hb, uv_timer_t, heap_node);

  if (a->timeout < b->timeout)
    return 1;
  if (b->timeout < a->timeout)
    return 0;

  /* Compare start_id when both have the same timeout. start_id is
   * allocated with loop->timer_counter in uv_timer_start().
   */
  return a->start_id < b->start_id;
}


// 初始timer对象
int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_TIMER);
  handle->timer_cb = NULL;
  handle->repeat = 0;  // 不重复
  return 0;
}

// 激活timer
int uv_timer_start(uv_timer_t* handle,
                   uv_timer_cb cb,   // 回调函数
                   uint64_t timeout,  // 间隔
                   uint64_t repeat) {  // 是否重复
  uint64_t clamped_timeout;

  // 已经关闭？
  if (uv__is_closing(handle) || cb == NULL)
    return UV_EINVAL;

  // 已经激活先停止
  if (uv__is_active(handle))
    uv_timer_stop(handle);

  // 溢出了？设成最大值
  clamped_timeout = handle->loop->time + timeout;
  if (clamped_timeout < timeout)
    clamped_timeout = (uint64_t) -1;

  handle->timer_cb = cb;
  handle->timeout = clamped_timeout;
  handle->repeat = repeat;
  /* start_id is the second index to be compared in timer_less_than() */
  handle->start_id = handle->loop->timer_counter++;

  // 插入heap
  heap_insert((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  // 成为活动handle
  uv__handle_start(handle);

  return 0;
}


// 停止timer
int uv_timer_stop(uv_timer_t* handle) {
  if (!uv__is_active(handle))
    return 0;

  // 从heap中移除
  heap_remove((struct heap*) &handle->loop->timer_heap,
              (struct heap_node*) &handle->heap_node,
              timer_less_than);

  // 从活动handle队列中移除
  uv__handle_stop(handle);

  return 0;
}

// 重启timer
int uv_timer_again(uv_timer_t* handle) {
  if (handle->timer_cb == NULL || handle->repeat == 0)
    return UV_EINVAL;

  uv_timer_start(handle, handle->timer_cb, handle->repeat, handle->repeat);

  return 0;
}


void uv_timer_set_repeat(uv_timer_t* handle, uint64_t repeat) {
  handle->repeat = repeat;
}


uint64_t uv_timer_get_repeat(const uv_timer_t* handle) {
  return handle->repeat;
}

// 计算距离下一个到点的时间
//   0 表示存在已经到点的timer
//   -1 没有timer
//   >0 下一个timer到点时间 
int uv__next_timeout(const uv_loop_t* loop) {
  const struct heap_node* heap_node;
  const uv_timer_t* handle;
  uint64_t diff;

  heap_node = heap_min((const struct heap*) &loop->timer_heap);
  if (heap_node == NULL)
    return -1; /* block indefinitely */

  handle = container_of(heap_node, uv_timer_t, heap_node);
  if (handle->timeout <= loop->time)
    return 0;

  diff = handle->timeout - loop->time;
  if (diff > INT_MAX) // 太大 
    diff = INT_MAX;

  return (int) diff;
}

// 运行到点的timer (在loop中运行）
void uv__run_timers(uv_loop_t* loop) {
  struct heap_node* heap_node;
  uv_timer_t* handle;

  for (;;) {
    // 从heap中取最小timeout的timer
    heap_node = heap_min((struct heap*) &loop->timer_heap);
    if (heap_node == NULL) // 无timer
      break;

    handle = container_of(heap_node, uv_timer_t, heap_node);
    if (handle->timeout > loop->time)  // 未到点
      break;

    uv_timer_stop(handle);  // 停止timer
    uv_timer_again(handle);  // 尝试schedule下一次定时
    handle->timer_cb(handle);  // 调用回调函数
  }
}


//适配器: uv_close的实现各个对象的close函数名称有要求
void uv__timer_close(uv_timer_t* handle) {
  uv_timer_stop(handle);
}

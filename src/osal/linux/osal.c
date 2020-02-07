/*********************************************************************
 *        _       _         _
 *  _ __ | |_  _ | |  __ _ | |__   ___
 * | '__|| __|(_)| | / _` || '_ \ / __|
 * | |   | |_  _ | || (_| || |_) |\__ \
 * |_|    \__|(_)|_| \__,_||_.__/ |___/
 *
 * www.rt-labs.com
 * Copyright 2017 rt-labs AB, Sweden.
 *
 * This software is dual-licensed under GPLv3 and a commercial
 * license. See the file LICENSE.md distributed with this software for
 * full license information.
 ********************************************************************/

#define _GNU_SOURCE

#include <osal.h>
#include <options.h>
#include <log.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>

#include <pthread.h>

#include <assert.h>
#include <errno.h>
#include <unistd.h>

/* Priority of timer callback thread (if USE_SCHED_FIFO is set) */
#define TIMER_PRIO        30

#define USECS_PER_SEC     (1 * 1000 * 1000)
#define NSECS_PER_SEC     (1 * 1000 * 1000 * 1000)

void os_log (uint8_t type, const char * fmt, ...)
{
   va_list list;

   switch(LOG_LEVEL_GET (type))
   {
   case LOG_LEVEL_DEBUG:   printf ("[DEBUG] "); break;
   case LOG_LEVEL_INFO:    printf ("[INFO ] "); break;
   case LOG_LEVEL_WARNING: printf ("[WARN ] "); break;
   case LOG_LEVEL_ERROR:   printf ("[ERROR] "); break;
   default: break;
   }

   va_start (list, fmt);
   vprintf (fmt, list);
   va_end (list);
   fflush (stdout);
}

void * os_malloc (size_t size)
{
   return malloc (size);
}

os_thread_t * os_thread_create (const char * name, uint32_t priority,
        size_t stacksize, void (*entry) (void * arg), void * arg)
{
   int result;
   pthread_t * thread = malloc (sizeof(*thread));
   pthread_attr_t attr;

   pthread_attr_init (&attr);

#if defined (USE_SCHED_FIFO)
   CC_STATIC_ASSERT (_POSIX_THREAD_PRIORITY_SCHEDULING > 0);
   struct sched_param param = { .sched_priority = priority };
   pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy (&attr, SCHED_FIFO);
   pthread_attr_setschedparam (&attr, &param);
#endif

   /* Note that the default stacksize is used */

   result = pthread_create (thread, &attr, (void *)entry, arg);
   if (result != 0)
      return NULL;

   pthread_setname_np (*thread, name);
   return thread;
}

os_mutex_t * os_mutex_create (void)
{
   int result;
   pthread_mutex_t * mutex = malloc (sizeof(*mutex));
   pthread_mutexattr_t attr;

   CC_STATIC_ASSERT (_POSIX_THREAD_PRIO_INHERIT > 0);
   pthread_mutexattr_init (&attr);
   pthread_mutexattr_setprotocol (&attr, PTHREAD_PRIO_INHERIT);

   result = pthread_mutex_init (mutex, &attr);
   if (result != 0)
      return NULL;

   return mutex;
}

void os_mutex_lock (os_mutex_t * _mutex)
{
   pthread_mutex_t * mutex = _mutex;
   pthread_mutex_lock (mutex);
}

void os_mutex_unlock (os_mutex_t * _mutex)
{
   pthread_mutex_t * mutex = _mutex;
   pthread_mutex_unlock (mutex);
}

void os_mutex_destroy (os_mutex_t * _mutex)
{
   pthread_mutex_t * mutex = _mutex;
   pthread_mutex_destroy (mutex);
   free (mutex);
}

os_sem_t * os_sem_create (size_t count)
{
   os_sem_t * sem;

   sem = (os_sem_t *)malloc (sizeof(*sem));

   pthread_cond_init (&sem->cond, NULL);
   pthread_mutex_init (&sem->mutex, NULL);
   sem->count = count;

   return sem;
}

bool os_sem_wait (os_sem_t * sem, uint32_t time)
{
   struct timespec ts;
   int error = 0;
   uint64_t nsec = (uint64_t)time * 1000 * 1000;

   clock_gettime (CLOCK_MONOTONIC, &ts);
   nsec += ts.tv_nsec;
   if (nsec > NSECS_PER_SEC)
   {
      ts.tv_sec += nsec / NSECS_PER_SEC;
      nsec %= NSECS_PER_SEC;
   }
   ts.tv_nsec = nsec;

   pthread_mutex_lock (&sem->mutex);
   while (sem->count == 0)
   {
      if (time != OS_WAIT_FOREVER)
      {
         error = pthread_cond_timedwait (&sem->cond, &sem->mutex, &ts);
         assert (error != EINVAL);
         if (error)
         {
            goto timeout;
         }
      }
      else
      {
         error = pthread_cond_wait (&sem->cond, &sem->mutex);
         assert (error != EINVAL);
      }
   }

   sem->count--;

 timeout:
   pthread_mutex_unlock (&sem->mutex);
   return (error != 0);
}

void os_sem_signal (os_sem_t * sem)
{
   pthread_mutex_lock (&sem->mutex);
   sem->count++;
   pthread_mutex_unlock (&sem->mutex);
   pthread_cond_signal (&sem->cond);
}

void os_sem_destroy (os_sem_t * sem)
{
   pthread_cond_destroy (&sem->cond);
   pthread_mutex_destroy (&sem->mutex);
   free (sem);
}

void os_usleep (uint32_t usec)
{
   struct timespec ts;
   struct timespec remain;
   ts.tv_sec = usec / USECS_PER_SEC;
   ts.tv_nsec = (usec % USECS_PER_SEC) * 1000;
   while (nanosleep (&ts, &remain) == -1)
   {
      ts = remain;
   }
}

uint32_t os_get_current_time_us (void)
{
   struct timespec ts;

   clock_gettime (CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1000 * 1000 + ts.tv_nsec / 1000;
}

os_event_t * os_event_create (void)
{
   os_event_t * event;

   event = (os_event_t *)malloc (sizeof(*event));

   pthread_cond_init (&event->cond, NULL);
   pthread_mutex_init (&event->mutex, NULL);
   event->flags = 0;

   return event;
}

bool os_event_wait (os_event_t * event, uint32_t mask, uint32_t * value, uint32_t time)
{
   struct timespec ts;
   int error = 0;
   uint64_t nsec = (uint64_t)time * 1000 * 1000;

   if (time != OS_WAIT_FOREVER)
   {
      clock_gettime (CLOCK_MONOTONIC, &ts);
      nsec += ts.tv_nsec;

      ts.tv_sec += nsec / NSECS_PER_SEC;
      ts.tv_nsec = nsec % NSECS_PER_SEC;
   }

   pthread_mutex_lock (&event->mutex);

   while ((event->flags & mask) == 0)
   {
      if (time != OS_WAIT_FOREVER)
      {
         error = pthread_cond_timedwait (&event->cond, &event->mutex, &ts);
         assert (error != EINVAL);
         if (error)
         {
            goto timeout;
         }
      }
      else
      {
         error = pthread_cond_wait (&event->cond, &event->mutex);
         assert (error != EINVAL);
      }
   }

 timeout:
   *value = event->flags & mask;
   pthread_mutex_unlock (&event->mutex);
   return (error != 0);
}

void os_event_set (os_event_t * event, uint32_t value)
{
   pthread_mutex_lock (&event->mutex);
   event->flags |= value;
   pthread_mutex_unlock (&event->mutex);
   pthread_cond_signal (&event->cond);
}

void os_event_clr (os_event_t * event, uint32_t value)
{
   pthread_mutex_lock (&event->mutex);
   event->flags &= ~value;
   pthread_mutex_unlock (&event->mutex);
   pthread_cond_signal (&event->cond);
}

void os_event_destroy (os_event_t * event)
{
   pthread_cond_destroy (&event->cond);
   pthread_mutex_destroy (&event->mutex);
   free (event);
}

os_mbox_t * os_mbox_create (size_t size)
{
   os_mbox_t * mbox;

   mbox = (os_mbox_t *)malloc (sizeof(*mbox) + size * sizeof(void *));

   pthread_cond_init (&mbox->cond, NULL);
   pthread_mutex_init (&mbox->mutex, NULL);

   mbox->r     = 0;
   mbox->w     = 0;
   mbox->count = 0;
   mbox->size  = size;

   return mbox;
}

bool os_mbox_fetch (os_mbox_t * mbox, void ** msg, uint32_t time)
{
   struct timespec ts;
   int error = 0;
   uint64_t nsec = (uint64_t)time * 1000 * 1000;

   if (time != OS_WAIT_FOREVER)
   {
      clock_gettime (CLOCK_MONOTONIC, &ts);
      nsec += ts.tv_nsec;

      ts.tv_sec += nsec / NSECS_PER_SEC;
      ts.tv_nsec = nsec % NSECS_PER_SEC;
   }

   pthread_mutex_lock (&mbox->mutex);

   while (mbox->count == 0)
   {
      if (time != OS_WAIT_FOREVER)
      {
         error = pthread_cond_timedwait (&mbox->cond, &mbox->mutex, &ts);
         assert (error != EINVAL);
         if (error)
         {
            goto timeout;
         }
      }
      else
      {
         error = pthread_cond_wait (&mbox->cond, &mbox->mutex);
         assert (error != EINVAL);
      }
   }

   *msg = mbox->msg[mbox->r++];
   if (mbox->r == mbox->size)
      mbox->r = 0;

   mbox->count--;

 timeout:
   pthread_mutex_unlock (&mbox->mutex);
   pthread_cond_signal (&mbox->cond);

   return (error != 0);
}

bool os_mbox_post (os_mbox_t * mbox, void * msg, uint32_t time)
{
   struct timespec ts;
   int error = 0;
   uint64_t nsec = (uint64_t)time * 1000 * 1000;

   if (time != OS_WAIT_FOREVER)
   {
      clock_gettime (CLOCK_MONOTONIC, &ts);
      nsec += ts.tv_nsec;

      ts.tv_sec += nsec / NSECS_PER_SEC;
      ts.tv_nsec = nsec % NSECS_PER_SEC;
   }

   pthread_mutex_lock (&mbox->mutex);

   while (mbox->count == mbox->size)
   {
      if (time != OS_WAIT_FOREVER)
      {
         error = pthread_cond_timedwait (&mbox->cond, &mbox->mutex, &ts);
         assert (error != EINVAL);
         if (error)
         {
            goto timeout;
         }
      }
      else
      {
         error = pthread_cond_wait (&mbox->cond, &mbox->mutex);
         assert (error != EINVAL);
      }
   }

   mbox->msg[mbox->w++] = msg;
   if (mbox->w == mbox->size)
      mbox->w = 0;

   mbox->count++;

 timeout:
   pthread_mutex_unlock (&mbox->mutex);
   pthread_cond_signal (&mbox->cond);

   return (error != 0);
}

void os_mbox_destroy (os_mbox_t * mbox)
{
   pthread_cond_destroy (&mbox->cond);
   pthread_mutex_destroy (&mbox->mutex);
   free (mbox);
}

static void os_timer_callback (union sigval sv)
{
   os_timer_t * timer = sv.sival_ptr;

   if (timer->fn)
      timer->fn (timer, timer->arg);
}

os_timer_t * os_timer_create (uint32_t us, void (*fn) (os_timer_t *, void * arg),
                              void * arg, bool oneshot)
{
   os_timer_t * timer;
   struct sigevent sev;
   pthread_attr_t attr;

   timer = (os_timer_t *)malloc (sizeof(*timer));

   timer->fn = fn;
   timer->arg = arg;
   timer->us = us;
   timer->oneshot = oneshot;

   pthread_attr_init (&attr);

#if defined (USE_SCHED_FIFO)
   struct sched_param param = { .sched_priority = TIMER_PRIO };
   pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
   pthread_attr_setschedpolicy (&attr, SCHED_FIFO);
   pthread_attr_setschedparam (&attr, &param);
#endif

   /* Create timer */
   sev.sigev_notify = SIGEV_THREAD;
   sev.sigev_value.sival_ptr = timer;
   sev.sigev_notify_function = os_timer_callback;
   sev.sigev_notify_attributes = &attr;

   if (timer_create (CLOCK_MONOTONIC, &sev, &timer->timerid) == -1)
      return NULL;

   return timer;
}

void os_timer_set (os_timer_t * timer, uint32_t us)
{
   timer->us = us;
}

void os_timer_start (os_timer_t * timer)
{
   struct itimerspec its;

   /* Start timer */
   its.it_value.tv_sec = 0;
   its.it_value.tv_nsec = 1000 * timer->us;
   its.it_interval.tv_sec = (timer->oneshot) ? 0 : its.it_value.tv_sec;
   its.it_interval.tv_nsec = (timer->oneshot) ? 0 : its.it_value.tv_nsec;
   timer_settime (timer->timerid, 0, &its, NULL);
}

void os_timer_stop (os_timer_t * timer)
{
   struct itimerspec its;

   /* Stop timer */
   its.it_value.tv_sec = 0;
   its.it_value.tv_nsec = 0;
   its.it_interval.tv_sec = 0;
   its.it_interval.tv_nsec = 0;
   timer_settime (timer->timerid, 0, &its, NULL);
}

void os_timer_destroy (os_timer_t * timer)
{
   timer_delete (timer->timerid);
   free (timer);
}
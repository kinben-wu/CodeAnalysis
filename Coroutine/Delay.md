### 主函数

0、程序入口

```kotlin
fun main() {
    GlobalScope.launch {
        println(Thread.currentThread())
        delay(1000)
        println(Thread.currentThread())
    }
}
```

### Builders.common.kt

1、CoroutineScope.launch

```kotlin
public fun CoroutineScope.launch(
    context: CoroutineContext = EmptyCoroutineContext,
    start: CoroutineStart = CoroutineStart.DEFAULT,
    block: suspend CoroutineScope.() -> Unit
): Job {
    val newContext = newCoroutineContext(context)
    val coroutine = if (start.isLazy)
        LazyStandaloneCoroutine(newContext, block) else
        StandaloneCoroutine(newContext, active = true)
    coroutine.start(start, coroutine, block)
    return coroutine
}
```

### CoroutineContext.kt

2、newCoroutineContext

```kotlin
public actual fun CoroutineScope.newCoroutineContext(context: CoroutineContext): CoroutineContext {
    val combined = foldCopies(coroutineContext, context, true)
    val debug = if (DEBUG) combined + CoroutineId(COROUTINE_ID.incrementAndGet()) else combined
    return if (combined !== Dispatchers.Default && combined[ContinuationInterceptor] == null)
        debug + Dispatchers.Default else debug
}
```

因为传入的context是EmptyCoroutineContext，所以combined还是EmptyCoroutineContext。 DEBUG为false，所以debug =
combined，后面的逻辑是，如果目前的context不等于Dispatchers.Default且没有拦截器的时候，会给他补上一个默认的拦截器（分发器）Dispatchers.Default。
然后，创建一个Coroutine实例，因为默认的启动模式是CoroutineStart.DEFAULT，所以创建的Coroutine实例为StandaloneCoroutine。传入的context为EmptyCoroutineContext+Dispatchers.Default=Dispatchers.Default。
最后，启动Coroutine。

### AbstractCoroutine.kt

3、coroutine.start

```kotlin
public fun <R> start(start: CoroutineStart, receiver: R, block: suspend R.() -> T) {
    start(block, receiver, this)
}
```

这里方法体里面的start，不是表示本方法名的start，而是方法参数的start，这里特性是kotlin的语法糖，实际会调用start的类CoroutineStart的invoke方法。通过编译后的代码可以看出来。

### CoroutineStart.kt

4、CoroutineStart.invoke

```kotlin
@InternalCoroutinesApi
public operator fun <R, T> invoke(block: suspend R.() -> T, receiver: R, completion: Continuation<T>): Unit =
    when (this) {
        DEFAULT -> block.startCoroutineCancellable(receiver, completion)
        ATOMIC -> block.startCoroutine(receiver, completion)
        UNDISPATCHED -> block.startCoroutineUndispatched(receiver, completion)
        LAZY -> Unit // will start lazily
    }
```

目前CoroutineStart是CoroutineStart.DEFAULT，所以走block.startCoroutineCancellable(receiver, completion)分支。

### Cancellable.kt

5、startCoroutineCancellable

```kotlin
internal fun <R, T> (suspend (R) -> T).startCoroutineCancellable(
    receiver: R, completion: Continuation<T>,
    onCancellation: ((cause: Throwable) -> Unit)? = null
) =
    runSafely(completion) {
        createCoroutineUnintercepted(receiver, completion).intercepted()
            .resumeCancellableWith(Result.success(Unit), onCancellation)
    }

private inline fun runSafely(completion: Continuation<*>, block: () -> Unit) {
    try {
        block()
    } catch (e: Throwable) {
        dispatcherFailure(completion, e)
    }
}
```

runSafely实际是包了一层try-catch块，所以执行createCoroutineUnintercepted(receiver, completion).intercepted().resumeCancellableWith(
Result.success(Unit), onCancellation)。 这个方法在Coroutine.md已经分析过了，最终会通过resumeCancellableWith执行进协程体。
与之前有不同的地方在于之前的第7步中，context[ContinuationInterceptor]是空，但现在因为第2步的时候，如果context[ContinuationInterceptor]
为空是会加上Dispatchers.Default。 所以现在context[ContinuationInterceptor]=Dispatchers.Default。
那么后续的interceptContinuation方法会将Dispatchers.Default，包成DispatchedContinuation(Dispatchers.Default, continuation)
在resumeCancellableWith方法中，Dispatchers.Default的isDispatchNeeded(context)为true，所以整个协程体会被分发到Dispatchers.Default所代表的线程执行。

### Dispatchers.kt

```kotlin
@JvmStatic
public actual val Default: CoroutineDispatcher = DefaultScheduler
```

### Dispatcher.kt

```kotlin
internal object DefaultScheduler : SchedulerCoroutineDispatcher(
    CORE_POOL_SIZE, MAX_POOL_SIZE,
    IDLE_WORKER_KEEP_ALIVE_NS, DEFAULT_SCHEDULER_NAME
)
internal open class SchedulerCoroutineDispatcher(
    private val corePoolSize: Int = CORE_POOL_SIZE,
    private val maxPoolSize: Int = MAX_POOL_SIZE,
    private val idleWorkerKeepAliveNs: Long = IDLE_WORKER_KEEP_ALIVE_NS,
    private val schedulerName: String = "CoroutineScheduler",
) : ExecutorCoroutineDispatcher() {

    override val executor: Executor
        get() = coroutineScheduler

    // This is variable for test purposes, so that we can reinitialize from clean state
    private var coroutineScheduler = createScheduler()

    private fun createScheduler() =
        CoroutineScheduler(corePoolSize, maxPoolSize, idleWorkerKeepAliveNs, schedulerName)

    override fun dispatch(context: CoroutineContext, block: Runnable): Unit = coroutineScheduler.dispatch(block)

    override fun dispatchYield(context: CoroutineContext, block: Runnable): Unit =
        coroutineScheduler.dispatch(block, tailDispatch = true)

    //... 
}
```

### Delay.kt

6、delay

```kotlin
public suspend fun delay(timeMillis: Long) {
    if (timeMillis <= 0) return // don't delay
    return suspendCancellableCoroutine sc@{ cont: CancellableContinuation<Unit> ->
        // if timeMillis == Long.MAX_VALUE then just wait forever like awaitCancellation, don't schedule.
        if (timeMillis < Long.MAX_VALUE) {
            cont.context.delay.scheduleResumeAfterDelay(timeMillis, cont)
        }
    }
}
```

```kotlin
public suspend inline fun <T> suspendCancellableCoroutine(
    crossinline block: (CancellableContinuation<T>) -> Unit
): T =
    suspendCoroutineUninterceptedOrReturn { uCont ->
        val cancellable = CancellableContinuationImpl(uCont.intercepted(), resumeMode = MODE_CANCELLABLE)
        /*
         * For non-atomic cancellation we setup parent-child relationship immediately
         * in case when `block` blocks the current thread (e.g. Rx2 with trampoline scheduler), but
         * properly supports cancellation.
         */
        cancellable.initCancellability()
        block(cancellable)
        cancellable.getResult()
    }
```

suspendCancellableCoroutine是个内联函数，于是展开即为

```kotlin
public suspend fun delay(timeMillis: Long) {
    if (timeMillis <= 0) return // don't delay
    return suspendCoroutineUninterceptedOrReturn { uCont ->
        val cancellable = CancellableContinuationImpl(uCont.intercepted(), resumeMode = MODE_CANCELLABLE)
        /*
         * For non-atomic cancellation we setup parent-child relationship immediately
         * in case when `block` blocks the current thread (e.g. Rx2 with trampoline scheduler), but
         * properly supports cancellation.
         */
        cancellable.initCancellability()
        sc@{
            // if timeMillis == Long.MAX_VALUE then just wait forever like awaitCancellation, don't schedule.
            if (timeMillis < Long.MAX_VALUE) {
                cancellable.context.delay.scheduleResumeAfterDelay(timeMillis, cont)
            }
        }
        cancellable.getResult()
    }
}
```

之前通过编译看过，suspendCoroutineUninterceptedOrReturn的实现是执行传入的block方法，uCont为外层协程体对象。

### CancellableContinuationImpl.kt

7、CancellableContinuationImpl(uCont.intercepted(), resumeMode = MODE_CANCELLABLE)

```kotlin
@PublishedApi
internal open class CancellableContinuationImpl<in T>(
    final override val delegate: Continuation<T>,
    resumeMode: Int
) : DispatchedTask<T>(resumeMode), CancellableContinuation<T>, CoroutineStackFrame {
    public override val context: CoroutineContext = delegate.context
}
```

成员delegate=uCont.intercepted()。uCont是外层的协程体，由第2步对uCont的上下文分析得知，uCont.intercepted()=DispatchedContinuation(Dispatchers.Default,
continuation)， 其中continuation就是参数uCont，即第1步创建的StandaloneCoroutine。

### Delay.kt

8、cancellable.context.delay.scheduleResumeAfterDelay(timeMillis, cont)

```kotlin
internal val CoroutineContext.delay: Delay get() = get(ContinuationInterceptor) as? Delay ?: DefaultDelay
```

其中context.delay的定义就是当前context的拦截器，前面知道，cancellable.context是代理对象delegate.context，delegate是DispatchedContinuation(
Dispatchers.Default, continuation)。 从代码上看，DispatchedContinuation类实现了Continuation接口/，但是没有实现context成员变量重写。这是？
原来，DispatchedContinuation类定义的使用使用了类委托模式Continuation<T> by continuation，意思就是接口Continuation的具体实现委托了continuation成员。
continuation同样是实现了Continuation接口，所以DispatchedContinuation.context=continuation.context=StandaloneCoroutine.context。
从第2步中知道，StandaloneCoroutine.context=Dispatchers.Default。但Dispatchers.Default并没有继承Delay，所以综上，
cancellable.context.delay=DefaultDelay。

### DefaultExecutor.kt

```kotlin
internal actual val DefaultDelay: Delay = initializeDefaultDelay()

private fun initializeDefaultDelay(): Delay {
    // Opt-out flag
    if (!defaultMainDelayOptIn) return DefaultExecutor
    val main = Dispatchers.Main
    /*
     * When we already are working with UI and Main threads, it makes
     * no sense to create a separate thread with timer that cannot be controller
     * by the UI runtime.
     */
    return if (main.isMissing() || main !is Delay) DefaultExecutor else main
}
```

所以，DefaultDelay=DefaultExecutor，而DefaultExecutor继承EventLoopImplBase。

### EventLoop.common.kt

9、scheduleResumeAfterDelay

```kotlin
public override fun scheduleResumeAfterDelay(timeMillis: Long, continuation: CancellableContinuation<Unit>) {
    val timeNanos = delayToNanos(timeMillis)
    if (timeNanos < MAX_DELAY_NS) {
        val now = nanoTime()
        DelayedResumeTask(now + timeNanos, continuation).also { task ->
            /*
             * Order is important here: first we schedule the heap and only then
             * publish it to continuation. Otherwise, `DelayedResumeTask` would
             * have to know how to be disposed of even when it wasn't scheduled yet.
             */
            schedule(now, task)
            continuation.disposeOnCancellation(task)
        }
    }
}
```

先把延时的时间转成nano，然后创建一个DelayedResumeTask，在now + timeNanos的时间执行。

```kotlin
private inner class DelayedResumeTask(
    nanoTime: Long,
    private val cont: CancellableContinuation<Unit>
) : DelayedTask(nanoTime) {
    override fun run() {
        with(cont) { resumeUndispatched(Unit) }
    }
    override fun toString(): String = super.toString() + cont.toString()
}

internal abstract class DelayedTask(
    @JvmField var nanoTime: Long
) : Runnable, Comparable<DelayedTask>, DisposableHandle, ThreadSafeHeapNode
```

10、DelayedResumeTask创建以后，接着执行schedule(now, task)

```kotlin
public fun schedule(now: Long, delayedTask: DelayedTask) {
    when (scheduleImpl(now, delayedTask)) {
        SCHEDULE_OK -> if (shouldUnpark(delayedTask)) unpark()
        SCHEDULE_COMPLETED -> reschedule(now, delayedTask)
        SCHEDULE_DISPOSED -> {} // do nothing -- task was already disposed
        else -> error("unexpected result")
    }
}
```

11、scheduleImpl

```kotlin
private fun scheduleImpl(now: Long, delayedTask: DelayedTask): Int {
    if (isCompleted) return SCHEDULE_COMPLETED
    val delayedQueue = _delayed.value ?: run {
        _delayed.compareAndSet(null, DelayedTaskQueue(now))
        _delayed.value!!
    }
    return delayedTask.scheduleTask(now, delayedQueue, this)
}

internal class DelayedTaskQueue(
    @JvmField var timeNow: Long
) : ThreadSafeHeap<DelayedTask>()
```

isCompleted是EventLoopImplBase的成员变量，loop未结束为false，结束以后为true。这里未结束，所以为false。
_delayed.value的初始值也为空，所以会创建一个DelayedTaskQueue实例，并赋值给_delayed.value. 

12、delayedTask.scheduleTask

```kotlin
@Synchronized
fun scheduleTask(now: Long, delayed: DelayedTaskQueue, eventLoop: EventLoopImplBase): Int {
    if (_heap === DISPOSED_TASK) return SCHEDULE_DISPOSED // don't add -- was already disposed
    delayed.addLastIf(this) { firstTask ->
        //...
    }
    return SCHEDULE_OK
}
```

这里把创建的delayedTask（DelayedResumeTask）添加到DelayedTaskQueue队列里，然后返回SCHEDULE_OK。
然后回到schedule方法，接着执行SCHEDULE_OK分支`if (shouldUnpark(delayedTask)) unpark()`。

```kotlin
private fun shouldUnpark(task: DelayedTask): Boolean = _delayed.value?.peek() === task
```

在`scheduleTask`方法中把DelayedTask添加到DelayedTaskQueue队列里面，所以从_delayed里peek出来的task就是当前的task。所以shouldUnpark返回true。

### EventLoop.kt

13、unpark

```kotlin
internal actual abstract class EventLoopImplPlatform : EventLoop() {
    protected abstract val thread: Thread

    protected actual fun unpark() {
        val thread = thread // atomic read
        if (Thread.currentThread() !== thread)
            unpark(thread)
    }

    //...
}
```

unpark方法首先获取线程thread，这个获取方法是在子类实现。

```kotlin
@Suppress("PLATFORM_CLASS_MAPPED_TO_KOTLIN")
internal actual object DefaultExecutor : EventLoopImplBase(), Runnable {
    const val THREAD_NAME = "kotlinx.coroutines.DefaultExecutor"

    //...

    @Suppress("ObjectPropertyName")
    @Volatile
    private var _thread: Thread? = null

    override val thread: Thread
        get() = _thread ?: createThreadSync()

    //...
}
```

可以看到thread的初始值为空，而为空的时候会调用`createThreadSync`创建线程。 

14、createThreadSync

```kotlin
@Synchronized
private fun createThreadSync(): Thread {
    return _thread ?: Thread(this, THREAD_NAME).apply {
        _thread = this
        isDaemon = true
        start()
    }
}
```

创建线程，并且将线程启动，线程名为kotlinx.coroutines.DefaultExecutor。 回到方法，接下来会判断当前线程是否是刚新建的线程，很明显不是，所以调用unpark(thread)。

### AbstractTimeSource.kt

15、unpark

```kotlin
@InlineOnly
internal inline fun unpark(thread: Thread) {
    timeSource?.unpark(thread) ?: LockSupport.unpark(thread)
}
```

最终调用`LockSupport.unpark(thread)`来唤醒线程。 该方法调用完以后会一路返回，返回到第9步的scheduleResumeAfterDelay。
接下来执行的是continuation.disposeOnCancellation(task)，这个方法暂时不看。
执行完这个方法以后，delay算是执行完一半了，总的来说上半部分就是新建一个DelayedResumeTask任务，然后添加到DelayedTaskQueue队列里面。
同时，新建一个名为kotlinx.coroutines.DefaultExecutor的线程，并启动线程。
此时`unpark`方法执行完，会一路返回（DelayedResumeTask的`continuation.disposeOnCancellation(task)`方法先忽略）
LockSupport.unpark->EventLoopImplPlatform.unpark->DelayedResumeTask.schedule->cancellable.context.delay.scheduleResumeAfterDelay
然后接下来会执行`cancellable.getResult()`。

###CancellableContinuationImpl.kt
16、cancellable.getResult()
```kotlin
@PublishedApi
internal fun getResult(): Any? {
    val isReusable = isReusable()
    // trySuspend may fail either if 'block' has resumed/cancelled a continuation
    // or we got async cancellation from parent.
    if (trySuspend()) {
        //...
        return COROUTINE_SUSPENDED
    }
    //...
}
```
该方法会调用`trySuspend`尝试挂起当前协程，trySuspend会通过cas设置变量_decision为挂起状态SUSPENDED，成功则返回true。
然后getResult就会返回COROUTINE_SUSPENDED，然后一路返回
getResult-》suspendCoroutineUninterceptedOrReturn-》delay-》invokeSuspend-》resumeWith-》-resumeCancellableWith-》startCoroutineCancellable-》start-》launch
此时，协程在调用线程就被挂起，调用线程会接着往下执行。

后半部分主要看线程kotlinx.coroutines.DefaultExecutor具体执行的逻辑。先从线程的target看起，从13步知道，线程创建时传入的target是DefaultExecutor实例。

### DefaultExecutor.kt

16、run

```kotlin
override fun run() {
    ThreadLocalEventLoop.setEventLoop(this)
    registerTimeLoopThread()
    try {
        var shutdownNanos = Long.MAX_VALUE
        if (!notifyStartup()) return
        while (true) {
            Thread.interrupted() // just reset interruption flag
            var parkNanos = processNextEvent()
            if (parkNanos == Long.MAX_VALUE) {
                // nothing to do, initialize shutdown timeout
                val now = nanoTime()
                if (shutdownNanos == Long.MAX_VALUE) shutdownNanos = now + KEEP_ALIVE_NANOS
                val tillShutdown = shutdownNanos - now
                if (tillShutdown <= 0) return // shut thread down
                parkNanos = parkNanos.coerceAtMost(tillShutdown)
            } else
                shutdownNanos = Long.MAX_VALUE
            if (parkNanos > 0) {
                // check if shutdown was requested and bail out in this case
                if (isShutdownRequested) return
                parkNanos(this, parkNanos)
            }
        }
    } finally {
        _thread = null // this thread is dead
        acknowledgeShutdownIfNeeded()
        unregisterTimeLoopThread()
        // recheck if queues are empty after _thread reference was set to null (!!!)
        if (!isEmpty) thread // recreate thread if it is needed
    }
}
```

进入循环，先重置线程的interruption标记。 然后通过`processNextEvent`方法获取需要等待的时间。

### EventLoop.common.kt

17、processNextEvent

```kotlin
override fun processNextEvent(): Long {
    // unconfined events take priority
    if (processUnconfinedEvent()) return 0
    // queue all delayed tasks that are due to be executed
    val delayed = _delayed.value
    if (delayed != null && !delayed.isEmpty) {
        val now = nanoTime()
        while (true) {
            // make sure that moving from delayed to queue removes from delayed only after it is added to queue
            // to make sure that 'isEmpty' and `nextTime` that check both of them
            // do not transiently report that both delayed and queue are empty during move
            delayed.removeFirstIf {
                if (it.timeToExecute(now)) {
                    enqueueImpl(it)
                } else
                    false
            } ?: break // quit loop when nothing more to remove or enqueueImpl returns false on "isComplete"
        }
    }
    // then process one event from queue
    val task = dequeue()
    if (task != null) {
        platformAutoreleasePool { task.run() }
        return 0
    }
    return nextTime
}

public inline fun removeFirstIf(predicate: (T) -> Boolean): T? = synchronized(this) {
    val first = firstImpl() ?: return null
    if (predicate(first)) {
        removeAtImpl(0)
    } else {
        null
    }
}
```

先判断delayed队列是否为空，非空的话，从队列取出任务，再判断任务是否到时间执行，如果是则调用`enqueueImpl`将任务添加到_queue队列，添加成功会将任务从_delayed队列移除。
然后继续循环取下一个任务。如果添加失败或者没有任务了，则跳出循环。 然后调用`dequeue`方法从_queue队列里面取出来执行。

18、enqueueImpl

```kotlin
@Suppress("UNCHECKED_CAST")
private fun enqueueImpl(task: Runnable): Boolean {
    _queue.loop { queue ->
        if (isCompleted) return false // fail fast if already completed, may still add, but queues will close
        when (queue) {
            null -> if (_queue.compareAndSet(null, task)) return true
            is Queue<*> -> {
                when ((queue as Queue<Runnable>).addLast(task)) {
                    Queue.ADD_SUCCESS -> return true
                    Queue.ADD_CLOSED -> return false
                    Queue.ADD_FROZEN -> _queue.compareAndSet(queue, queue.next())
                }
            }
            else -> when {
                queue === CLOSED_EMPTY -> return false
                else -> {
                    // update to full-blown queue to add one more
                    val newQueue = Queue<Runnable>(Queue.INITIAL_CAPACITY, singleConsumer = true)
                    newQueue.addLast(queue as Runnable)
                    newQueue.addLast(task)
                    if (_queue.compareAndSet(queue, newQueue)) return true
                }
            }
        }
    }
}
```

参数task是第9步创建的DelayedResumeTask实例，首先判断loop是不是结束，结束直接返回false。 然后根据queue的类型判断，为空的时候，会将当前的task赋值给_queue。 下次再有task进来的时候，此时_
queue已经不为空了，同时也不是Queue类型，那么会走else分支，创建一个Queue<Runnable>实例， 然后将当前_queue和这次的task先后添加进Queue<Runnable>
实例newQueue里，然后将newQueue赋值给_queue，赋值成功返回true。 下次再有task进来的时候，此时_queue已经不为空了，同时是Queue类型，那么会走第二个分支，把task添加进_queue，成功返回true。
所以，_queue最终是一个Queue<Runnable>队列，要执行的task会被添加到队列里。

19、dequeue

```kotlin
@Suppress("UNCHECKED_CAST")
private fun dequeue(): Runnable? {
    _queue.loop { queue ->
        when (queue) {
            null -> return null
            is Queue<*> -> {
                val result = (queue as Queue<Runnable>).removeFirstOrNull()
                if (result !== Queue.REMOVE_FROZEN) return result as Runnable?
                _queue.compareAndSet(queue, queue.next())
            }
            else -> when {
                queue === CLOSED_EMPTY -> return null
                else -> if (_queue.compareAndSet(queue, null)) return queue as Runnable
            }
        }
    }
}
```

dequeue其实是enqueueImpl的逆向操作，_queue为空返回null，类型是Queue就调用`removeFirstOrNull`取出第一个任务返回， 非Queue类型说明自己本身就是任务，直接返回自己。

### EventLoop.common.kt

20、task.run()

```kotlin
private inner class DelayedResumeTask(
    nanoTime: Long,
    private val cont: CancellableContinuation<Unit>
) : DelayedTask(nanoTime) {
    override fun run() {
        with(cont) { resumeUndispatched(Unit) }
    }
    override fun toString(): String = super.toString() + cont.toString()
}
```

### Standard.kt

```kotlin
@kotlin.internal.InlineOnly
public inline fun <T, R> with(receiver: T, block: T.() -> R): R {
    contract {
        callsInPlace(block, InvocationKind.EXACTLY_ONCE)
    }
    return receiver.block()
}
```

这里的cont是在第6步创建的CancellableContinuationImpl实例。

###CancellableContinuationImpl.kt
21、resumeUndispatched
```kotlin
override fun CoroutineDispatcher.resumeUndispatched(value: T) {
    val dc = delegate as? DispatchedContinuation
    resumeImpl(value, if (dc?.dispatcher === this) MODE_UNDISPATCHED else resumeMode)
}
```
从第6步知道，这里的delegate是uCont.intercepted()，由第7步分析得知，delegate是DispatchedContinuation类型，所以dc非空。
dc?.dispatcher是Dispatchers.Default，this是CancellableContinuationImpl，所以不相等，结合第6步得：mode=resumeMode=MODE_CANCELLABLE。
22、resumeImpl
```kotlin
private fun resumeImpl(
    proposedUpdate: Any?,
    resumeMode: Int,
    onCancellation: ((cause: Throwable) -> Unit)? = null
) {
    _state.loop { state ->
        when (state) {
            is NotCompleted -> {
                //...
                dispatchResume(resumeMode) // dispatch resume, but it might get cancelled in process
                return // done
            }
            //...
        }
        alreadyResumedError(proposedUpdate) // otherwise, an error (second resume attempt)
    }
}
```
23、dispatchResume
```kotlin
private fun dispatchResume(mode: Int) {
    if (tryResume()) return // completed before getResult invocation -- bail out
    // otherwise, getResult has already commenced, i.e. completed later or in other thread
    dispatch(mode)
}
```
###DispatchedTask.kt
24、dispatch
```kotlin
internal fun <T> DispatchedTask<T>.dispatch(mode: Int) {
    assert { mode != MODE_UNINITIALIZED } // invalid mode value for this method
    val delegate = this.delegate
    val undispatched = mode == MODE_UNDISPATCHED
    if (!undispatched && delegate is DispatchedContinuation<*> && mode.isCancellableMode == resumeMode.isCancellableMode) {
        // dispatch directly using this instance's Runnable implementation
        val dispatcher = delegate.dispatcher
        val context = delegate.context
        if (dispatcher.isDispatchNeeded(context)) {
            dispatcher.dispatch(context, this)
        } else {
            resumeUnconfined()
        }
    } else {
        // delegate is coming from 3rd-party interceptor implementation (and does not support cancellation)
        // or undispatched mode was requested
        resume(delegate, undispatched)
    }
}
```
这里mode=resumeMode=MODE_CANCELLABLE，所以undispatched=false，然后会进入if分支。
首先取出delegate.dispatcher，前面分析过，是Dispatchers.Default，所以isDispatchNeeded=true，所以会调用dispatch方法进行任务的分发。

###SchedulerCoroutineDispatcher
```kotlin
override fun dispatch(context: CoroutineContext, block: Runnable): Unit = coroutineScheduler.dispatch(block)
```
coroutineScheduler.dispatch方法在Coroutine.md第22步详细分析过了。最终会将要恢复的协程CancellableContinuationImpl插入到Dispatchers.Default的队列里，等待Dispatchers.Default的线程取出执行。
从Coroutine.md第25步分析得知，Dispatchers.Default的线程会执行入口函数的协程体。意思就是，协程是在Dispatchers.Default的线程上恢复，执行完协程体剩下的部分。

任务执行完成以后，回到17步processNextEvent，直接返回0。 如果这次没有任务执行，则会计算下次任务的等待时间，并且返回。
25、nextTime
```kotlin
internal abstract class EventLoopImplBase : EventLoopImplPlatform(), Delay {
    // null | CLOSED_EMPTY | task | Queue<Runnable>
    private val _queue = atomic<Any?>(null)//即时任务队列

    // Allocated only only once
    private val _delayed = atomic<DelayedTaskQueue?>(null)//延时任务队列

    //...

    protected override val nextTime: Long
        get() {
            if (super.nextTime == 0L) return 0L
            val queue = _queue.value
            when {
                queue === null -> {} // empty queue -- proceed
                queue is Queue<*> -> if (!queue.isEmpty) return 0 // non-empty queue
                queue === CLOSED_EMPTY -> return Long.MAX_VALUE // no more events -- closed
                else -> return 0 // non-empty queue
            }
            val nextDelayedTask = _delayed.value?.peek() ?: return Long.MAX_VALUE
            return (nextDelayedTask.nanoTime - nanoTime()).coerceAtLeast(0)
        }

    //...
}
```
nextTime的主要逻辑就是从延时任务队列取出下一个任务，计算下个任务的时间与当前时间的差值，就是等待的时间。
nextTime返回以后，回到第16步。现在拿到等待时间，先判断时间>0和<最大值，然后调用`parkNanos(this, parkNanos)`让当前线程等待。
###DefaultExecutor.kt
26、parkNanos
```kotlin
@InlineOnly
internal inline fun parkNanos(blocker: Any, nanos: Long) {
    timeSource?.parkNanos(blocker, nanos) ?: LockSupport.parkNanos(blocker, nanos)
}
```

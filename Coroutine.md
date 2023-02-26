## 协程

### Hello.kt

0、程序入口

```kotlin
fun main() {
    suspend {
        println("start")
        val s = get()
        println(s)
        s
    }.createCoroutine(object : Continuation<String> {
        override val context: CoroutineContext = EmptyCoroutineContext

        override fun resumeWith(result: Result<String>) {
            println(Thread.currentThread())
        }

    }).resume(Unit)
}
suspend fun get(): String {
    return withContext(Dispatchers.IO) {
        val c = URL("https://cn.bing.com").openConnection()
        val a = c.getInputStream().readBytes()
        String(a)
    }
}
```

### SafeContinuationJvm.kt

1、createCoroutine

```kotlin
@SinceKotlin("1.3")
@Suppress("UNCHECKED_CAST")
public fun <T> (suspend () -> T).createCoroutine(
    completion: Continuation<T>
): Continuation<Unit> =
    SafeContinuation(createCoroutineUnintercepted(completion).intercepted(), COROUTINE_SUSPENDED)
```

创建了SafeContinuation示例返回

### Continuation.kt

2、resume

```kotlin
@SinceKotlin("1.3")
@InlineOnly
public inline fun <T> Continuation<T>.resume(value: T): Unit =
    resumeWith(Result.success(value))
```

resume是Continuation的扩展方法，里面实际调用的是Continuation的resumeWith方法

### SafeContinuationJvm.kt

3、resumeWith

```kotlin
@PublishedApi
@SinceKotlin("1.3")
internal actual class SafeContinuation<in T>
internal actual constructor(
    private val delegate: Continuation<T>,
    initialResult: Any?
) : Continuation<T>, CoroutineStackFrame {
    @Volatile
    private var result: Any? = initialResult

    public actual override fun resumeWith(result: Result<T>) {
        while (true) { // lock-free loop
            val cur = this.result // atomic read
            when {
                cur === UNDECIDED -> if (RESULT.compareAndSet(this, UNDECIDED, result.value)) return
                cur === COROUTINE_SUSPENDED -> if (RESULT.compareAndSet(this, COROUTINE_SUSPENDED, RESUMED)) {
                    delegate.resumeWith(result)
                    return
                }
                else -> throw IllegalStateException("Already resumed")
            }
        }
    }
}
```

从1创建SafeContinuation实例传入的是COROUTINE_SUSPENDED，所以执行delegate.resumeWith(result)。
SafeContinuation仅是一个代理类，他代理的是delegate才是真正实现协程的地方。 
从1得知，createCoroutineUnintercepted(completion).intercepted()。

### IntrinsicsJvm.kt

4、createCoroutineUnintercepted

```kotlin
@SinceKotlin("1.3")
public actual fun <T> (suspend () -> T).createCoroutineUnintercepted(
    completion: Continuation<T>
): Continuation<Unit> {
    val probeCompletion = probeCoroutineCreated(completion)
    return if (this is BaseContinuationImpl)
        create(probeCompletion)
    else
        createCoroutineFromSuspendFunction(probeCompletion) {
            (this as Function1<Continuation<T>, Any?>).invoke(it)
        }
}
```

### DebugProbes.kt

```kotlin
@SinceKotlin("1.3")
internal fun <T> probeCoroutineCreated(completion: Continuation<T>): Continuation<T> {
    /** implementation of this function is replaced by debugger */
    return completion
}
```

createCoroutineUnintercepted是(suspend () -> T)的扩展方法，所以方法体中的this就是(suspend () -> T)。
(suspend () -> T)是0中的一段suspend代码体。 查看Hello.kt编译后的字节码：

```
final class HelloKt$main$1 extends kotlin/coroutines/jvm/internal/SuspendLambda implements kotlin/jvm/functions/Function1
```

得知，0中的suspend代码体会被编译成一个类HelloKt$main$1，这个类继承SuspendLambda并实现Function1接口。
其中，SuspendLambda继承ContinuationImpl继承BaseContinuationImpl继承Continuation。
SuspendLambda是继承了BaseContinuationImpl，所以(this is BaseContinuationImpl)为true，执行create(probeCompletion)。

### ContinuationImpl.kt

5、create

```kotlin
public open fun create(value: Any?, completion: Continuation<*>): Continuation<Unit> {
    throw UnsupportedOperationException("create(Any?;Continuation) has not been overridden")
}
```
create是在BaseContinuationImpl定义的方法，从上面代码看出，create的具体实现放在了子类。
这里BaseContinuationImpl的子类就是HelloKt$main$1。
将HelloKt$main$1反编译成java：

```java
public final class HelloKt {
    Function1 var2 = (Function1) (new Function1((Continuation) null) {

        @NotNull
        public final Continuation create(@NotNull Continuation completion) {
            Intrinsics.checkNotNullParameter(completion, "completion");
            /**
             NEW HelloKt$main$1
             DUP
             ALOAD 1
             INVOKESPECIAL HelloKt$main$1.<init> (Lkotlin/coroutines/Continuation;)V
             ASTORE 2
             ALOAD 2
             ARETURN
             */
            Function1 var2 = new HelloKt$main$1(completion);
            return var2;
        }
    });
}
```
得知，create就是重新new一个HelloKt$main$1实例，并把4中的completion传入。
综上，4中，createCoroutineUnintercepted(completion) = new HelloKt$main$1(completion)。

### IntrinsicsJvm.kt
6、intercepted
```kotlin
@SinceKotlin("1.3")
public actual fun <T> Continuation<T>.intercepted(): Continuation<T> =
    (this as? ContinuationImpl)?.intercepted() ?: this
```
该扩展方法的receiver是Continuation<T>，此处是createCoroutineUnintercepted返回，即HelloKt$main$1。
HelloKt$main$1是继承了ContinuationImpl，所以此处将执行ContinuationImpl类的intercepted方法
### ContinuationImpl.kt
7、intercepted
```kotlin
@SinceKotlin("1.3")
// State machines for named suspend functions extend from this class
internal abstract class ContinuationImpl(
    completion: Continuation<Any?>?,
    private val _context: CoroutineContext?
) : BaseContinuationImpl(completion) {
    constructor(completion: Continuation<Any?>?) : this(completion, completion?.context)

    public override val context: CoroutineContext
        get() = _context!!

    public fun intercepted(): Continuation<Any?> =
        intercepted
            ?: (context[ContinuationInterceptor]?.interceptContinuation(this) ?: this)
                .also { intercepted = it }
}
```
intercepted的初始值为空，所以是`context[ContinuationInterceptor]?.interceptContinuation(this) ?: this`。
这里的context是通过ContinuationImpl的构造方法传入的。 
从`constructor(completion: Continuation<Any?>?) : this(completion, completion?.context)`看出，
这里的completion为0中传入的Continuation实例，它的context设置为EmptyCoroutineContext，

###CoroutineContextImpl.kt
```kotlin
@SinceKotlin("1.3")
public object EmptyCoroutineContext : CoroutineContext, Serializable {

    public override fun <E : Element> get(key: Key<E>): E? = null
    public override fun <R> fold(initial: R, operation: (R, Element) -> R): R = initial
    public override fun plus(context: CoroutineContext): CoroutineContext = context
    public override fun minusKey(key: Key<*>): CoroutineContext = this
}
```
所以context[ContinuationInterceptor]为null，返回this.
综上，1中，createCoroutineUnintercepted(completion).intercepted() = new HelloKt$main$1(completion)。
所以，3中，delegate.resumeWith(result)等价于执行HelloKt$main$1的resumeWith方法。

### ContinuationImpl.kt
8、resumeWith
```kotlin
public final override fun resumeWith(result: Result<Any?>) {
    // This loop unrolls recursion in current.resumeWith(param) to make saner and shorter stack traces on resume
    var current = this
    var param = result
    while (true) {
        // Invoke "resume" debug probe on every resumed continuation, so that a debugging library infrastructure
        // can precisely track what part of suspended callstack was already resumed
        probeCoroutineResumed(current)
        with(current) {
            val completion = completion!! // fail fast when trying to resume continuation without completion
            val outcome: Result<Any?> =
                try {
                    val outcome = invokeSuspend(param)
                    if (outcome === COROUTINE_SUSPENDED) return
                    Result.success(outcome)
                } catch (exception: Throwable) {
                    Result.failure(exception)
                }
            releaseIntercepted() // this state machine instance is terminating
            if (completion is BaseContinuationImpl) {
                // unrolling recursion via loop
                current = completion
                param = outcome
            } else {
                // top-level completion reached -- invoke and return
                completion.resumeWith(outcome)
                return
            }
        }
    }
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
执行invokeSuspend(param)  
9、invokeSuspend
```java
public final class HelloKt {
    Function1 var2 = (Function1) (new Function1((Continuation) null) {
        int label;

        @Nullable
        public final Object invokeSuspend(@NotNull Object $result) {
            Object var5 = IntrinsicsKt.getCOROUTINE_SUSPENDED();
            Object var10000;
            switch (this.label) {
                case 0:
                    ResultKt.throwOnFailure($result);
                    Thread var2 = Thread.currentThread();
                    boolean var3 = false;
                    System.out.println(var2);
                    this.label = 1;
                    var10000 = HelloKt.get(this);
                    if (var10000 == var5) {
                        return var5;
                    }
                    break;
                case 1:
                    ResultKt.throwOnFailure($result);
                    var10000 = $result;
                    break;
                default:
                    throw new IllegalStateException("call to 'resume' before 'invoke' with coroutine");
            }

            String s = (String) var10000;
            Thread var7 = Thread.currentThread();
            boolean var4 = false;
            System.out.println(var7);
            return s;
        }
    });
}
```
label的初始值为0，若`var10000 = HelloKt.get(this)`为COROUTINE_SUSPENDED，该方法会被挂起，表示等待执行结果。
如果不为为COROUTINE_SUSPENDED，则表示马上就拿到结果，那么直接执行后续逻辑。
这里调用的get方法是个耗时方法，不会马上拿到结果，所以该方法被挂起，等待执行get方法。

### Builders.common.kt
10、withContext
```kotlin
public suspend fun <T> withContext(
    context: CoroutineContext,
    block: suspend CoroutineScope.() -> T
): T {
    contract {
        callsInPlace(block, InvocationKind.EXACTLY_ONCE)
    }
    return suspendCoroutineUninterceptedOrReturn sc@{ uCont ->
        // compute new context
        val oldContext = uCont.context
        // Copy CopyableThreadContextElement if necessary
        val newContext = oldContext.newCoroutineContext(context)
        // always check for cancellation of new context
        newContext.ensureActive()
        // FAST PATH #1 -- new context is the same as the old one
        if (newContext === oldContext) {
            val coroutine = ScopeCoroutine(newContext, uCont)
            return@sc coroutine.startUndispatchedOrReturn(coroutine, block)
        }
        // FAST PATH #2 -- the new dispatcher is the same as the old one (something else changed)
        // `equals` is used by design (see equals implementation is wrapper context like ExecutorCoroutineDispatcher)
        if (newContext[ContinuationInterceptor] == oldContext[ContinuationInterceptor]) {
            val coroutine = UndispatchedCoroutine(newContext, uCont)
            // There are changes in the context, so this thread needs to be updated
            withCoroutineContext(newContext, null) {
                return@sc coroutine.startUndispatchedOrReturn(coroutine, block)
            }
        }
        // SLOW PATH -- use new dispatcher
        val coroutine = DispatchedCoroutine(newContext, uCont)
        block.startCoroutineCancellable(coroutine, coroutine)
        coroutine.getResult()
    }
}
```

### Intrinsics.kt

```kotlin
@SinceKotlin("1.3")
@InlineOnly
@Suppress("UNUSED_PARAMETER", "RedundantSuspendModifier")
public suspend inline fun <T> suspendCoroutineUninterceptedOrReturn(crossinline block: (Continuation<T>) -> Any?): T {
    contract { callsInPlace(block, InvocationKind.EXACTLY_ONCE) }
    throw NotImplementedError("Implementation of suspendCoroutineUninterceptedOrReturn is intrinsic")
}
```

`suspendCoroutineUninterceptedOrReturn`没有源码，直接看编译后的字节码，发现是执行传入的block。
其中参数uCont是从9中`var10000 = HelloKt.get(this)`传入的this，类型为Continuation。
* `val oldContext = uCont.context`  
oldContext = HelloKt$main$1的context = EmptyCoroutineContext
* `val newContext = oldContext.newCoroutineContext(context)`  
这里的context是Dispatchers.IO。
newCoroutineContext是CoroutineContext的扩展方法，定义在CoroutineContext.common.kt
###CoroutineContext.common.kt
```kotlin
@InternalCoroutinesApi
public expect fun CoroutineContext.newCoroutineContext(addedContext: CoroutineContext): CoroutineContext
```
这个方法的具体实现有三种，分别为js、native、jvm。这里看jvm的实现。
###CoroutineContext.kt
```kotlin
@InternalCoroutinesApi
public actual fun CoroutineContext.newCoroutineContext(addedContext: CoroutineContext): CoroutineContext {
    /*
     * Fast-path: we only have to copy/merge if 'addedContext' (which typically has one or two elements)
     * contains copyable elements.
     */
    if (!addedContext.hasCopyableElements()) return this + addedContext
    return foldCopies(this, addedContext, false)
}
private fun CoroutineContext.hasCopyableElements(): Boolean =
    fold(false) { result, it -> result || it is CopyableThreadContextElement<*> }

public override fun <R> fold(initial: R, operation: (R, Element) -> R): R =
    operation(initial, this)
```
Dispatchers.IO没有继承CopyableThreadContextElement，所以hasCopyableElements为false。
所以newCoroutineContext = EmptyCoroutineContext + Dispatchers.IO。
从EmptyCoroutineContext的plus方法可知，EmptyCoroutineContext + Dispatchers.IO = Dispatchers.IO。
所以newContext = Dispatchers.IO。
所以newContext !== oldContext。
oldContext[ContinuationInterceptor] = EmptyCoroutineContext[ContinuationInterceptor] = null
newContext[ContinuationInterceptord] = Dispatchers.IO[ContinuationInterceptor] = DefaultIoScheduler[ContinuationInterceptor]
其中，DefaultIoScheduler继承ExecutorCoroutineDispatcher继承CoroutineDispatcher
```kotlin
//CoroutineDispatcher.kt
public abstract class CoroutineDispatcher :
    AbstractCoroutineContextElement(ContinuationInterceptor), ContinuationInterceptor

//CoroutineContextImpl.kt
@SinceKotlin("1.3")
public abstract class AbstractCoroutineContextElement(public override val key: Key<*>) : Element

//ContinuationInterceptor.kt
public override operator fun <E : CoroutineContext.Element> get(key: CoroutineContext.Key<E>): E? {
    // getPolymorphicKey specialized for ContinuationInterceptor key
    @OptIn(ExperimentalStdlibApi::class)
    if (key is AbstractCoroutineContextKey<*, *>) {
        @Suppress("UNCHECKED_CAST")
        return if (key.isSubKey(this.key)) key.tryCast(this) as? E else null
    }
    @Suppress("UNCHECKED_CAST")
    return if (ContinuationInterceptor === key) this as E else null
}
```
所以DefaultIoScheduler[ContinuationInterceptor]=DefaultIoScheduler。
所以newContext[ContinuationInterceptord] ！= oldContext[ContinuationInterceptor]，所以执行
```kotlin
val coroutine = DispatchedCoroutine(newContext, uCont)
block.startCoroutineCancellable(coroutine, coroutine)
coroutine.getResult()
```

### Cancellable.kt
11、startCoroutineCancellable
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
依次调用createCoroutineUnintercepted、intercepted、resumeCancellableWith。
### IntrinsicsJvm.kt
12、createCoroutineUnintercepted
```kotlin
@SinceKotlin("1.3")
public actual fun <R, T> (suspend R.() -> T).createCoroutineUnintercepted(
    receiver: R,
    completion: Continuation<T>
): Continuation<Unit> {
    val probeCompletion = probeCoroutineCreated(completion)
    return if (this is BaseContinuationImpl)
        create(receiver, probeCompletion)
    else {
        createCoroutineFromSuspendFunction(probeCompletion) {
            (this as Function2<R, Continuation<T>, Any?>).invoke(receiver, it)
        }
    }
}
```
同4，不过这里(suspend () -> T)是get方法中withContext的协程体。 查看Hello.kt编译后的字节码：
```
final class HelloKt$get$2 extends kotlin/coroutines/jvm/internal/SuspendLambda implements kotlin/jvm/functions/Function2
```
其中，SuspendLambda继承ContinuationImpl继承BaseContinuationImpl继承Continuation。
SuspendLambda是继承了BaseContinuationImpl，所以(this is BaseContinuationImpl)为true，执行`create(receiver, probeCompletion)`。

### Hello.kt
13、create
同5，将HelloKt$get$2反编译成java：
```
public final Continuation create(@Nullable Object value, @NotNull Continuation completion) {
    Function2 var3 = new HelloKt$get$2(completion);
    return var3;
}
```
得知，create就是重新new一个HelloKt$get$2实例，并把12中的completion传入。
12接收的completion是10中的`val coroutine = DispatchedCoroutine(newContext, uCont)`
综上，12中，createCoroutineUnintercepted(receiver, completion) = new HelloKt$get$2(completion)。

### IntrinsicsJvm.kt
14、intercepted
```kotlin
@SinceKotlin("1.3")
public actual fun <T> Continuation<T>.intercepted(): Continuation<T> =
    (this as? ContinuationImpl)?.intercepted() ?: this
```
该扩展方法的receiver是Continuation<T>，此处是createCoroutineUnintercepted返回，即HelloKt$get$2。
HelloKt$get$2是继承了ContinuationImpl，所以此处将执行ContinuationImpl类的intercepted方法

### ContinuationImpl.kt
15、intercepted
```kotlin
public fun intercepted(): Continuation<Any?> =
    intercepted
        ?: (context[ContinuationInterceptor]?.interceptContinuation(this) ?: this)
            .also { intercepted = it }
```
intercepted的初始值为空，所以是`context[ContinuationInterceptor]?.interceptContinuation(this) ?: this`。
同7，这里的context是在ContinuationImpl的构造方法传入的参数completion的context。
这里的completion?.context为10中创建的DispatchedCoroutine实例里面的成员变量context。

```kotlin
//Builders.common.kt
internal class DispatchedCoroutine<in T>(
    context: CoroutineContext,
    uCont: Continuation<T>
) : ScopeCoroutine<T>(context, uCont)

//Scopes.kt
internal open class ScopeCoroutine<in T>(
    context: CoroutineContext,
    @JvmField val uCont: Continuation<T> // unintercepted continuation
) : AbstractCoroutine<T>(context, true, true), CoroutineStackFrame

//AbstractCoroutine.kt
@InternalCoroutinesApi
public abstract class AbstractCoroutine<in T>(
    parentContext: CoroutineContext,
    initParentJob: Boolean,
    active: Boolean
) : JobSupport(active), Job, Continuation<T>, CoroutineScope {
    @Suppress("LeakingThis")
    public final override val context: CoroutineContext = parentContext + this
}
```
这里的parentContext就是10中的newContext，即是Dispatchers.IO。
```kotlin
//CoroutineContext.kt
public operator fun plus(context: CoroutineContext): CoroutineContext =
    if (context === EmptyCoroutineContext) this else // fast path -- avoid lambda creation
        context.fold(this) { acc, element ->
            val removed = acc.minusKey(element.key)
            if (removed === EmptyCoroutineContext) element else {
                // make sure interceptor is always last in the context (and thus is fast to get when present)
                val interceptor = removed[ContinuationInterceptor]
                if (interceptor == null) CombinedContext(removed, element) else {
                    val left = removed.minusKey(ContinuationInterceptor)
                    if (left === EmptyCoroutineContext) CombinedContext(element, interceptor) else
                        CombinedContext(CombinedContext(left, element), interceptor)
                }
            }
        }

//ContinuationInterceptor.kt
public override fun minusKey(key: CoroutineContext.Key<*>): CoroutineContext {
    // minusPolymorphicKey specialized for ContinuationInterceptor key
    @OptIn(ExperimentalStdlibApi::class)
    if (key is AbstractCoroutineContextKey<*, *>) {
        return if (key.isSubKey(this.key) && key.tryCast(this) != null) EmptyCoroutineContext else this
    }
    return if (ContinuationInterceptor === key) EmptyCoroutineContext else this
}
```
这里acc=Dispatchers.IO，element=DispatchedCoroutine，element.key=Job。
所以removed=Dispatchers.IO，从10分析得知，interceptor=removed[ContinuationInterceptor]=Dispatchers.IO，非空。
left = EmptyCoroutineContext，所以返回 CombinedContext(element, interceptor)。
综上，context[ContinuationInterceptor]?.interceptContinuation(this)的context[ContinuationInterceptor]=CombinedContext(element, interceptor)[ContinuationInterceptor]

```kotlin
//CoroutineContextImpl.kt
@SinceKotlin("1.3")
internal class CombinedContext(
    private val left: CoroutineContext,
    private val element: Element
) : CoroutineContext, Serializable {
    override fun <E : Element> get(key: Key<E>): E? {
        var cur = this
        while (true) {
            cur.element[key]?.let { return it }
            val next = cur.left
            if (next is CombinedContext) {
                cur = next
            } else {
                return next[key]
            }
        }
    }
}
```
cur.element[key]=Dispatchers.IO[ContinuationInterceptor]=Dispatchers.IO，非空。
所以执行`context[ContinuationInterceptor]?.interceptContinuation(this)`，
等于执行Dispatchers.IO?.interceptContinuation(this)。

### Dispatchers.kt

```kotlin
@JvmStatic
public val IO: CoroutineDispatcher = DefaultIoScheduler
```
DefaultIoScheduler继承ExecutorCoroutineDispatcher继承CoroutineDispatcher继承ContinuationInterceptor

### CoroutineDispatcher.kt
16、interceptContinuation
```kotlin
public open fun isDispatchNeeded(context: CoroutineContext): Boolean = true

public final override fun <T> interceptContinuation(continuation: Continuation<T>): Continuation<T> =
    DispatchedContinuation(this, continuation)
```
创建一个DispatchedContinuation实例，并返回。这里的参数continuation就是在13中创建HelloKt$get$2类型的实例。

### DispatchedContinuation.kt
17、resumeCancellableWith
```kotlin
internal class DispatchedContinuation<in T>(
    @JvmField val dispatcher: CoroutineDispatcher,
    @JvmField val continuation: Continuation<T>
) : DispatchedTask<T>(MODE_UNINITIALIZED), CoroutineStackFrame, Continuation<T> by continuation {
    @Suppress("NOTHING_TO_INLINE")
    inline fun resumeCancellableWith(
        result: Result<T>,
        noinline onCancellation: ((cause: Throwable) -> Unit)?
    ) {
        val state = result.toState(onCancellation)
        if (dispatcher.isDispatchNeeded(context)) {
            _state = state
            resumeMode = MODE_CANCELLABLE
            dispatcher.dispatch(context, this)
        } else {
            executeUnconfined(state, MODE_CANCELLABLE) {
                if (!resumeCancelled(state)) {
                    resumeUndispatchedWith(result)
                }
            }
        }
    }
}
```
从16得知，`dispatcher.isDispatchNeeded(context)`为true，所以执行`dispatcher.dispatch(context, this)`
这里的this就是当前的DispatchedContinuation实例。
DispatchedContinuation继承DispatchedTask继承SchedulerTask继承Runnable。

### Dispatcher.kt
18、dispatch
```kotlin
// Dispatchers.IO
internal object DefaultIoScheduler : ExecutorCoroutineDispatcher(), Executor {

    private val default = UnlimitedIoScheduler.limitedParallelism(
        systemProp(
            IO_PARALLELISM_PROPERTY_NAME,
            64.coerceAtLeast(AVAILABLE_PROCESSORS)
        )
    )

    override fun dispatch(context: CoroutineContext, block: Runnable) {
        default.dispatch(context, block)
    }
}
```
UnlimitedIoScheduler是继承CoroutineDispatcher。
### CoroutineDispatcher.kt

```kotlin
@ExperimentalCoroutinesApi
public open fun limitedParallelism(parallelism: Int): CoroutineDispatcher {
    parallelism.checkParallelism()
    return LimitedDispatcher(this, parallelism)
}
```
这里的default就是LimitedDispatcher实例，参数this是对象UnlimitedIoScheduler。

### LimitedDispatcher.kt
19、dispatch
```kotlin
internal class LimitedDispatcher(
    private val dispatcher: CoroutineDispatcher,
    private val parallelism: Int
) : CoroutineDispatcher(), Runnable, Delay by (dispatcher as? Delay ?: DefaultDelay) {
    private val queue = LockFreeTaskQueue<Runnable>(singleConsumer = false)
    
    override fun dispatch(context: CoroutineContext, block: Runnable) {
        dispatchInternal(block) {
            dispatcher.dispatch(this, this)
        }
    }
    private inline fun dispatchInternal(block: Runnable, dispatch: () -> Unit) {
        // Add task to queue so running workers will be able to see that
        if (addAndTryDispatching(block)) return
        /*
         * Protect against the race when the number of workers is enough,
         * but one (because of synchronized serialization) attempts to complete,
         * and we just observed the number of running workers smaller than the actual
         * number (hit right between `--runningWorkers` and `++runningWorkers` in `run()`)
         */
        if (!tryAllocateWorker()) return
        dispatch()
    }

    private fun addAndTryDispatching(block: Runnable): Boolean {
        queue.addLast(block)
        return runningWorkers >= parallelism
    }

    private fun tryAllocateWorker(): Boolean {
        synchronized(workerAllocationLock) {
            if (runningWorkers >= parallelism) return false
            ++runningWorkers
            return true
        }
    }
}
```
开始的时候runningWorkers较小，所以addAndTryDispatching为false，tryAllocateWorker为true，所以执行dispatch。
执行dispatch就是执行`dispatcher.dispatch(this, this)`。dispatcher = UnlimitedIoScheduler。

### Dispatcher.kt
20、dispatch
```kotlin
private object UnlimitedIoScheduler : CoroutineDispatcher() {

    @InternalCoroutinesApi
    override fun dispatchYield(context: CoroutineContext, block: Runnable) {
        DefaultScheduler.dispatchWithContext(block, BlockingContext, true)
    }

    override fun dispatch(context: CoroutineContext, block: Runnable) {
        DefaultScheduler.dispatchWithContext(block, BlockingContext, false)
    }
}
```
21、dispatchWithContext  
DefaultScheduler继承SchedulerCoroutineDispatcher
```kotlin
//Dispatcher.kt
// Instance of Dispatchers.Default
internal object DefaultScheduler : SchedulerCoroutineDispatcher(
    CORE_POOL_SIZE, MAX_POOL_SIZE,
    IDLE_WORKER_KEEP_ALIVE_NS, DEFAULT_SCHEDULER_NAME
)

// Instantiated in tests so we can test it in isolation
internal open class SchedulerCoroutineDispatcher(
    private val corePoolSize: Int = CORE_POOL_SIZE,
    private val maxPoolSize: Int = MAX_POOL_SIZE,
    private val idleWorkerKeepAliveNs: Long = IDLE_WORKER_KEEP_ALIVE_NS,
    private val schedulerName: String = "CoroutineScheduler",
) : ExecutorCoroutineDispatcher() {

    // This is variable for test purposes, so that we can reinitialize from clean state
    private var coroutineScheduler = createScheduler()

    private fun createScheduler() =
        CoroutineScheduler(corePoolSize, maxPoolSize, idleWorkerKeepAliveNs, schedulerName)

    internal fun dispatchWithContext(block: Runnable, context: TaskContext, tailDispatch: Boolean) {
        coroutineScheduler.dispatch(block, context, tailDispatch)
    }
}
```

### CoroutineScheduler.kt
22、dispatch
```kotlin
fun dispatch(block: Runnable, taskContext: TaskContext = NonBlockingContext, tailDispatch: Boolean = false) {
    trackTask() // this is needed for virtual time support
    val task = createTask(block, taskContext)
    // try to submit the task to the local queue and act depending on the result
    val currentWorker = currentWorker()
    val notAdded = currentWorker.submitToLocalQueue(task, tailDispatch)
    if (notAdded != null) {
        if (!addToGlobalQueue(notAdded)) {
            // Global queue is closed in the last step of close/shutdown -- no more tasks should be accepted
            throw RejectedExecutionException("$schedulerName was terminated")
        }
    }
    val skipUnpark = tailDispatch && currentWorker != null
    // Checking 'task' instead of 'notAdded' is completely okay
    if (task.mode == TASK_NON_BLOCKING) {
        if (skipUnpark) return
        signalCpuWork()
    } else {
        // Increment blocking tasks anyway
        signalBlockingWork(skipUnpark = skipUnpark)
    }
}
```
1. `val task = createTask(block, taskContext)`
```kotlin
fun createTask(block: Runnable, taskContext: TaskContext): Task {
    val nanoTime = schedulerTimeSource.nanoTime()
    if (block is Task) {
        block.submissionTime = nanoTime
        block.taskContext = taskContext
        return block
    }
    return TaskImpl(block, nanoTime, taskContext)
}
```
这里的block是19中传入的LimitedDispatcher实例，由于LimitedDispatcher没有继承Task，
所以`lock is Task`为false，这里返回`TaskImpl(block, nanoTime, taskContext)`

2. `val currentWorker = currentWorker()`
```kotlin
private fun currentWorker(): Worker? = (Thread.currentThread() as? Worker)?.takeIf { it.scheduler == this }
```
因为当前还处在主线程，所以当前线程并不能转成Worker，所以这里返回空。

3. `val notAdded = currentWorker.submitToLocalQueue(task, tailDispatch)`
```kotlin
private fun Worker?.submitToLocalQueue(task: Task, tailDispatch: Boolean): Task? {
    if (this == null) return task
    /*
     * This worker could have been already terminated from this thread by close/shutdown and it should not
     * accept any more tasks into its local queue.
     */
    if (state === WorkerState.TERMINATED) return task
    // Do not add CPU tasks in local queue if we are not able to execute it
    if (task.mode == TASK_NON_BLOCKING && state === WorkerState.BLOCKING) {
        return task
    }
    mayHaveLocalTasks = true
    return localQueue.add(task, fair = tailDispatch)
}
```
当Worker为空的时候，直接返回task。那么notAdded是非空，执行addToGlobalQueue。
(这里假设当前线程是工作线程Worker，会先判断该线程状态是否为终止或者阻塞，两者都否表示当前线程可用，于是将任务插入队列localQueue，插入成功返回null。
localQueue是Worker自身的队列。如果当前线程状态是终止或阻塞，则调用addToGlobalQueue将任务插入到CoroutineScheduler的公共队列globalCpuQueue或globalBlockingQueue)

4. `addToGlobalQueue(notAdded)`
```kotlin
@JvmField
val globalCpuQueue = GlobalQueue()

@JvmField
val globalBlockingQueue = GlobalQueue()

private fun addToGlobalQueue(task: Task): Boolean {
    return if (task.isBlocking) {
        globalBlockingQueue.addLast(task)
    } else {
        globalCpuQueue.addLast(task)
    }
}
```
将任务插入到队列中，如果add失败直接抛异常，现在假设add成功。
这里的task.mode是20中传入的BlockingContext，他的mode是TASK_PROBABLY_BLOCKING。
```kotlin
@JvmField
internal val BlockingContext: TaskContext = TaskContextImpl(TASK_PROBABLY_BLOCKING)
```
所以执行signalBlockingWork，因为currentWorker=null，所以skipUnpark=false
5. `signalBlockingWork(skipUnpark = skipUnpark)`
```kotlin
private fun signalBlockingWork(skipUnpark: Boolean) {
    // Use state snapshot to avoid thread overprovision
    val stateSnapshot = incrementBlockingTasks()
    if (skipUnpark) return
    if (tryUnpark()) return
    if (tryCreateWorker(stateSnapshot)) return
    tryUnpark() // Try unpark again in case there was race between permit release and parking
}
```
5. 1)tryUnpark
```kotlin
private fun tryUnpark(): Boolean {
    while (true) {
        val worker = parkedWorkersStackPop() ?: return false
        if (worker.workerCtl.compareAndSet(PARKED, CLAIMED)) {
            LockSupport.unpark(worker)
            return true
        }
    }
}
```
parkedWorkersStackPop：开启一个死循环，从等待的Worker堆栈中提取一个Worker，如果失败，直接返回false。如果成功，则尝试修改Worker的状态，
即尝试将worker.workerCtl从PARKED修改为CLAIMED，如果成功，则将该线程唤醒，并返回true。
这里先假设刚开始，没有worker，那么tryUnpark返回false。这种情况下将会创建一个worker。
5. 2)tryCreateWorker
```kotlin
private fun tryCreateWorker(state: Long = controlState.value): Boolean {
    val created = createdWorkers(state)
    val blocking = blockingTasks(state)
    val cpuWorkers = (created - blocking).coerceAtLeast(0)
    /*
     * We check how many threads are there to handle non-blocking work,
     * and create one more if we have not enough of them.
     */
    if (cpuWorkers < corePoolSize) {
        val newCpuWorkers = createNewWorker()
        // If we've created the first cpu worker and corePoolSize > 1 then create
        // one more (second) cpu worker, so that stealing between them is operational
        if (newCpuWorkers == 1 && corePoolSize > 1) createNewWorker()
        if (newCpuWorkers > 0) return true
    }
    return false
}
```
调用createNewWorker创建worker
5. 3）createNewWorker
```kotlin
private fun createNewWorker(): Int {
    synchronized(workers) {
        // Make sure we're not trying to resurrect terminated scheduler
        if (isTerminated) return -1
        val state = controlState.value
        val created = createdWorkers(state)
        val blocking = blockingTasks(state)
        val cpuWorkers = (created - blocking).coerceAtLeast(0)
        // Double check for overprovision
        if (cpuWorkers >= corePoolSize) return 0
        if (created >= maxPoolSize) return 0
        // start & register new worker, commit index only after successful creation
        val newIndex = createdWorkers + 1
        require(newIndex > 0 && workers[newIndex] == null)
        /*
         * 1) Claim the slot (under a lock) by the newly created worker
         * 2) Make it observable by increment created workers count
         * 3) Only then start the worker, otherwise it may miss its own creation
         */
        val worker = Worker(newIndex)
        workers.setSynchronized(newIndex, worker)
        require(newIndex == incrementCreatedWorkers())
        worker.start()
        return cpuWorkers + 1
    }
}
```
可以看到，里面直接new了一个Worker实例，并把实例加到Worker数组workers管理缓存复用。然后调用start启动该worker。
无论是提取一个存在的等待的worker唤醒，还是创建一个worker启动，都会执行worker里面的逻辑。
总结一下dispatch的逻辑，创建一个任务task，然后添加到队列里面，然后唤醒一个worker或新启动一个worker。
接下来看看Worker里面做了什么工作。  

23、runWorker
```kotlin
internal inner class Worker private constructor() : Thread() {
    override fun run() = runWorker()

    @JvmField
    var mayHaveLocalTasks = false

    private fun runWorker() {
        var rescanned = false
        while (!isTerminated && state != WorkerState.TERMINATED) {
            val task = findTask(mayHaveLocalTasks)
            // Task found. Execute and repeat
            if (task != null) {
                rescanned = false
                minDelayUntilStealableTaskNs = 0L
                executeTask(task)
                continue
            } else {
                mayHaveLocalTasks = false
            }
            /*
             * No tasks were found:
             * 1) Either at least one of the workers has stealable task in its FIFO-buffer with a stealing deadline.
             *    Then its deadline is stored in [minDelayUntilStealableTask]
             * // '2)' can be found below
             *
             * Then just park for that duration (ditto re-scanning).
             * While it could potentially lead to short (up to WORK_STEALING_TIME_RESOLUTION_NS ns) starvations,
             * excess unparks and managing "one unpark per signalling" invariant become unfeasible, instead we are going to resolve
             * it with "spinning via scans" mechanism.
             * NB: this short potential parking does not interfere with `tryUnpark`
             */
            if (minDelayUntilStealableTaskNs != 0L) {
                if (!rescanned) {
                    rescanned = true
                } else {
                    rescanned = false
                    tryReleaseCpu(WorkerState.PARKING)
                    interrupted()
                    LockSupport.parkNanos(minDelayUntilStealableTaskNs)
                    minDelayUntilStealableTaskNs = 0L
                }
                continue
            }
            /*
             * 2) Or no tasks available, time to park and, potentially, shut down the thread.
             * Add itself to the stack of parked workers, re-scans all the queues
             * to avoid missing wake-up (requestCpuWorker) and either starts executing discovered tasks or parks itself awaiting for new tasks.
             */
            tryPark()
        }
        tryReleaseCpu(WorkerState.TERMINATED)
    }
}
```
Worker继承了Thread。所以会执行run方法，再执行runWorker。
开启一个循环，不停通过findTask从队列里面取出task，然后拿到以后，调用executeTask执行任务。
如果没取到，会判断是否是延时任务，如果是会重新再拿一次，这是因为在这期间，有可能延时时间已到，所以直接重试一次。如果拿到就返回task。
如果还拿不到，则将线程阻塞延迟时间。
如果不是延时任务，直接调用`tryPark`阻塞线程。其中有个变量idleWorkerKeepAliveNs，表示worker空闲存活的最长时间。
worker会先`parkNanos(idleWorkerKeepAliveNs)`，如果期间被被唤醒，则再循环去取任务执行。
如果过了idleWorkerKeepAliveNs，仍然未被唤醒，则将状态置为TERMINATED，终止线程。

```kotlin
1. findTask
fun findTask(scanLocalQueue: Boolean): Task? {
    if (tryAcquireCpuPermit()) return findAnyTask(scanLocalQueue)
    // If we can't acquire a CPU permit -- attempt to find blocking task
    val task = if (scanLocalQueue) {
        localQueue.poll() ?: globalBlockingQueue.removeFirstOrNull()
    } else {
        globalBlockingQueue.removeFirstOrNull()
    }
    return task ?: trySteal(blockingOnly = true)
}

private fun findAnyTask(scanLocalQueue: Boolean): Task? {
    /*
     * Anti-starvation mechanism: probabilistically poll either local
     * or global queue to ensure progress for both external and internal tasks.
     */
    if (scanLocalQueue) {
        val globalFirst = nextInt(2 * corePoolSize) == 0
        if (globalFirst) pollGlobalQueues()?.let { return it }
        localQueue.poll()?.let { return it }
        if (!globalFirst) pollGlobalQueues()?.let { return it }
    } else {
        pollGlobalQueues()?.let { return it }
    }
    return trySteal(blockingOnly = false)
}

private fun pollGlobalQueues(): Task? {
    if (nextInt(2) == 0) {
        globalCpuQueue.removeFirstOrNull()?.let { return it }
        return globalBlockingQueue.removeFirstOrNull()
    } else {
        globalBlockingQueue.removeFirstOrNull()?.let { return it }
        return globalCpuQueue.removeFirstOrNull()
    }
}
```
一共三种队列，globalCpuQueue、globalBlockingQueue、localQueue。先从自身的localQueue取，取不到再从公共的globalCpuQueue和globalBlockingQueue取。
这里假设取到从22中插入的任务，然后执行。

2. executeTask
```kotlin
private fun executeTask(task: Task) {
    val taskMode = task.mode
    idleReset(taskMode)
    beforeTask(taskMode)
    runSafely(task)
    afterTask(taskMode)
}

fun runSafely(task: Task) {
    try {
        task.run()
    } catch (e: Throwable) {
        val thread = Thread.currentThread()
        thread.uncaughtExceptionHandler.uncaughtException(thread, e)
    } finally {
        unTrackTask()
    }
}
```
执行task的run方法。这里的task是在22中通过createTask创建并插入的，22中的task是`TaskImpl(block, nanoTime, taskContext)`
```kotlin
internal class TaskImpl(
    @JvmField val block: Runnable,
    submissionTime: Long,
    taskContext: TaskContext
) : Task(submissionTime, taskContext) {
    override fun run() {
        try {
            block.run()
        } finally {
            taskContext.afterTask()
        }
    }
}
```
执行block.run()。这里的block是19中传入的LimitedDispatcher实例。
### LimitedDispatcher.kt
24、run
```kotlin
override fun run() {
    var fairnessCounter = 0
    while (true) {
        val task = queue.removeFirstOrNull()
        if (task != null) {
            try {
                task.run()
            } catch (e: Throwable) {
                handleCoroutineException(EmptyCoroutineContext, e)
            }
            // 16 is our out-of-thin-air constant to emulate fairness. Used in JS dispatchers as well
            if (++fairnessCounter >= 16 && dispatcher.isDispatchNeeded(this)) {
                // Do "yield" to let other views to execute their runnable as well
                // Note that we do not decrement 'runningWorkers' as we still committed to do our part of work
                dispatcher.dispatch(this, this)
                return
            }
            continue
        }

        synchronized(workerAllocationLock) {
            --runningWorkers
            if (queue.size == 0) return
            ++runningWorkers
            fairnessCounter = 0
        }
    }
}
```
这里开启一个循环，从queue中取出task，调用task.run()执行。
在19中，调用`queue.addLast(block)`往queue插入了任务，在这里取出来。这里的block是在17传入的DispatchedContinuation实例。

### DispatchedTask
25、run
```kotlin
internal class DispatchedContinuation<in T>(
    @JvmField val dispatcher: CoroutineDispatcher,
    @JvmField val continuation: Continuation<T>
) : DispatchedTask<T>(MODE_UNINITIALIZED), CoroutineStackFrame, Continuation<T> by continuation {
    override val delegate: Continuation<T>
        get() = this
    
    //DispatchedTask
    public final override fun run() {
        assert { resumeMode != MODE_UNINITIALIZED } // should have been set before dispatching
        val taskContext = this.taskContext
        var fatalException: Throwable? = null
        try {
            val delegate = delegate as DispatchedContinuation<T>
            val continuation = delegate.continuation
            withContinuationContext(continuation, delegate.countOrElement) {
                val context = continuation.context
                val state = takeState() // NOTE: Must take state in any case, even if cancelled
                val exception = getExceptionalResult(state)
                /*
                 * Check whether continuation was originally resumed with an exception.
                 * If so, it dominates cancellation, otherwise the original exception
                 * will be silently lost.
                 */
                val job = if (exception == null && resumeMode.isCancellableMode) context[Job] else null
                if (job != null && !job.isActive) {
                    val cause = job.getCancellationException()
                    cancelCompletedResult(state, cause)
                    continuation.resumeWithStackTrace(cause)
                } else {
                    if (exception != null) {
                        continuation.resumeWithException(exception)
                    } else {
                        continuation.resume(getSuccessfulResult(state))
                    }
                }
            }
        } catch (e: Throwable) {
            // This instead of runCatching to have nicer stacktrace and debug experience
            fatalException = e
        } finally {
            val result = runCatching { taskContext.afterTask() }
            handleFatalException(fatalException, result.exceptionOrNull())
        }
    }
}
```
withContinuationContext同样也有三种实现，这里看jvm的实现，执行block。
```kotlin
//CoroutineContext.kt
internal actual inline fun <T> withContinuationContext(continuation: Continuation<*>, countOrElement: Any?, block: () -> T): T {
    //...
    try {
        return block()
    } finally {
        if (undispatchedCompletion == null || undispatchedCompletion.clearThreadContext()) {
            restoreThreadContext(context, oldValue)
        }
    }
}
```
这里的delegate就是DispatchedContinuation实例本身，delegate.continuation就是通过DispatchedContinuation的构造方法传入的Continuation实例。
这里假设当前任务没有发生异常，没有取消，所以job=null，exception=null，所以执行`continuation.resume(getSuccessfulResult(state))`
这里的continuation就是在15中传入的HelloKt$get$2实例。

### ContinuationImpl.kt
26、resumeWith
```kotlin
public final override fun resumeWith(result: Result<Any?>) {
    // This loop unrolls recursion in current.resumeWith(param) to make saner and shorter stack traces on resume
    var current = this
    var param = result
    while (true) {
        // Invoke "resume" debug probe on every resumed continuation, so that a debugging library infrastructure
        // can precisely track what part of suspended callstack was already resumed
        probeCoroutineResumed(current)
        with(current) {
            val completion = completion!! // fail fast when trying to resume continuation without completion
            val outcome: Result<Any?> =
                try {
                    val outcome = invokeSuspend(param)
                    if (outcome === COROUTINE_SUSPENDED) return
                    Result.success(outcome)
                } catch (exception: Throwable) {
                    Result.failure(exception)
                }
            releaseIntercepted() // this state machine instance is terminating
            if (completion is BaseContinuationImpl) {
                // unrolling recursion via loop
                current = completion
                param = outcome
            } else {
                // top-level completion reached -- invoke and return
                completion.resumeWith(outcome)
                return
            }
        }
    }
}
```
将当前的协程current设为HelloKt$get$2，执行HelloKt$get$2的invokeSuspend方法，直接拿到结果。
然后去拿HelloKt$get$2的成员completion，由13知道，该成员是10中的`val coroutine = DispatchedCoroutine(newContext, uCont)`。
DispatchedCoroutine不继承BaseContinuationImpl，所以`completion is BaseContinuationImpl`为false。
于是执行`completion.resumeWith(outcome)`，入参为HelloKt$get$2的invokeSuspend的结果。

### AbstractCoroutine.kt
27、resumeWith
```kotlin
@InternalCoroutinesApi
public abstract class AbstractCoroutine<in T>(
    parentContext: CoroutineContext,
    initParentJob: Boolean,
    active: Boolean
) : JobSupport(active), Job, Continuation<T>, CoroutineScope {
    public final override fun resumeWith(result: Result<T>) {
        val state = makeCompletingOnce(result.toState())
        if (state === COMPLETING_WAITING_CHILDREN) return
        afterResume(state)
    }

    protected open fun afterResume(state: Any?): Unit = afterCompletion(state)
}
```

### Builders.common.kt
28、afterResume
```kotlin
internal class DispatchedCoroutine<in T>(
    context: CoroutineContext,
    uCont: Continuation<T>
) : ScopeCoroutine<T>(context, uCont) {

    override fun afterResume(state: Any?) {
        if (tryResume()) return // completed before getResult invocation -- bail out
        // Resume in a cancellable way because we have to switch back to the original dispatcher
        uCont.intercepted().resumeCancellableWith(recoverResult(state, uCont))
    }

    fun getResult(): Any? {
        if (trySuspend()) return COROUTINE_SUSPENDED
        // otherwise, onCompletionInternal was already invoked & invoked tryResume, and the result is in the state
        val state = this.state.unboxState()
        if (state is CompletedExceptionally) throw state.cause
        @Suppress("UNCHECKED_CAST")
        return state as T
    }
}
```
这里的uCont就是10中传入的HelloKt$main$1实例。由7知道，uCont.intercepted()=HelloKt$main$1。  
29、resumeCancellableWith
```kotlin
@InternalCoroutinesApi
public fun <T> Continuation<T>.resumeCancellableWith(
    result: Result<T>,
    onCancellation: ((cause: Throwable) -> Unit)? = null
): Unit = when (this) {
    is DispatchedContinuation -> resumeCancellableWith(result, onCancellation)
    else -> resumeWith(result)
}
```
因为HelloKt$main$1不继承DispatchedContinuation，所以执行`resumeWith(result)`。

同8，执行HelloKt$main$1的invokeSuspend方法，入参为HelloKt$get$2的invokeSuspend的结果。
```java
public final class HelloKt {
    Function1 var2 = (Function1) (new Function1((Continuation) null) {
        int label;

        @Nullable
        public final Object invokeSuspend(@NotNull Object $result) {
            Object var5 = IntrinsicsKt.getCOROUTINE_SUSPENDED();
            Object var10000;
            switch (this.label) {
                case 0:
                    ResultKt.throwOnFailure($result);
                    Thread var2 = Thread.currentThread();
                    boolean var3 = false;
                    System.out.println(var2);
                    this.label = 1;
                    var10000 = HelloKt.get(this);
                    if (var10000 == var5) {
                        return var5;
                    }
                    break;
                case 1:
                    ResultKt.throwOnFailure($result);
                    var10000 = $result;
                    break;
                default:
                    throw new IllegalStateException("call to 'resume' before 'invoke' with coroutine");
            }

            String s = (String) var10000;
            Thread var7 = Thread.currentThread();
            boolean var4 = false;
            System.out.println(var7);
            return s;
        }
    });
}
```
此时label经过9的执行，已经变为1。于是直接拿到结果，继续执行后面的逻辑。
然后去拿HelloKt$main$1的成员completion，该成员是在1传入的匿名Continuation实例。
此时，`completion is BaseContinuationImpl`为false，于是执行`completion.resumeWith(outcome)`，入参为HelloKt$main$1的invokeSuspend的结果。
（可以看出，通过BaseContinuationImpl的成员completion，能将各个协程关联起来。
可以一层层地往上恢复（走的回调）。直到completion不再继承BaseContinuationImpl。）
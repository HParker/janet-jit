# (jit/jitable (fn []))

Small callable Janet Jit. You tell the jit what functions to compile
and it compiles them on next call. The jit makes a huge number of
assumptions, so the jit is not theright choice in all cases.

Right now it only target System V x86 with nanboxed janet values. I
would love to support ARM and Janet without nanboxed, but haven't
doneso yet.

```janet
(defn lerp
  [a b t]
  (+ a (* t (- b a))))
(def lerp-jit (jit/jitable lerp))

(lerp-jit 1 20 0.3) # call just like usual, but will compile to binary on first call.
```

## Project Status

experimental / educational

## Setup

### use in your project

`jit.c` can be used directly in your project via jpm,

```janet
(declare-project :name "my project")
(declare-native :name "jit" :source @["jit.c"])
```

You can then build it with,

```janet
jpm build
```

## Performance

```
$ jpm build && janet benchmarks/microbench.janet
```

-------------------------------------------------------------------------------------------------------------------
| name             | interpreter ips   | interpreter elapsed | jit ips           | jit elapsed     | times faster |
|------------------|-------------------|---------------------|-------------------|-----------------|--------------|
| lerp             | 29918408.6608 ips | 3.3424 ellapsed     | 55818487.9227 ips | 1.7915 ellapsed | 1.87x faster |
| fade             | 14604545.4819 ips | 6.8472 ellapsed     | 39950763.5767 ips | 2.5031 ellapsed | 2.74x faster |
| dot              | 27826564.1137 ips | 3.5937 ellapsed     | 56335134.1300 ips | 1.7751 ellapsed | 2.02x faster |
| bubble sort      | 167147.7850 ips   | 0.0598 ellapsed     | 269108.6009 ips   | 0.0372 ellapsed | 1.61x faster |
| perlin gradients | 81021.3418 ips    | 0.0123 ellapsed     | 143108.6455 ips   | 0.0070 ellapsed | 1.77x faster |
| in               | 28750844.8431 ips | 3.4782 ellapsed     | 50421734.4458 ips | 1.9833 ellapsed | 1.75x faster |
| get              | 25482825.1394 ips | 3.9242 ellapsed     | 43789892.2086 ips | 2.2836 ellapsed | 1.72x faster |
| cmp              | 28202955.0538 ips | 3.5457 ellapsed     | 62948263.5131 ips | 1.5886 ellapsed | 2.23x faster |
| call             | 29130345.5453 ips | 3.4328 ellapsed     | 23202911.6174 ips | 4.3098 ellapsed | 0.80x faster |
| C call           | 22250024.1462 ips | 4.4944 ellapsed     | 35393128.2533 ips | 2.8254 ellapsed | 1.59x faster |
| call jit         | 26754024.7929 ips | 3.7378 ellapsed     | 50056373.4888 ips | 1.9977 ellapsed | 1.87x faster |

I believe the next big performance boost for larger functions will be inlining.

## Notable Differences from Janet interpreter

Jit generated code has less strict garantees about correctness and errors than the interpreter. It expects that you to pass it correct numeric focused code.

In Janet, `(+ "hi" 1)` raises an error. This JIT that should be treated as undefined behavior. On my machine this returns "hi", but that may change at any time.

This also applies to defining your own operators. writting,

```janet
(def addable
  @{
    :+ (fn [x y] (print x " " y))
   })
(+ addable 10)) # prints #<table> 10
```

works in the interpreter, but should be considered undefined behavior in the JIT.

There are also a number of operations the VM can do that the JIT today does not support. They mostly have to do with VM state and probably make bad candidates for the JIT anyways. Today the list of unsupported ops is:

- (ldu) load up value
- (setu) set up value
- (res) resume
- (sig) signal
- (prop) propagate
- (clo) closure

Operations that fall back to the interpreter, will use the JIT:

- push
- type check
- next
- call
- in
- get
- getindex
- set
- setindex
- length
- make array
- make tuple
- make buffer
- make string
- make table
- make struct

Using these operators can slow down jitted functions, but should work as expected.

## Other things

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a ~10% performance penalty. If you need to leave the JIT a lot, you won't have very good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

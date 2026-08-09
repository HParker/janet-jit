# (janet-jit (fn []))

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
------------------------------------------------------------------------
| name | interpreter ips | interpreter elapsed | jit ips | jit elapsed | times faster |
| lerp | 30076163.6578 ips | 3.3249 ellapsed | 55391189.6168 ips | 1.8053 ellapsed  | 1.84x faster |
| fade | 15188307.3811 ips | 6.5840 ellapsed | 40022490.8151 ips | 2.4986 ellapsed  | 2.64x faster |
| dot  | 26054360.3505 ips | 3.8381 ellapsed | 55481671.7551 ips | 1.8024 ellapsed  | 2.13x faster |
| in   | 29014488.2883 ips | 3.4466 ellapsed | 50005290.6583 ips | 1.9998 ellapsed  | 1.72x faster |
| get  | 25627316.2686 ips | 3.9021 ellapsed | 49932067.1727 ips | 2.0027 ellapsed  | 1.95x faster |
| cmp  | 27714058.1696 ips | 3.6083 ellapsed | 61109278.8144 ips | 1.6364 ellapsed  | 2.20x faster |
------------------------------------------------------------------------
```

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

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a about 10% performance penalty. If you need to leave the JIT a lot, you won't have very good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

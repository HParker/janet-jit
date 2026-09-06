# (jit/jitable (fn []))

Small single file callable Jit for the janet programming language. You tell the jit what functions to compile
and it compiles them on next call.

Right now it only target System V x86 with nanboxed janet values. I
would love to support ARM and Janet without nanboxed, but haven't
done so yet.

```janet
(defn lerp
  [a b t]
  (+ a (* t (- b a))))
(def lerp-jit (jit/jitable lerp))

(lerp-jit 1 20 0.3) # call just like usual, but will compile to binary on first call.
```

## API

### `(jit/jitable function &opt fallback-behavior)`

```janet
(jit/jitable (fn [x] (* x x)))
```

returns an abstract type which when called will first compile then run a version of the function you supplied.

#### Optional arguments

`jit/jitable` takes a optional argument specifying what to do if the argument types do not match the recorded signature. Valid options are:

| option    | behavior                                                                                                                                                                                         |
|-----------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| :error    | panic if different argument types are passed. This is useful if you want to assure your jitted functions are always called monomorphicly, but probably not a good option outside of dev and test |
| :fallback | When the signature does not match call the Janet function that was provided. This is a reasonable default if you want make one case fast, but not break everything else.                         |


### `(jit/compiled? jit-fn)`

Returns if a jitted function was compiled. Useful for knowing if you are seeing JIT behavior or pre-compiled behavior.

### `(get jit-fn :hir)`

Returns the High level Intermediate representation (HIR) of your program. Useful for debugging code generation and optimizing JIT behavior.

```janet
(defn my-fib [n] (if (< n 2) n (+ (my-fib (- n 1)) (my-fib (- n 2)))))
(def fib-jit (jit/jitable my-fib))
(pp (get fib-jit :hir))
# => (({:args ((:immus 0) (:unused :_) (:unused :_)) :result (:vreg 1) :type :load-arg} {:args ((:jimm <function my-fib>) (:unused :_) (:unused :_)) :result (:vreg 2) :type :load} {:args ((:vreg 1) (:imms 2) (:unused :_)) :result (:vreg 3) :type :lti} {:args ((:bb 2) (:bb 1) (:vreg 3)) :result (:unused :_) :type :jump-if-not}) ({:args ((:vreg 1) (:unused :_) (:unused :_)) :result (:unused :_) :type :ret}) ({:args ((:vreg 1) (:imms 1) (:unused :_)) :result (:vreg 4) :type :subi} {:args ((:vreg 4) (:unused :_) (:unused :_)) :result (:unused :_) :type :push} {:args ((:vreg 2) (:unused :_) (:unused :_)) :result (:vreg 5) :type :call} {:args ((:vreg 1) (:imms 2) (:unused :_)) :result (:vreg 6) :type :subi} {:args ((:vreg 6) (:unused :_) (:unused :_)) :result (:unused :_) :type :push} {:args ((:vreg 2) (:unused :_) (:unused :_)) :result (:vreg 7) :type :call} {:args ((:vreg 5) (:vreg 7) (:unused :_)) :result (:vreg 8) :type :add} {:args ((:vreg 8) (:unused :_) (:unused :_)) :result (:unused :_) :type :ret}))
```

there is a pretty printer in test/hir-debug.janet that will turn that output into:

```
---- basic block 0 -----
v1 = load-arg(0u)
v2 = load(<function my-fib>)
v3 = lti(v1, 2i)
_ = jump-if-not(bb2, bb1, v3)
---- basic block 1 -----
_ = ret(v1)
---- basic block 2 -----
v4 = subi(v1, 1i)
_ = push(v4)
v5 = call(v2)
v6 = subi(v1, 2i)
_ = push(v6)
v7 = call(v2)
v8 = add(v5, v7)
_ = ret(v8)
```

## Project Status

experimental / educational : Please report segfaults, behavior mismatches and performance regressions.

## Setup

### use in your project

`jit.c` can be used directly in your project via jpm,

```janet
(declare-project :name "my project")
(declare-native :name "jit" :source @["jit.c"])
```

## Contributing

You can then build it with,

```
jpm build
```

Run the tests with,

```
janet test/test-asm.janet
janet test/test-fns.janet
```

## Performance

```
$ jpm build && janet benchmarks/microbench.janet
```

---------------------------------------------------------------------------------------
| name                  | interpreter ips   | interpreter elapsed | jit ips           | jit elapsed     | times faster |
|-----------------------|-------------------|---------------------|-------------------|-----------------|--------------|
| fib                   | 133.8781 ips      | 7.4695 ellapsed     | 248.2442 ips      | 4.0283 ellapsed | 1.85x faster |
| 10 mul                | 22031517.0047 ips | 4.5390 ellapsed     | 51974349.7811 ips | 1.9240 ellapsed | 2.36x faster |
| len (tuple)           | 32352740.9406 ips | 3.0909 ellapsed     | 47378685.5751 ips | 2.1107 ellapsed | 1.46x faster |
| get (tuple & number)  | 30408868.5913 ips | 3.2885 ellapsed     | 43722734.9833 ips | 2.2871 ellapsed | 1.44x faster |
| lerp                  | 27538952.0302 ips | 0.3631 ellapsed     | 44019993.3000 ips | 0.2272 ellapsed | 1.60x faster |
| fade                  | 15102558.2411 ips | 0.6621 ellapsed     | 42252826.9305 ips | 0.2367 ellapsed | 2.80x faster |
| dot                   | 26314413.4375 ips | 0.3800 ellapsed     | 42530503.1858 ips | 0.2351 ellapsed | 1.62x faster |
| bubble sort           | 602.0981 ips      | 1.6609 ellapsed     | 608.0222 ips      | 1.6447 ellapsed | 1.01x faster |
| perlin gradients      | 89155.1173 ips    | 1.1216 ellapsed     | 117511.0833 ips   | 0.8510 ellapsed | 1.32x faster |
| in                    | 25656890.1517 ips | 0.3898 ellapsed     | 42126377.2528 ips | 0.2374 ellapsed | 1.64x faster |
| get                   | 26572971.5030 ips | 0.3763 ellapsed     | 43025828.7535 ips | 0.2324 ellapsed | 1.62x faster |
| call                  | 25661979.3710 ips | 0.3897 ellapsed     | 21168456.9973 ips | 0.4724 ellapsed | 0.82x faster |
| C call                | 21760819.3959 ips | 0.4595 ellapsed     | 29688328.8660 ips | 0.3368 ellapsed | 1.36x faster |
| call jit              | 27227228.8372 ips | 0.0367 ellapsed     | 32638293.2601 ips | 0.0306 ellapsed | 1.20x faster |
| deep-not= (false)     | 1564670.8341 ips  | 0.0639 ellapsed     | 1575417.5624 ips  | 0.0635 ellapsed | 1.01x faster |
| cmp (numeric)         | 29406045.8350 ips | 0.3401 ellapsed     | 46887159.1591 ips | 0.2133 ellapsed | 1.59x faster |
| cmp (type difference) | 29880584.2176 ips | 0.3347 ellapsed     | 36833424.8658 ips | 0.2715 ellapsed | 1.23x faster |
| cmp (not numeric)     | 24846590.9369 ips | 0.0402 ellapsed     | 29050851.4501 ips | 0.0344 ellapsed | 1.17x faster |

## Notable Differences from Janet interpreter

Defining your own operators is supported, but will likely not perform better than the interpreter:

```janet
(def addable
  @{
    :+ (fn [x y] (print x " " y))
   })
(+ addable 10)) # prints #<table> 10
```

works, but will end up doing nearly the same work as in the interpreter.

There are also a number of operations the VM can do that the JIT today does not support. They mostly have to do with VM state and probably make bad candidates for the JIT anyways. Today the list of unsupported ops is:

- `(ldu)` load up value
- `(setu)` set up value
- `(res)` resume
- `(sig)` signal
- `(prop)` propagate
- `(clo)` closure

Operations that always call into the interpreter:

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

All other operations can also fall back to the VM when runtime type information isn't provable.

## Other things

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a ~10% performance penalty. If you need to leave the JIT a lot, you won't have good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

- Bounds checks don't currently happen on binary shift type operterations which does not match the interpreter's behavior. (I want to change this)

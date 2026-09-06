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
| fib                   | 133.6935 ips      | 7.4798 ellapsed     | 239.2511 ips      | 4.1797 ellapsed | 1.79x faster |
| 10 mul                | 20632467.2423 ips | 4.8467 ellapsed     | 47215853.8607 ips | 2.1179 ellapsed | 2.29x faster |
| len (tuple)           | 31648314.9313 ips | 3.1597 ellapsed     | 46636476.5745 ips | 2.1442 ellapsed | 1.47x faster |
| get (tuple & number)  | 30175096.3800 ips | 3.3140 ellapsed     | 43519952.6056 ips | 2.2978 ellapsed | 1.44x faster |
| lerp                  | 28570264.1290 ips | 0.3500 ellapsed     | 44939597.9220 ips | 0.2225 ellapsed | 1.57x faster |
| fade                  | 14658801.2582 ips | 0.6822 ellapsed     | 42330445.9796 ips | 0.2362 ellapsed | 2.89x faster |
| dot                   | 27992717.8399 ips | 0.3572 ellapsed     | 51888382.1966 ips | 0.1927 ellapsed | 1.85x faster |
| bubble sort           | 600.8210 ips      | 1.6644 ellapsed     | 574.2124 ips      | 1.7415 ellapsed | 0.96x faster |
| perlin gradients      | 90163.0899 ips    | 1.1091 ellapsed     | 108192.8611 ips   | 0.9243 ellapsed | 1.20x faster |
| in                    | 27070759.1799 ips | 0.3694 ellapsed     | 46542171.6941 ips | 0.2149 ellapsed | 1.72x faster |
| get                   | 27081496.1872 ips | 0.3693 ellapsed     | 49009707.6422 ips | 0.2040 ellapsed | 1.81x faster |
| call                  | 26106147.6279 ips | 0.3831 ellapsed     | 22730621.4644 ips | 0.4399 ellapsed | 0.87x faster |
| C call                | 20340909.9071 ips | 0.4916 ellapsed     | 27925610.9201 ips | 0.3581 ellapsed | 1.37x faster |
| call jit              | 27525602.7360 ips | 0.0363 ellapsed     | 34025719.2889 ips | 0.0294 ellapsed | 1.24x faster |
| deep-not= (false)     | 1596794.1483 ips  | 0.0626 ellapsed     | 1505129.5417 ips  | 0.0664 ellapsed | 0.94x faster |
| cmp (numeric)         | 28687928.7746 ips | 0.3486 ellapsed     | 41006155.7411 ips | 0.2439 ellapsed | 1.43x faster |
| cmp (type difference) | 32248402.6862 ips | 0.3101 ellapsed     | 42636771.7187 ips | 0.2345 ellapsed | 1.32x faster |
| cmp (not numeric)     | 25514445.2067 ips | 0.0392 ellapsed     | 28952353.8645 ips | 0.0345 ellapsed | 1.13x faster |

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

- `(lds)` load self in a jitted function returns the jitted abstract self. This is required to allow Jitted functions to recurse to the jitted version instead of the Janet version, but can change program semantics.

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a ~10% performance penalty. If you need to leave the JIT a lot, you won't have good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

- Bounds checks don't currently happen on binary shift type operterations which does not match the interpreter's behavior. (I want to change this)

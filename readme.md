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
| fib                   | 133.7940 ips      | 3.7371 ellapsed     | 275.3028 ips      | 1.8162 ellapsed | 2.06x faster |
| 10 mul                | 20511591.6855 ips | 4.8753 ellapsed     | 52514452.2058 ips | 1.9042 ellapsed | 2.56x faster |
| len (tuple)           | 34852350.3581 ips | 2.8692 ellapsed     | 54091580.2194 ips | 1.8487 ellapsed | 1.55x faster |
| len (string)          | 31940699.2351 ips | 3.1308 ellapsed     | 54639530.1764 ips | 1.8302 ellapsed | 1.71x faster |
| len (buffer)          | 34081650.7678 ips | 2.9341 ellapsed     | 46677356.6650 ips | 2.1424 ellapsed | 1.37x faster |
| get (tuple & number)  | 31564357.8538 ips | 3.1681 ellapsed     | 43652577.4165 ips | 2.2908 ellapsed | 1.38x faster |
| lerp                  | 29403841.7541 ips | 0.3401 ellapsed     | 52717344.2382 ips | 0.1897 ellapsed | 1.79x faster |
| fade                  | 14753615.0927 ips | 0.6778 ellapsed     | 53052320.1459 ips | 0.1885 ellapsed | 3.60x faster |
| dot                   | 25953221.8713 ips | 0.3853 ellapsed     | 42387474.3214 ips | 0.2359 ellapsed | 1.63x faster |
| bubble sort           | 592.9003 ips      | 1.6866 ellapsed     | 570.3864 ips      | 1.7532 ellapsed | 0.96x faster |
| perlin gradients      | 90912.4440 ips    | 1.1000 ellapsed     | 99288.3987 ips    | 1.0072 ellapsed | 1.09x faster |
| in                    | 27766886.1395 ips | 0.3601 ellapsed     | 49472367.0637 ips | 0.2021 ellapsed | 1.78x faster |
| get                   | 25971672.4005 ips | 0.3850 ellapsed     | 44639962.5211 ips | 0.2240 ellapsed | 1.72x faster |
| call                  | 28635941.7481 ips | 0.3492 ellapsed     | 23254623.6953 ips | 0.4300 ellapsed | 0.81x faster |
| C call                | 20817244.8125 ips | 0.4804 ellapsed     | 28961508.1460 ips | 0.3453 ellapsed | 1.39x faster |
| call jit              | 26487239.2779 ips | 0.0378 ellapsed     | 31945569.8608 ips | 0.0313 ellapsed | 1.21x faster |
| deep-not= (false)     | 1570453.9073 ips  | 0.0637 ellapsed     | 1539834.1703 ips  | 0.0649 ellapsed | 0.98x faster |
| cmp (numeric)         | 28668724.4061 ips | 0.3488 ellapsed     | 44701897.7729 ips | 0.2237 ellapsed | 1.56x faster |
| cmp (type difference) | 29217278.0043 ips | 0.3423 ellapsed     | 35475163.8519 ips | 0.2819 ellapsed | 1.21x faster |
| cmp (not numeric)     | 23711882.7925 ips | 0.0422 ellapsed     | 27398544.6314 ips | 0.0365 ellapsed | 1.16x faster |

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

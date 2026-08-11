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

------------------------------------------------------------------------------------------------------------------------------------
| name                              | interpreter ips   | interpreter elapsed | jit ips           | jit elapsed     | times faster |
|-----------------------------------|-------------------|---------------------|-------------------|-----------------|--------------|
| len (tuple)                       | 34240890.8491 ips | 2.9205 ellapsed     | 56693764.6445 ips | 1.7639 ellapsed | 1.66x faster |
| len (string)                      | 32251733.9902 ips | 3.1006 ellapsed     | 55523892.3161 ips | 1.8010 ellapsed | 1.72x faster |
| len (buffer)                      | 31282958.1885 ips | 3.1966 ellapsed     | 56602906.6222 ips | 1.7667 ellapsed | 1.81x faster |
| get (tuple & number)              | 33110004.1920 ips | 3.0202 ellapsed     | 57406100.1631 ips | 1.7420 ellapsed | 1.73x faster |
| get (tuple & number)              | 32348319.1143 ips | 3.0914 ellapsed     | 57923676.9431 ips | 1.7264 ellapsed | 1.79x faster |
| lerp                              | 28101883.9250 ips | 0.3558 ellapsed     | 49455386.3564 ips | 0.2022 ellapsed | 1.76x faster |
| fade                              | 14675185.7683 ips | 0.6814 ellapsed     | 39749205.4254 ips | 0.2516 ellapsed | 2.71x faster |
| dot                               | 26169521.6822 ips | 0.3821 ellapsed     | 47739693.5452 ips | 0.2095 ellapsed | 1.82x faster |
| bubble sort                       | 164326.4821 ips   | 0.0609 ellapsed     | 270486.3307 ips   | 0.0370 ellapsed | 1.65x faster |
| perlin gradients                  | 90512.6010 ips    | 1.1048 ellapsed     | 147985.4779 ips   | 0.6757 ellapsed | 1.63x faster |
| in                                | 26172565.0780 ips | 0.3821 ellapsed     | 42788382.6650 ips | 0.2337 ellapsed | 1.63x faster |
| get                               | 29058099.2683 ips | 0.3441 ellapsed     | 62917614.8530 ips | 0.1589 ellapsed | 2.17x faster |
| call                              | 25278975.2954 ips | 0.3956 ellapsed     | 22138527.4269 ips | 0.4517 ellapsed | 0.88x faster |
| C call                            | 22205791.1697 ips | 0.4503 ellapsed     | 36333494.6268 ips | 0.2752 ellapsed | 1.64x faster |
| call jit                          | 28491659.3465 ips | 0.0351 ellapsed     | 40356134.7105 ips | 0.0248 ellapsed | 1.42x faster |
| deep-not= (true)                  | 1624630.3861 ips  | 0.0616 ellapsed     | 1865690.6469 ips  | 0.0536 ellapsed | 1.15x faster |
| deep-not= (false type difference) | 1880226.8086 ips  | 0.0532 ellapsed     | 2251388.3616 ips  | 0.0444 ellapsed | 1.20x faster |
| cmp (numeric)                     | 31043180.1427 ips | 0.3221 ellapsed     | 44820904.7677 ips | 0.2231 ellapsed | 1.44x faster |
| cmp (type difference)             | 31486703.9636 ips | 0.3176 ellapsed     | 42384049.7211 ips | 0.2359 ellapsed | 1.35x faster |
| cmp (not numeric)                 | 24305963.2370 ips | 0.0411 ellapsed     | 30651891.5954 ips | 0.0326 ellapsed | 1.26x faster |

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

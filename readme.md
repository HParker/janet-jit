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

---------------------------------------------------------------------------------------
| name                               | interpreter ips   | interpreter elapsed | jit ips           | jit elapsed     | times faster |
|------------------------------------|-------------------|---------------------|-------------------|-----------------|--------------|
| lerp                               | 30645584.3206 ips | 0.3263 ellapsed     | 57815351.7667 ips | 0.1730 ellapsed | 1.89x faster |
| fade                               | 14727353.3183 ips | 0.6790 ellapsed     | 39557321.7580 ips | 0.2528 ellapsed | 2.69x faster |
| dot                                | 26685344.7691 ips | 0.3747 ellapsed     | 55041756.9395 ips | 0.1817 ellapsed | 2.06x faster |
| bubble sort                        | 154294.2862 ips   | 0.0648 ellapsed     | 263985.3640 ips   | 0.0379 ellapsed | 1.71x faster |
| perlin gradients                   | 85847.0050 ips    | 0.0116 ellapsed     | 147283.6267 ips   | 0.0068 ellapsed | 1.72x faster |
| in                                 | 29085615.8022 ips | 0.3438 ellapsed     | 50901189.2605 ips | 0.1965 ellapsed | 1.75x faster |
| get                                | 26764144.4628 ips | 0.3736 ellapsed     | 51039835.3351 ips | 0.1959 ellapsed | 1.91x faster |
| call                               | 26293972.5356 ips | 0.3803 ellapsed     | 22377352.9710 ips | 0.4469 ellapsed | 0.85x faster |
| C call                             | 21988227.7526 ips | 0.4548 ellapsed     | 36924371.8988 ips | 0.2708 ellapsed | 1.68x faster |
| call jit                           | 29741495.3617 ips | 0.3362 ellapsed     | 43852395.9750 ips | 0.2280 ellapsed | 1.47x faster |
| deep-not= (true)                   | 1621984.6661 ips  | 0.0617 ellapsed     | 1712031.9474 ips  | 0.0584 ellapsed | 1.06x faster |
| deep-not= (false type difference)  | 1842070.9666 ips  | 0.0543 ellapsed     | 1985943.7282 ips  | 0.0504 ellapsed | 1.08x faster |
| deep-not= (false value difference) | 1653813.0673 ips  | 0.0605 ellapsed     | 1770605.9890 ips  | 0.0565 ellapsed | 1.07x faster |
| deep-not= (deep array)             | 233428.1670 ips   | 0.4284 ellapsed     | 242980.8104 ips   | 0.4116 ellapsed | 1.04x faster |
| deep-not= (deep table)             | 221018.2660 ips   | 0.4525 ellapsed     | 223684.0382 ips   | 0.4471 ellapsed | 1.01x faster |
| cmp (numeric)                      | 30792105.5198 ips | 0.3248 ellapsed     | 49646474.1689 ips | 0.2014 ellapsed | 1.61x faster |
| cmp (type difference)              | 30296288.4630 ips | 0.3301 ellapsed     | 45241640.9573 ips | 0.2210 ellapsed | 1.49x faster |
| cmp (not numeric)                  | 26209991.7334 ips | 0.3815 ellapsed     | 34568671.4320 ips | 0.2893 ellapsed | 1.32x faster |

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

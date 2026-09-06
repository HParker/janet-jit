# (jit/jitable (fn []))

Small single file callable Jit for the janet programming language. You tell the jit what functions to compile
and it compiles them on next call. The jit makes a huge number of
assumptions, so the jit is not the right choice in all cases.

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
| 10 mul                             | 20329661.0839 ips | 4.9189 ellapsed     | 53346752.0759 ips | 1.8745 ellapsed | 2.62x faster |
| len (tuple)                        | 31750088.3389 ips | 3.1496 ellapsed     | 47266664.9534 ips | 2.1157 ellapsed | 1.49x faster |
| len (string)                       | 30390680.9501 ips | 3.2905 ellapsed     | 39894507.6078 ips | 2.5066 ellapsed | 1.31x faster |
| len (buffer)                       | 32874418.5158 ips | 3.0419 ellapsed     | 46192819.5047 ips | 2.1648 ellapsed | 1.41x faster |
| get (tuple & number)               | 32391191.0890 ips | 3.0873 ellapsed     | 43673945.0947 ips | 2.2897 ellapsed | 1.35x faster |
| get (tuple & number)               | 32509071.9792 ips | 3.0761 ellapsed     | 43605404.6743 ips | 2.2933 ellapsed | 1.34x faster |
| lerp                               | 29152278.9848 ips | 0.3430 ellapsed     | 50142773.0225 ips | 0.1994 ellapsed | 1.72x faster |
| fade                               | 15085604.2494 ips | 0.6629 ellapsed     | 47844517.8715 ips | 0.2090 ellapsed | 3.17x faster |
| dot                                | 26305710.7870 ips | 0.3801 ellapsed     | 51561939.5372 ips | 0.1939 ellapsed | 1.96x faster |
| bubble sort                        | 571.2923 ips      | 1.7504 ellapsed     | 560.3170 ips      | 1.7847 ellapsed | 0.98x faster |
| perlin gradients                   | 92158.3297 ips    | 1.0851 ellapsed     | 116208.7656 ips   | 0.8605 ellapsed | 1.26x faster |
| in                                 | 26507896.1734 ips | 0.3772 ellapsed     | 44973588.6973 ips | 0.2224 ellapsed | 1.70x faster |
| get                                | 26232817.6257 ips | 0.3812 ellapsed     | 42276238.7709 ips | 0.2365 ellapsed | 1.61x faster |
| call                               | 28975552.1708 ips | 0.3451 ellapsed     | 22824946.6396 ips | 0.4381 ellapsed | 0.79x faster |
| C call                             | 20711345.7506 ips | 0.4828 ellapsed     | 27138547.3866 ips | 0.3685 ellapsed | 1.31x faster |
| call jit                           | 27409171.6244 ips | 0.0365 ellapsed     | 33036918.3925 ips | 0.0303 ellapsed | 1.21x faster |
| deep-not= (true)                   | 1560792.7365 ips  | 0.0641 ellapsed     | 1603911.3109 ips  | 0.0623 ellapsed | 1.03x faster |
| deep-not= (false type difference)  | 1793042.2575 ips  | 0.0558 ellapsed     | 1845113.2592 ips  | 0.0542 ellapsed | 1.03x faster |
| deep-not= (false value difference) | 1612397.1930 ips  | 0.0620 ellapsed     | 1657832.2993 ips  | 0.0603 ellapsed | 1.03x faster |
| deep-not= (deep array)             | 216008.1660 ips   | 0.4629 ellapsed     | 218708.1494 ips   | 0.4572 ellapsed | 1.01x faster |
| deep-not= (deep table)             | 211008.2538 ips   | 0.4739 ellapsed     | 224888.4067 ips   | 0.4447 ellapsed | 1.07x faster |
| cmp (numeric)                      | 29822908.7644 ips | 0.3353 ellapsed     | 45275688.5796 ips | 0.2209 ellapsed | 1.52x faster |
| cmp (type difference)              | 30166166.3149 ips | 0.3315 ellapsed     | 38826019.9633 ips | 0.2576 ellapsed | 1.29x faster |
| cmp (not numeric)                  | 24058534.2181 ips | 0.0416 ellapsed     | 27807994.2534 ips | 0.0360 ellapsed | 1.16x faster |


## Notable Differences from Janet interpreter

Jit generated code has less strict garantees about correctness and errors than the interpreter. It expects that you to pass it correct numeric focused code.

Defining your own operators is supported, but will likely not perform better than the interpreter

```janet
(def addable
  @{
    :+ (fn [x y] (print x " " y))
   })
(+ addable 10)) # prints #<table> 10
```

works, but will end up doing nearly the same work as in the interpreter.

There are also a number of operations the VM can do that the JIT today does not support. They mostly have to do with VM state and probably make bad candidates for the JIT anyways. Today the list of unsupported ops is:

- (ldu) load up value
- (setu) set up value
- (res) resume
- (sig) signal
- (prop) propagate
- (clo) closure

Operations that always fall back to the interpreter:

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

All other operations can also fall back to the VM when runtime type information isn't provable. I want to add information about when these cases are compiled, but haven't added that yet.

## Other things

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a ~10% performance penalty. If you need to leave the JIT a lot, you won't have good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

- Bounds checks don't currently happen on binary shift type operterations which does not match the interpreter's behavior. (I want to change this)

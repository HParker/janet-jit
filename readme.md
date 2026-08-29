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
| name                              | interpreter ips   | interpreter elapsed | jit ips           | jit elapsed     | times faster |
|-----------------------------------|-------------------|---------------------|-------------------|-----------------|--------------|
| len (tuple)                       | 34014509.4530 ips | 2.9399 ellapsed     | 48883811.8011 ips | 2.0457 ellapsed | 1.44x faster |
| len (string)                      | 33528732.5166 ips | 2.9825 ellapsed     | 48542028.6646 ips | 2.0601 ellapsed | 1.45x faster |
| len (buffer)                      | 32793682.0859 ips | 3.0494 ellapsed     | 48051715.8168 ips | 2.0811 ellapsed | 1.47x faster |
| get (tuple & number)              | 32494780.1848 ips | 3.0774 ellapsed     | 45999537.4149 ips | 2.1739 ellapsed | 1.42x faster |
| get (tuple & number)              | 32612811.5521 ips | 3.0663 ellapsed     | 46285487.2208 ips | 2.1605 ellapsed | 1.42x faster |
| lerp                              | 28074388.3705 ips | 0.3562 ellapsed     | 35665951.7269 ips | 0.2804 ellapsed | 1.27x faster |
| fade                              | 14890030.9347 ips | 0.6716 ellapsed     | 17487797.8802 ips | 0.5718 ellapsed | 1.17x faster |
| dot                               | 26127761.3827 ips | 0.3827 ellapsed     | 33357779.8043 ips | 0.2998 ellapsed | 1.28x faster |
| bubble sort                       | 162506.4527 ips   | 0.0615 ellapsed     | 200137.6026 ips   | 0.0500 ellapsed | 1.23x faster |
| perlin gradients                  | 89550.0136 ips    | 1.1167 ellapsed     | 99798.8042 ips    | 1.0020 ellapsed | 1.11x faster |
| in                                | 28421756.5158 ips | 0.3518 ellapsed     | 43457819.6335 ips | 0.2301 ellapsed | 1.53x faster |
| get                               | 26465289.7362 ips | 0.3779 ellapsed     | 46439781.0310 ips | 0.2153 ellapsed | 1.75x faster |
| call                              | 26120288.7359 ips | 0.3828 ellapsed     | 22709359.7391 ips | 0.4403 ellapsed | 0.87x faster |
| C call                            | 20907056.4055 ips | 0.4783 ellapsed     | 29459941.5883 ips | 0.3394 ellapsed | 1.41x faster |
| call jit                          | 27184281.4810 ips | 0.0368 ellapsed     | 36117977.6370 ips | 0.0277 ellapsed | 1.33x faster |
| deep-not= (true)                  | 1568892.7722 ips  | 0.0637 ellapsed     | 1615307.5069 ips  | 0.0619 ellapsed | 1.03x faster |
| deep-not= (false type difference) | 1784871.7944 ips  | 0.0560 ellapsed     | 1850493.7951 ips  | 0.0540 ellapsed | 1.04x faster |
| cmp (numeric)                     | 29009846.6763 ips | 0.3447 ellapsed     | 48313706.6952 ips | 0.2070 ellapsed | 1.67x faster |
| cmp (type difference)             | 29277705.8797 ips | 0.3416 ellapsed     | 46933024.6921 ips | 0.2131 ellapsed | 1.60x faster |
| cmp (not numeric)                 | 24027096.0296 ips | 0.0416 ellapsed     | 46890745.0388 ips | 0.0213 ellapsed | 1.95x faster |


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

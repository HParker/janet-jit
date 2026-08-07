# (janet-jit (fn []))

Small callable Janet Jit. You tell the jit what functions to compile and it compiles them.
The jit makes a huge number of assumptions to get good performance, so the jit is not the
right choice in all cases.

Right now it only target System V x86 with nanboxed janet values. I would love to support ARM and Janet without nanboxed types.

```janet
(defn lerp
  [a b t]
  (+ a (* t (- b a))))
(def lerp-jit (jit/jitable lerp))

(lerp-jit 1 20 0.3) # call just like usual, but will compile to binary on first call.
```

## Notable gaps

Jit generated code has less strict garantees about correctness and errors than the interpreter. It expects that you basically pass it correct expected code.

In Janet `(+ "hi" 1)` raises an error, under the JIT, this should be treated as undefined behavior. On my machine this returns "hi", but that may change at any time. This leads to a more useful JIT that doesn't have to generate much error checking.

There are also a number of operations the VM can do that the JIT today does not support. They mostly have to do with VM state and probably make bad candidates for the JIT anyways. Today the list of unsupported ops is:

- (ldu) load up value
- (setu) set up value
- (res) resume
- (sig) signal
- (prop) propagate
- (clo) closure

Operations that fall back to the interpreter, but return to the JIT:

- push
- call
- in
- get
- getindex
- set
- setindex
- length


## Other things

- Calling out of the JIT uses `janet_call` which means GC will not run. This can be a benefit, or a mistake depending on the application. Calls also have a about 10% performance penalty. If you need to leave the JIT a lot, you won't have very good JIT performance.

- `tailcalls` are just normal calls, so you can get stack depth issues you otherwise wouldn't get.

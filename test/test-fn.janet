(import /build/jit :as jit)

(defn jit-result-deep-matches [f & args]
  (let [jit-f (jit/jitable f)]
    (let [expected (f ;args)
	  actual (jit-f ;args)]
      (do
	(if (not (jit/compiled? jit-f))
	  (error "function did not compile"))
	(if (or (deep= expected actual) (and (nan? expected) (nan? actual)))
	  (do
	    true)
	  (do
	    (print "x (deep) expected " expected " actual: " actual " args: " (string/format "%p" args))
	    (pp (disasm f))
	    false))))))

(defn jit-result-matches [f & args]
  (let [jit-f (jit/jitable f)]
    (let [expected (f ;args)
	  actual (jit-f ;args)]
      (do
	(if (not (jit/compiled? jit-f))
	  (error "function did not compile"))
	(if (or (= expected actual) (and (nan? expected) (nan? actual)))
	  (do
	    true)
	  (do
	    (print "x expected " expected " actual: " actual " args: " (string/format "%p" args))
	    (pp (disasm f))
	    false))))))

# from https://github.com/sogaiu/jref/blob/master/data/asm.janet
(def move-far-test (asm ~{:bytecode @[(movf 0 1) (ret 1)] :arity 1}))
(if (jit-result-matches move-far-test 10)
    (print ". movf pass"))

(def move-near-test (asm ~{:bytecode @[(movn 1 0) (ret 1)] :arity 1}))
(if (jit-result-matches move-near-test 10)
  (print ". movn pass"))

(def jmp-test (asm ~{:bytecode @[(jmp 3)
                      (ldi 0 0x8)
                      (ret 0)
                      (ldi 0 0x9)
                      (ret 0)]
          :arity 0}))
(if (jit-result-matches jmp-test)
  (print ". jmp pass"))

(def jmp-test (asm ~{:bytecode @[(jmp 3)
                      (ldi 0 0x8)
                      (ret 0)
	              (jmp -2)
                      (ldi 0 0x9)
                      (ret 0)]
          :arity 0}))
(if (jit-result-matches jmp-test)
  (print ". jmp back pass"))

  # movn: $dest = $src
  ((asm ~{:bytecode @[(movn 1 0)  # (movn dest src)
                      (ret 1)]
          :arity 1})
    :again)

(def rng (math/rng))

(def number-edges
  [-2147483648
    -1000000
    -255
    -2
    -1
    -0.0
    0
    1
    2
    255
    1000000
    2147483647])

(def number-ints (array/join (range 0 10) (range 400 420) (range 1000 1010)))
(def basic-decimals (map (fn [x] (* (math/rng-int rng 1000) (- (math/random) 0.5))) (range 0 10)))

(var ret (fn  [] 1))
(if (jit-result-matches ret)
  (print ". ret passed"))

(var ret-nil (fn  []))
(jit-result-matches ret-nil)

(var test-err-jit (jit/jitable (fn [] (error "hi"))))
(try (do
       (test-err-jit)
       (prin "x"))
     ([e] (print ". error passed")))

(def kw-method
  @{ :hi (fn [self x] (+ x x)) })

(defn copy-deep-not=
  ``Like `not=`, but mutable types (arrays, tables, buffers) are considered
  equal if they have identical structure. Much slower than `not=`.``
  [x y]
  (def tx (type x))
  (or
    (not= tx (type y))
    (cond
      (or (= tx :tuple) (= tx :array))
      (or (not= (length x) (length y))
          (do
            (var ret false)
            (forv i 0 (length x)
              (def xx (in x i))
              (def yy (in y i))
              (if (deep-not= xx yy)
                (break (set ret true))))
            ret))
      (or (= tx :struct) (= tx :table))
      (or (not= (length x) (length y))
          (do
            (def rawget (if (= tx :struct) struct/rawget table/rawget))
            (var ret false)
            (eachp [k v] x
              (if (deep-not= (rawget y k) v) (break (set ret true))))
            ret))
      (= tx :buffer) (not= 0 (- (length x) (length y)) (memcmp x y))
      (not= x y))))

(def deep-not-eq-tests
  [[{ :x 123 :y 321 } { :x 123 :y 321 }]
   [{ :x 123 :y 321 } { :x 123 :y 123 }]
   [{ :x :the-x :y :the-y } { :x :the-x :y :the-y }]
   [{ :x :the-x :y :the-y } { :x :the-x :y :the-x }]
   [{ :x "the-x" :y "the-y" } { :x "the-x" :y "the-y" }]
   [{ :x "the-x" :y "the-y" } { :x "the-x" :y "the-x" }]
  ])


(var pass-count 0)
(each [lhs rhs] deep-not-eq-tests
    (if (jit-result-deep-matches copy-deep-not= lhs rhs)
      (++ pass-count)))
(print pass-count " copy-deep-not= passed")

# Tiny tests too small to name
(def tests
  [(fn [x] x)
   (fn [x])
   (fn [x] true)
   (fn [x] false)
   (fn [x] nil)
   (fn [x] (length [1 2 3]))
   (fn [x] (length @[1 2 3]))
   (fn [x] (length "1 2 3"))
   (fn [x] (length @"1 2 3"))
   (fn [x] (length { :one 1 :two 2 :three 3 }))
   (fn [x] (length { :one 1 :two 2 :three 3 }))
   (fn [x] (- x))
   (fn [x] (+ x))
   (fn [x] (+ x 321))
   (fn [x] (- x 321))
   (fn [x] (* x 321))
   (fn [x] (/ x 321))
   (fn [x] (+ x x))
   (fn [x] (- x x))
   (fn [x] (* x x))
   (fn [x] (% x x))
   (fn [x] (if x 100 -100))
   (fn [x] (if (not x) 100 -100))
   (fn [x] (:hi kw-method x))
   # (fn [x] (mod x x)) # we don't match the interpreter here exactly
   (fn [x] (+ (+ x x) (- x x)  (* x x) (/ x x) x))
   ])

(def integer-only-tests
  [(fn [x] (% x x))
   (fn [x] (% x 123))
   (fn [x] (blshift x x))
   (fn [x] (blshift x 3))
   (fn [x] (brshift x x))
   (fn [x] (brshift x 3))
   (fn [x] (brushift x x))
   (fn [x] (brushift x 3))
   (fn [x] (band x x))
   (fn [x] (band x 3))
   (fn [x] (bor x x))
   (fn [x] (bor x 3))
   (fn [x] (bxor x x))
   (fn [x] (bxor x 3))
   (fn [x] (bnot x))
   ])

(def binary-integer-tests
  [(fn [x y] (% x y))
   (fn [x y] (blshift x y))
   (fn [x y] (brshift x y))
   (fn [x y] (brushift x y))
   (fn [x y] (band x y))
   (fn [x y] (bor x y))
   (fn [x y] (bxor x y))
   ])

(def binary-tests
  [(fn [x y] x)
   (fn [x y] y)
   (fn [x y] (- y))
   (fn [x y] (+ y))
   (fn [x y] (+ y 7.654))
   (fn [x y] (- y 7.654))
   (fn [x y] (* y 7.654))
   (fn [x y] (/ y 7.654))
   (fn [x y] (+ y x))
   (fn [x y] (- y x))
   (fn [x y] (* y x))
   (fn [x y] (div x 123))
   (fn [x y] (div x y))

   (fn [x y] (+ (+ x y) (- y x)  (* x y) (/ y x) x))
   (fn [x y] (cmp x y))
   (fn [x y] (= x y))
   (fn [x y] (= x 123))
   (fn [x y] (> x y))
   (fn [x y] (> x 123))
   (fn [x y] (< x y))
   (fn [x y] (< x 123))
   (fn [x y] (>= x y))
   (fn [x y] (>= x 123))
   (fn [x y] (<= x y))
   (fn [x y] (<= x 123))
   ])

(def deep-equality-tests
  [(fn [x y] [x y])
   (fn [x y] @[x y])
   (fn [x y] "x y")
   (fn [x y] @"x y")
   (fn [x y] { :x x :y y })
   (fn [x y] @{ :x x :y y })])

(var pass-count 0)
(each bn number-ints
  (each t deep-equality-tests
    (if (jit-result-deep-matches t bn bn)
      (++ pass-count))))
(print pass-count " deep equality passed")

(var pass-count 0)
(each bn number-ints
  (do
    (each t integer-only-tests
      (do
	(if (jit-result-matches t bn)
	  (++ pass-count))))
    (each t tests
      (if (jit-result-matches t bn)
	(++ pass-count)))))
(print pass-count " integers passed")

(var pass-count 0)
(each lhs number-ints
  (each rhs number-ints
    (do
      (each bit binary-tests
	(if (jit-result-matches bit lhs rhs)
	  (++ pass-count)))
      (each bit binary-integer-tests
	(if (jit-result-matches bit lhs rhs)
	  (++ pass-count))))))
(print pass-count " binary passed")

(var pass-count 0)
(each lhs basic-decimals
  (each rhs basic-decimals
    (do
      (each bit binary-tests
	(if (jit-result-matches bit lhs rhs)
	  (++ pass-count))))))
(print pass-count " binary decimals passed")

(var pass-count 0)
(each bn basic-decimals
  (each t tests
    (if (jit-result-matches t bn)
      (++ pass-count))))
(print pass-count " decimals passed")

(var pass-count 0)
(each bn number-edges
  (each t tests
    (if (jit-result-matches t bn)
      (++ pass-count))))
(print pass-count " edges passed")

(print "success")
